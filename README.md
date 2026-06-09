# RTS 游戏服务器后端

一个 C++20 实现的**权威状态同步(state-sync)RTS 游戏服务器后端**:网关/逻辑双进程、
无锁分片 actor 架构,带完整的 tick 仿真(经济 / 建造 / 训练 / 战斗)、压测工具与可观测性。

> 重点不只在"能跑",而在一条完整的工程闭环:**架构设计 → 压测定位瓶颈 → 无锁优化 → 量化验证 → 背压兜底 → 指标可观测**,每一步都有数据支撑(见 [PERF_REPORT.md](PERF_REPORT.md))。

**技术栈**:C++20 · Boost.Asio(协程)· Protobuf · spdlog · CMake · Catch2 · GitHub Actions

---

## 亮点(给面试官的 30 秒版)

- **双层无锁分片 actor**:逻辑按 `room_id` 分片、网络层 `SO_REUSEPORT` 分片,**每个 shard 单线程 → 全程无锁**(不是用锁,而是用分片 + 消息投递)。
- **性能优化有据可依**:压测逐线程定位到"单网络线程瓶颈",做网络层分片后,同负载 **p95 延迟 5304ms → 21ms(~250×)、吞吐翻倍**。
- **可靠性**:压测发现过载时发送队列无界增长(RSS 1.1GB),加**有界队列 + 背压**后同场景 **1.1GB → 108MiB,tick 从 9.6Hz 回到 19.8Hz**(优雅降级而非 OOM)。
- **可观测性**:内置 Prometheus `/metrics`,暴露 tick p50/p99、吞吐、房间/单位数、背压丢包。
- **真实负载验证**:2 玩家真实对战(16v16,采集+战斗+训练同跑)下,4 核机稳定支撑 **~4000 玩家 / 6.4 万单位 @ 20Hz,p95 6.6ms**;**端到端**(真客户端经网关全链路)2000 客户端 @ 20Hz。
- **工程化**:17 个单测(纯逻辑层)+ CI + 详细压测报告。

---

## 架构

```
                 客户端 (Unity)
                      │  TCP, 10字节大端包头 + protobuf
                      ▼
        ┌─────────────────────────────┐
        │  GateServer (网关进程)        │   登录 / 会话 / 转发
        │  WorkShard 池 (N 线程)        │   每客户端一个 Csession
        │  GameServerConnPool (长连接池)│
        └─────────────┬───────────────┘
                      │  内部长连接, GateToGameEnvelope
                      ▼
        ┌─────────────────────────────┐
        │  GameServer (逻辑进程)         │
        │  NetworkShard × N             │  各自 io_context+线程+acceptor(SO_REUSEPORT)
        │   (无锁, 按链路 slot 管理)     │  入站解包 / 出站序列化
        │  LogicShard  × M              │  各自 io_context+线程
        │   (按 room_id 分片, 跑 tick)   │  房间 / 仿真 / 快照
        └─────────────────────────────┘
```

- **GateServer**:面向客户端,承接连接、登录、消息转发;`WorkShard` 线程池分担客户端 I/O。
- **GameServer**:跑游戏逻辑。`NetworkShard`(网络)与 `LogicShard`(逻辑)各自分片,各自单线程。
- **跨线程只靠 `boost::asio::post` 投递消息**(actor 信箱),shard 内无共享可变状态 → 无需锁。

---

## 核心设计

### 1. 状态同步 + 固定步长 tick
- 每个逻辑 shard 一个 **20Hz 心跳**,**绝对时刻调度**防累积漂移,落后则重对齐(防死亡螺旋)。
- 客户端只发**操作意图**,入队;到 tick 边界统一应用 + 推进世界一步(服务器权威)。
- 每 N 个 tick 发**全量快照** `SnapshotNtf`,其余发**增量** `WorldDelta/EntitySpawn/EntityDespawn`(脏标记)。

### 2. 双层分片(都无锁)
- **逻辑分片按 `room_id`**:房间 id 生成时落在自己同余类(`room_id % shard_count == shard_id`),保证**一个房间和它所有成员落在同一 shard** → 房间状态无锁。
- **网络分片用 `SO_REUSEPORT`**:N 个 acceptor 绑同一端口,内核负载均衡连接;每个 `NetworkShard` 单线程管自己那批链路。

### 3. 回包路由(无锁的关键)
`uid → 路由坐标 {net_shard, link, generation}` 下放到逻辑 shard(本就单线程)。回包时逻辑 shard 按坐标分组投给对应网络 shard,网络 shard 用 `link_id` 在自己的 `slots_` 里找 session、用 `generation` 防 slot 复用串台。**整条回包路径零锁。**

### 4. 背压 / 优雅降级
每连接发送队列有界(`kMaxSendQueueDepth`),满则丢最旧的包(状态同步靠下一帧全量快照重新对齐)。过载时**主动丢可恢复的数据保住 tick 与延迟**,而不是无界堆积 OOM。

### 5. 可观测性
独立线程的 HTTP `/metrics`(Prometheus 文本):tick `p50/p99/max`、命令/快照/增量计数、房间/单位 gauge、背压丢包数。

### 玩法(状态同步仿真)
登录 → 房间/准备/开局 → 服务器生成世界(主基地水晶 + 资源场 + 单位)→ 移动(方案A:客户端 NavMesh 路径 + 服务器折线推进)/ 采集经济 / 建造 / 训练 / 战斗 → 一方团灭判胜。

