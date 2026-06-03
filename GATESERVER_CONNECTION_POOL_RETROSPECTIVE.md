# GateServer 启动日志刷屏问题复盘

## 背景

本项目采用 `GateServer + GameServer` 的服务拆分：

- `GateServer` 监听客户端连接，负责登录、客户端会话管理和消息转发。
- `GameServer` 监听后端连接，负责房间、战斗和游戏逻辑。
- `GateServer` 内部通过 `GameServerConnPool` 维护到 `GameServer` 的长连接池。

这次问题发生在 `GateServer` 启动后。服务可以启动，但日志持续打印大量 `info`，主要内容类似：

```text
GameServerConnPool rebuild slot 0 success
GameServerConnPool rebuild slot 1 success
...
```

日志量很大，并且持续增长，说明不是单次启动日志，而是后台连接池在反复重建连接。

## 影响

这个问题的影响不只是日志刷屏：

- `GateServer` 日志被重复连接池信息淹没，真正错误难以观察。
- 大量连接处于未完成状态，可能消耗文件描述符和内核 TCP 资源。
- 服务表面上启动成功，但 `GateServer -> GameServer` 后端链路并不稳定。
- 如果继续运行，连接池可能持续创建新连接，带来额外 CPU、内存和网络压力。

## 定位过程

### 1. 先确认日志来源

从日志内容定位到 `GateServer/source/GameServerConnPool.cc`：

```cpp
spdlog::info("GameServerConnPool rebuild slot {} success", i);
```

这说明刷屏来自 `GateServer` 到 `GameServer` 的后端连接池，而不是外部客户端连接。

因此，外部客户端是否连接 `GateServer:8888`，不是直接原因。

### 2. 检查连接状态

使用 `ss -tnp` 查看 TCP 状态时，发现大量连接卡在：

```text
SYN-SENT 10.2.0.17:xxxxx -> 49.232.172.156:50051
```

含义是：

- `GateServer` 从服务器内网地址 `10.2.0.17` 发起连接。
- 目标是服务器公网地址 `49.232.172.156:50051`。
- 连接没有建立完成，一直停留在 `SYN-SENT`。

这说明 `GateServer` 没有真正连上本机的 `GameServer`。

### 3. 检查配置

原配置里 `GameServer.host` 使用了公网 IP：

```ini
[GameServer]
host=49.232.172.156
port=50051
```

但 `GateServer` 和 `GameServer` 实际部署在同一台服务器上。

同机服务之间用公网 IP 回连自己，依赖云厂商网络是否支持 NAT loopback/hairpin。当前服务器环境下，这条路径不稳定或不可用，导致 TCP 连接一直卡在 `SYN-SENT`。

### 4. 检查连接池状态判断

`GameServerConnPool` 每秒执行一次 detection：

```cpp
timer_.expires_after(LINK_DETECTION_TIME);
```

原逻辑只把这些状态视为可用：

```cpp
Connected
Busy
```

但异步连接刚创建时状态是：

```cpp
Connecting
```

原逻辑没有把 `Connecting` 当作“正在连接中”，而是当作不可用连接。结果就是：

1. 创建连接对象。
2. 异步连接还没完成，状态仍是 `Connecting`。
3. 下一轮 detection 认为它不可用。
4. 再次创建新连接。
5. 重复以上过程。

这导致连接池每秒不断重建连接。

### 5. 检查日志语义

原日志：

```cpp
GameServerConnPool rebuild slot {} success
```

这里的 `success` 只代表创建了一个 `ClientSession` 对象并启动异步连接，不代表 TCP 连接已经成功。

真正连接成功发生在 `ClientSession::handleconnect()` 内部：

```cpp
co_await socket_.async_connect(...);
state_ = ClientSession_state::Connected;
```

所以原日志文案会误导排查方向。

### 6. 继续观察空闲连接

改成 `127.0.0.1` 后，连接可以建立，`ss` 中状态变为 `ESTAB`。

但继续观察超过 15 秒后，连接又出现断开重连。

原因是 `ClientSession::keep_alive()` 中原本有逻辑：

```cpp
if (now - last_recv_time_ > std::chrono::seconds(15)) {
    state_=ClientSession_state::Timeout;
    close();
    co_return;
}
```

当前 `GateServer -> GameServer` 后端连接还没有实现真正的 `Ping/Pong` 心跳。没有客户端消息时，后端链路长时间收不到数据，于是被误判为超时，进入断开重连循环。

`GameServer` 侧也有类似的空闲 heartbeat 关闭逻辑。

## 根因总结

本次问题不是单一原因，而是多个问题叠加：

### 1. 内部服务连接使用了公网 IP

