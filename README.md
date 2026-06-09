# RTS 游戏服务器后端 — 开发文档

C++20 实现的权威状态同步(state-sync)RTS 游戏服务器。两进程:**GateServer**(网关,面向客户端)+ **GameServer**(逻辑)。并发模型是**分片 actor**:每个 shard 独占一个线程 + `io_context`,shard 之间只用 `boost::asio::post` 投递消息,因此**没有业务锁**。

技术栈:C++20 · Boost.Asio(协程)· Protobuf · CMake · Catch2。
性能数据与压测方法见 [PERF_REPORT.md](PERF_REPORT.md)。

---

## 1. 架构总览

```
客户端(Unity) ──TCP/8888──> GateServer ──内部长连接/50051──> GameServer
                              (网关进程)                      (逻辑进程)
```

### GateServer(`GateServer/`)
| 组件 | 职责 |
|---|---|
| `Cserver` | 监听 8888,accept 后把 `Csession` 轮询分配给某个 `WorkShard` |
| `WorkShard` | 一线程一 `io_context`;持有本片的客户端 `Csession` + 一个 `GameServerConnPool` |
| `Csession` | 一条客户端连接:收发包、登录态、心跳;登录后注册 `uid → session` |
| `GameServerConnPool` | 本片到 GameServer 的长连接池(`ClientSession`),带健康检测/重连 |
| `ClientSession` | 网关→游戏服的一条内部连接 |
| `ClientIngressRouter` / `BackendIngressRouter` | 按 `msg_id` 分发:客户端上行 / 游戏服下行 |
| `GateProtocol` | 组包、`GateToGameEnvelope` 封装、`uid` 注入 |

### GameServer(`GameServer/`)
| 组件 | 职责 |
|---|---|
| `GameServer` | 持有 `vector<NetworkShard>` + `vector<LogicShard>`;`PostToLogic` / `PostToNetwork` |
| `NetworkShard` | 一线程一 `io_context` + 一个 acceptor(`SO_REUSEPORT`)+ 自己那批链路 `slots_` |
| `GatewayLinkSession` | 一条网关链路;收包回调 `OnPacket`,发包 `PostSend` |
| `LogicShard` | 一线程一 `io_context`;持有 `rooms_`、`uid_route_`,跑 20Hz tick |
| `LogicRouter` | 按 `msg_id` 把 `LogicTask` 分发到 `HandleXxx` |
| `DungeonRoom` | 房间世界状态 + 仿真(**纯逻辑,不依赖 proto,可单测**) |
| `Metrics` / `MetricsServer` | 指标注册表 + Prometheus `/metrics` 端点 |

`common/`:`Protocol.h`(包头常量、`MsgId` 枚举)、`ProtoCodec.h`(protobuf 编解码)。
`proto/rts.proto`:全部协议定义。

---

## 2. 消息流转(读这一节就懂整个系统)

### 上行:客户端指令 → 生效

```
Csession.handle_read                         (网关, 客户端线程片)
  └─ ClientIngressRouter::HandleMsg(msg_id)
       ├─ LoginReq  → HandleLoginReq → BindUid(uid)      // 登录在网关本地处理
       └─ 其它      → ForwardClientMsgToGame
              └─ GateProtocol::BuildGateToGameEnvelope    // 注入 uid, 包成 envelope
                   └─ WorkShard::PostMessage → GameServerConnPool → ClientSession::SendData
                                                                          │ 内部连接
GatewayLinkSession.handle_read ──> NetworkShard::OnPacket                 ▼ (游戏服, 网络 shard 线程)
  └─ 解 GateToGameEnvelope, 取 uid / inner_msg_id
  └─ ResolveLogicShard(msg_id, envelope)                 // 按 room_id % shard_count 选逻辑 shard
  └─ 组 LogicTask{msg_id, uid, seq, origin=MakeRoute(link), body}
  └─ GameServer::PostToLogic(shard) ── asio::post ──>
LogicShard::handleTask                                    (游戏服, 逻辑 shard 线程)
  └─ uid_route_[uid] = task.origin                        // 记回包坐标
  └─ LogicRouter::Dispatch(msg_id) → HandleCreateRoom / HandlePlayerCommand / ...
       └─ 解析意图, 入队到 room.pending_(不立即改世界)
```