---

## 性能(4 核 / 16GB,压测器与服务器同机,完整方法见 [PERF_REPORT.md](PERF_REPORT.md))

### 优化闭环:网络层分片 before / after(同 4000 房负载)

| 指标 | 单网络线程(before) | 分片后(after) |
|---|---|---|
| p95 延迟 | **5304 ms** | **21 ms**(~250×) |
| 出站吞吐 | 41k msgs/s | **81k msgs/s** |
| 有效 tick | 10.3 Hz | **20.0 Hz** |
| RSS | 65 MiB | 15 MiB |

> 逐线程采样证明:分片前一个网络线程被钉在 100%、8 个逻辑线程几乎全闲;分片后 4 个网络线程均衡(各 ~0.4 核),单线程瓶颈消除。

### 真实 2 人对战容量(16v16:采集 + 战斗 + 训练同时跑)

| 2P 房 | 玩家 | 单位 | tick | 客户端 p95 | 服务器 tick p99 | 丢包 | CPU |
|---|---|---|---|---|---|---|---|
| 1000 | 2000 | 32k | 20Hz | 6.3 ms | 3.0 ms | 0 | 0.24 核 |
| **2000** | **4000** | **64k** | **20Hz** | **6.6 ms** | **7.4 ms** | **0** | **0.41 核** |
| 4000 | 8000 | 126k | 20Hz | 22.8 ms | 18 ms | 3789 | 0.81 核 |

### 背压:过载下的优雅降级(同 32 万单位过载场景)

| | 无界队列 | 有界 + 丢最旧 |
|---|---|---|
| RSS | ~1100 MiB | **108 MiB** |
| tick | 9.6 Hz(崩) | **19.8 Hz**(稳) |

### 端到端(网关 + 游戏服一起)

用真实客户端经 GateServer 全链路压(每客户端一条 TCP 连接:登录 → 建房 → 准备 → 移动,命令由网关注入 uid + 打包转发):

| 真实客户端 | 端到端 tick | 登录 RTT p95 | GameServer CPU | GateServer CPU |
|---|---|---|---|---|
| 1000 | 20 Hz | 87 ms | 0.48 核 | 0.58 核 |
| 2000 | 20 Hz | 64 ms | 0.85 核 | 1.00 核 |

> 网关不是免费的:每条消息**过网关两次**(客户端↔网关、网关↔游戏服)并拆/装 envelope,所以 Gate CPU 与 Game 同量级,**系统总开销 ≈ Game + Gate**。这是只测游戏服看不到的部分。

**诚实边界**:① 4 核机与压测器争 CPU,绝对上限偏保守;② 移动是直线/折线(方案A 将寻路放在客户端,服务器不跑 A\*),真实服务器权威寻路/碰撞会显著拉高单房成本、把容量降到几百房量级(会话制游戏常态:少量重对局/机 + 横向扩);③ 上述数字反映当前(较轻)per-unit 仿真,要测真实上限需独立压测机 + 更多核 + 更重 sim。

---

## 测试 & CI
- 17 个 Catch2 单测,覆盖纯逻辑层(`DungeonRoom`):队伍分配、指令所有权校验、移动/折线推进、战斗(射程/冷却/死亡)、经济(采集→返还)、建造、训练、团灭判胜。
- GitHub Actions:每次 push / PR 自动装依赖 → 编译 → `ctest`。

```bash
ctest --test-dir build/linux-debug --output-on-failure
```

---

## 构建 & 运行

```bash
# 依赖 (Ubuntu)
sudo apt install -y cmake g++ libboost-system-dev libspdlog-dev \
    protobuf-compiler libprotobuf-dev catch2

# 构建
cmake --preset linux-release
cmake --build --preset linux-release -j

# 运行(从仓库根目录,读取 config/config.ini)
./build/linux-release/GameServer    # 逻辑服 :50051, metrics :9100
./build/linux-release/GateServer    # 网关   :8888

# 压测:直连游戏服(模拟网关链路,压逻辑/网络分片)
./build/linux-release/loadtest 127.0.0.1 50051 8 500 10 100
# 压测:端到端真客户端(经网关全链路 Gate+Game)
./build/linux-release/e2e_loadtest 127.0.0.1 8888 2000 10 100 2
curl localhost:9100/metrics
```

关键配置(`config/config.ini`):`logic_shards` / `network_shards` / `gateway_link_count` / `metrics_port`。

---

## 目录结构

```
common/        协议常量 + protobuf 编解码
proto/         rts.proto (协议定义)
GateServer/    网关:Cserver / Csession / WorkShard / 连接池 / Router
GameServer/    逻辑:GameServer / NetworkShard / LogicShard / DungeonRoom(纯逻辑) / Metrics
tests/         Catch2 单测
tools/         loadtest.cc(直连游戏服压测) / e2e_loadtest.cc(端到端真客户端压测)
PERF_REPORT.md 完整分层压测报告
```

---

## 设计取舍与后续

- **为何状态同步而非帧同步**:服务器权威,反作弊强,且把含金量(仿真、快照、背压、AOI)放在服务端。
- **为何网络分片而非给单 NetworkShard 加锁**:保持无锁 actor 模型,横向可扩(加 shard / 加机器),压测数据也证明分片优于加锁。
- **后续**:服务器端权威寻路(A\*/流场)、AOI 视野裁剪、持久化 + 鉴权、容器化部署。