`GateServer` 和 `GameServer` 在同一台服务器上，内部通信应该使用：

```ini
GameServer.host=127.0.0.1
```

原来使用公网 IP `49.232.172.156`，导致同机回连公网地址失败或不稳定。

### 2. 连接池没有正确处理 Connecting 状态

异步连接处于 `Connecting` 时，应该视为“已有连接尝试正在进行”，不应该每秒重建。

原逻辑只认可 `Connected` 和 `Busy`，导致 pending 连接被反复替换。

### 3. 连接池重建旧连接前没有主动关闭旧 session

反复重建时，旧连接对象没有明确关闭，容易堆积未完成连接和文件描述符。

### 4. 空闲后端连接没有完整心跳协议

项目协议中已经定义了 `PingReq`、`PongRsp`、`GateLinkHello`，但后端链路当前没有完整实现心跳处理。

因此，不能把“15 秒没收到 GameServer 数据”直接当成连接失效。

### 5. 日志级别和文案不准确

连接池的 `success` 日志并不代表连接成功，而且使用 `info` 打在高频路径上，导致正常排查时噪音很大。

## 修复内容

### 1. 拆分公网地址和内部连接地址

当前配置：

```ini
[GateServer]
port = 8888
host=49.232.172.156

[GameServer]
port=50051
host=127.0.0.1
listen_ip=0.0.0.0
logic_shards=8
gateway_link_count=64
```

含义：

- 外部客户端连接 `49.232.172.156:8888`。
- `GateServer` 内部连接 `GameServer` 时走 `127.0.0.1:50051`。
- `GameServer` 仍监听 `0.0.0.0:50051`。

### 2. 连接池增加 pending 状态判断

新增逻辑：

```cpp
bool GameServerConnPool::IsConnPendingOrAvailable(const ConnPtr& conn) const {
    if (!conn) {
        return false;
    }

    auto state = conn->get_state();
    return state == ClientSession_state::Connecting ||
           state == ClientSession_state::Connected ||
           state == ClientSession_state::Busy;
}
```

detection 中如果连接处于 `Connecting`、`Connected`、`Busy`，都不会立刻重建：

```cpp
if (IsConnPendingOrAvailable(sessions_[i])) {
    continue;
}
```

### 3. 重建连接前先关闭旧 session

```cpp
if (sessions_[i]) {
    sessions_[i]->close();
}
```

这样可以避免失败连接对象和 socket 资源持续堆积。

### 4. 给异步 connect 增加连接超时

`ClientSession::handleconnect()` 中给 `async_connect` 配置了 5 秒超时。

如果连接一直无法完成，会主动关闭，而不是无限停留在 `Connecting`：

```cpp
timer_.expires_after(std::chrono::seconds(5));
timer_.async_wait(...);
```

### 5. 实现后端握手和心跳闭环

后续补齐了 `GateServer -> GameServer` 后端链路的业务层握手和心跳闭环：

- `GateServer` 后端连接建立成功后发送 `GateLinkHello`。
- `GameServer` 收到 `GateLinkHello` 后解析 `gate_id` 和 `link_index`。
- `GateServer` 每 5 秒发送一次 `PingReq`。
- `GameServer` 收到 `PingReq` 后回复 `PongRsp`。
- `GateServer` 收到 `PongRsp` 后刷新 `last_pong_time_`。
- 如果 15 秒内没有收到 `PongRsp`，`GateServer` 才认为后端连接超时并关闭连接。

```cpp
if (now - last_pong_time_ > kBackendHeartbeatTimeout) {
    state_=ClientSession_state::Timeout;
    close();
    co_return;
}
```

这样空闲连接不会被误杀，真正的异常连接也能通过心跳超时被连接池重建。

### 6. 降低高频日志级别

这些日志从 `info` 调整为 `debug`：

- `GateServer` 到 `GameServer` 的每条连接成功。
- `GameServer` 每个 gateway link slot accept 成功。
- `GateServer` 收到的后端心跳 `PongRsp`。
- 连接池 rebuild scheduled。
- GameServer 没有空闲 slot 时的 accept retry。

这样正常启动时不会输出几十条重复 `info`。

### 7. 移除 `MsgNode` 析构噪音

原来 `MsgNode` 析构时直接输出：

```cpp
std::cout << "destruct MsgNode" << endl;
```

这会在连接断开或对象释放时污染日志。已经删除。

## 涉及文件