### tick:固定步长推进(每个 LogicShard,20Hz)

```
LogicShard::TickLoop  (绝对时刻调度, 防累积漂移)
  └─ TickRooms: 遍历 started 房间
       ├─ room.ApplyPending()   // 把本帧攒下的意图统一应用
       ├─ room.Step(dt)         // 推进: 移动/采集/建造/战斗/死亡
       ├─ server_tick % N == 0 ? SendFullSnapshot : SendDelta
       └─ room.ClearFrameChanges()  // 清 dirty/spawned/despawned
```

### 下行:服务器状态 → 客户端

```
LogicShard::SendToPlayers(uids, msg)                      (逻辑 shard 线程)
  └─ 按 uid_route_ 把目标分组到 {net_shard → [link, uids]}
  └─ 对每个 net_shard: GameServer::PostToNetwork(net_shard, NetworkTask) ── asio::post ──>
NetworkShard::HandleNetworkTask                          (网络 shard 线程)
  └─ 对每个 link: 校验 slots_[link].generation == route.generation
  └─ BuildGameToGatewayEnvelope → slots_[link].session->PostSend
                                                          │ 内部连接
ClientSession.HandleRead ──> BackendIngressRouter         ▼ (网关)
  └─ ForwardGameToClients: 解 GameToGateEnvelope
  └─ WorkShard::SendToUid(uid) → Csession::SendData → 客户端
```

**关键点**:回包不靠 uid 在网络层查表,而是逻辑层存的"路由坐标" `{net_shard, link, generation}`——网络 shard 用 `link` 在自己的 `slots_` 里直接拿 session,用 `generation` 防止 slot 被复用后串台。

---

## 3. 协议(`common/Protocol.h` + `proto/rts.proto`)

包头 **10 字节,全大端**:`magic(2, 0x5254 "RT") | msg_id(2) | flags(2) | body_len(4)`,后接 protobuf body。

- 客户端 ↔ 网关:裸消息(`msg_id` = 具体消息,如 `MoveCmd`),网关负责注入 `uid` 并封装。
- 网关 ↔ 游戏服:房间/玩法消息包在 `GateToGameEnvelope{uid, room_id, inner_msg_id, payload}`;回包包在 `GameToGateEnvelope{target_uids, inner_msg_id, payload}`。`GateLinkHello` / `PingReq` 是裸包。
- `RoomRouteHint{room_id}`:仅供网络层在内层 payload 里快速取 `room_id` 做路由,无需关心具体消息类型。

---

## 4. 并发模型(无锁的来由)

- **一切重状态都属于某个单线程 shard**:`LogicShard` 的 `rooms_`/`uid_route_`、`NetworkShard` 的 `slots_`,都只在各自线程访问。
- **跨线程只通过 `asio::post` 投消息**(actor 信箱):上行 `PostToLogic`、下行 `PostToNetwork`,都是把任务投到目标 shard 的 `io_context`,由它自己的线程取出执行。
- **分片键**:逻辑按 `room_id`(房间 id 生成时落在 `room_id % shard_count == shard_id` 的同余类,保证房间与成员同 shard);网络用 `SO_REUSEPORT`(内核把连接分到各 acceptor)。
- 因此**没有任何业务锁**,也不需要 strand——单 shard 单线程本身就是串行的。

---

## 5. 游戏逻辑(`DungeonRoom`,纯逻辑)

`DungeonRoom` 不依赖 proto,持有世界状态并自己跑仿真,所以能直接单测:

- 实体:`Unit` / `Building` / `ResourceFieldEntity` / `ResourceDropEntity`,服务器分配唯一 id。
- 帧变更追踪:`dirty_` / `spawned_` / `despawned_` 三个集合,`Step` 里改状态时标脏,tick 末打包成增量后 `ClearFrameChanges`。
- 命令:`EnqueueCommand` 入队,`ApplyPending` 在 tick 边界统一应用(所有权校验在这里)。
- `Step(dt)`:移动(方案A 沿客户端给的 `path` 折线,空则直线)/ 工人采集-返还状态机 / 建造进度 / 训练队列 / 追击-攻击-死亡。
- 胜负:`BeginBattle` 记初始阵营数,`CheckGameOver` 在只剩一个阵营时判胜。

下发:`SendFullSnapshot` 每 N tick 发全量 `SnapshotNtf`;其余 `SendDelta` 发 `WorldDeltaNtf` + `EntitySpawnNtf` + `EntityDespawnNtf`(增量排除本帧新生)。

---

## 6. 可靠性 & 可观测性

- **背压**:每条连接发送队列上限 `kMaxSendQueueDepth`,满则丢最旧(状态同步靠下一帧全量快照重对齐),过载时优雅降级而非 OOM。丢包计入 `send_dropped_` / `metrics::send_drops_total`。
- **指标**:`MetricsServer` 在独立线程开 HTTP `/metrics`(默认 9100),暴露 tick `p50/p99/max`、命令/快照/增量计数、房间/单位 gauge、背压丢包。

---

## 7. 构建 / 运行 / 配置 / 测试

```bash
sudo apt install -y cmake g++ libboost-system-dev libspdlog-dev \
    protobuf-compiler libprotobuf-dev catch2

cmake --preset linux-release && cmake --build --preset linux-release -j

# 需在仓库根目录运行(读 config/config.ini)
./build/linux-release/GameServer    # 逻辑服 :50051, metrics :9100
./build/linux-release/GateServer    # 网关   :8888

ctest --test-dir build/linux-debug --output-on-failure   # 17 个 Catch2 用例
```

`config/config.ini` 关键项:`logic_shards`、`network_shards`、`gateway_link_count`、`metrics_port`。

---

## 8. 怎么扩展

- **加一个新指令**(如新的玩法命令):
  1. `proto/rts.proto` 加消息 + `MsgId`;`common/Protocol.h` 同步枚举。
  2. 网关:在 `ClientIngressRouter::Init` 注册转发(`RegisterForward`)。
  3. 游戏服:`NetworkShard::OnPacket` 的 switch 里归到玩法分支;`LogicRouter::Init` 注册到某个 `HandleXxx`;在 `LogicShard::HandlePlayerCommand` 解析并 `EnqueueCommand`。
  4. `DungeonRoom` 加对应的 `Apply*` + `Step*` 仿真逻辑,并补单测。
- **加一个新仿真系统**:在 `DungeonRoom::Step` 里加一步,改动的实体记得进 `dirty_`,并在快照/增量 `Fill*` 里带上字段。
- **调分片数**:改 `config.ini` 的 `logic_shards` / `network_shards`(网络 shard 走 `SO_REUSEPORT`,加机器/加 shard 即横向扩)。

---

## 9. 压测工具(`tools/`)

| 工具 | 用途 |
|---|---|
| `loadtest` | 直连 GameServer:50051 模拟网关链路,压逻辑/网络分片(少连接驱动大量虚拟房间) |
| `e2e_loadtest` | 端到端真客户端:连 Gate:8888 走全链路(登录→建房→准备→移动) |

```bash
./build/linux-release/loadtest     127.0.0.1 50051 8 500 10 100
./build/linux-release/e2e_loadtest 127.0.0.1 8888 2000 10 100 2
curl localhost:9100/metrics
```

---

## 10. 目录

```
common/        协议常量 + protobuf 编解码
proto/         rts.proto
GateServer/    网关(include/ + source/)
GameServer/    逻辑(include/ + source/)
tests/         Catch2 单测(覆盖 DungeonRoom 纯逻辑)
tools/         loadtest.cc / e2e_loadtest.cc
PERF_REPORT.md 分层压测报告(方法 + 数据)
```