- `config/config.ini`
- `GateServer/include/GameServerConnPool.h`
- `GateServer/source/GameServerConnPool.cc`
- `GateServer/source/ClientSession.cc`
- `GateServer/include/GateProtocol.h`
- `GateServer/source/GateProtocol.cc`
- `GateServer/include/MsgNode.h`
- `GameServer/source/GatewayLinkSession.cc`
- `GameServer/source/NetworkShard.cc`
- `GameServer/include/NetworkShard.h`
- `GameServer/include/MsgNode.h`

## 验证结果

### 编译

```bash
cmake --build --preset linux-debug --parallel 1
```

结果：

```text
[100%] Built target GateServer
```

### 服务状态

重启后确认：

```text
GameServer listening 0.0.0.0:50051
GateServer listening 0.0.0.0:8888
```

### TCP 连接状态

`GateServer -> GameServer` 后端连接状态稳定为 `ESTAB`：

```text
127.0.0.1:xxxxx -> 127.0.0.1:50051 ESTAB
```

观察超过心跳超时时间后，连接数量稳定，没有继续断开重连。

### 日志结果

`GateServer.log` 最终只保留启动信息：

```text
loaded config from config/config.ini
GateServer listening on 0.0.0.0:8888
```

没有再持续打印：

```text
GameServerConnPool rebuild slot ... success
```

## 经验教训

### 1. 配置字段要区分外部地址和内部地址

`host` 这个字段语义太模糊。它既可能表示公网展示地址，也可能表示内部连接地址。

更清晰的配置应该类似：

```ini
[GateServer]
listen_ip=0.0.0.0
public_host=49.232.172.156
port=8888

[GameServer]
listen_ip=0.0.0.0
backend_host=127.0.0.1
port=50051
```

### 2. 异步连接状态机必须区分 pending 和 failed

`Connecting` 不是失败，而是“正在进行中”。

连接池不能只关心可用状态，也要正确处理进行中状态，否则会造成重复建连。

### 3. 日志要准确表达真实状态

“创建连接对象成功”和“TCP 连接成功”不是一回事。

高频路径不要轻易使用 `info`，否则线上排查时日志会失去信号。

### 4. 心跳协议不能只写超时关闭

如果要判断连接空闲超时，就必须同时实现主动心跳和心跳响应。

只有关闭逻辑、没有 Ping/Pong，会导致正常空闲连接被误杀。

### 5. 服务端内部连接优先使用回环地址

同机进程通信使用 `127.0.0.1` 更直接、更稳定，也不会依赖云厂商 NAT 回环能力。

## 后续优化建议

### 1. 给 GateLinkHello 增加 ACK

当前 `GateServer` 已经会发送 `GateLinkHello`，`GameServer` 也会解析。下一步可以增加明确的 Hello ACK，让 `GateServer` 只有在收到 ACK 后才把后端连接标记为完全 ready。

```proto
message GateLinkHello {
  uint32 gate_id = 1;
  uint32 link_index = 2;
}
```

### 2. 让心跳超时支持连续失败次数

当前心跳已经使用：

```proto
message PingReq {
  uint64 client_time_ms = 1;
}

message PongRsp {
  uint64 client_time_ms = 1;
  uint64 server_time_ms = 2;
}
```

后续可以把“15 秒未收到 Pong”改成“连续 N 次 Ping 没收到 Pong”，这样对偶发网络抖动更友好。

### 3. 给连接池增加退避重连

当前检测间隔是 1 秒。真实线上环境建议使用指数退避：

```text
1s -> 2s -> 4s -> 8s -> max 30s
```

避免后端不可用时连接池持续打满日志和连接资源。

### 4. 把连接数量改成配置项

当前默认：

```cpp
WORK_SHARD_NUMBER=8
GAMESERVER_CONN_CNT=8
```

一启动就是 64 条后端连接。对小服务器来说偏重。

建议放到 `config.ini`，根据服务器规格调整。

### 5. 增加运行状态指标

建议输出或统计：

- 当前连接池总连接数。
- `Connecting` 数量。
- `Connected` 数量。
- 重连次数。
- 最近一次连接失败原因。

这样后续排查会更快。

## 面试可讲点

这次问题可以作为一个真实工程排障案例来讲：

> 我在项目中遇到过 GateServer 启动后连接池日志持续刷屏的问题。通过日志定位到后端连接池，再用 `ss` 查看 TCP 状态，发现连接卡在 `SYN-SENT`，原因是同机服务用公网 IP 回连自己。同时，连接池把 `Connecting` 状态误判为失败，导致每秒重复建连。后续我拆分了公网地址和内部连接地址，将后端连接改为 `127.0.0.1`，修复连接池状态判断，增加连接超时，关闭前释放旧 session，并把高频日志降级。最终服务启动后后端连接稳定为 `ESTAB`，日志不再刷屏。
