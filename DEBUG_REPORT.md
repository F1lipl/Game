# 编译与调试问题记录

本文记录本次在低内存服务器上编译 `Gamer` 项目时遇到的问题、根因、修复位置和验证结果。

## 编译方式

服务器内存较小，不适合使用默认的高并行编译。最终使用单线程编译：

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug --parallel 1
```

## 问题 1：并行编译容易超时或占用过高

### 现象

最开始使用：

```bash
cmake --build --preset linux-debug -j
```

编译过程中在多个 C++ 源文件同时构建时超时。低内存服务器上并行编译会同时拉起多个 `g++` 进程，内存压力较大。

### 处理

改用：

```bash
cmake --build --preset linux-debug --parallel 1
```

单线程编译后可以稳定暴露真实的编译错误，并最终完成构建。

## 问题 2：Singleton 创建对象方式错误

### 现象

单线程编译时，`GameServer/source/GameServer.cc` 中调用：

```cpp
LogicRouter::Getinstance()->Init();
```

触发模板实例化后报错：

```text
no matching function for call to 'LogicRouter::LogicRouter(LogicRouter*)'
```

### 出错位置

- `GameServer/include/Singleton.h`
- `GateServer/include/Singleton.h`

原代码：

```cpp
instance_=std::make_shared<T>(new T);
```

### 根因

`std::make_shared<T>(new T)` 的含义不是“用这个指针创建 shared_ptr”，而是“调用 `T` 的构造函数，并把 `new T` 这个指针作为构造参数传进去”。

因此编译器会尝试调用：

```cpp
LogicRouter::LogicRouter(LogicRouter*)
```

但 `LogicRouter` 只有无参构造函数，所以编译失败。

另外，`LogicRouter` 的构造函数是 private，并通过：

```cpp
friend Singleton<LogicRouter>;
```

授权 `Singleton<LogicRouter>` 创建实例。因此这里应该在 `Singleton` 内部直接 `new T`，再交给 `std::shared_ptr` 管理。

### 修复

改为：

```cpp
instance_=std::shared_ptr<T>(new T);
```

## 问题 3：接收包头成员类型声明错误

### 现象

编译 `GameServer/source/GatewayLinkSession.cc` 时出现：

```text
'MsgNode' has no member named 'MsgId'
'MsgNode' has no member named 'Flags'
```

报错点在 `GatewayLinkSession.cc` 中调用：

```cpp
Recv_node_->MsgId()
Recv_node_->Flags()
```

### 出错位置

- `GameServer/include/GatewayLinkSession.h`
- `GateServer/include/Csession.h`
- `GateServer/include/ClientSession.h`

原声明使用了基类指针：

```cpp
std::shared_ptr<MsgNode> Recv_node_;
std::shared_ptr<MsgNode>head_;
```

但实际创建的是：

```cpp
std::make_shared<RecvNode>(...)
```

并且后续代码需要访问 `RecvNode` 上的 `MsgId()` 和 `Flags()`。

### 根因

`MsgNode` 基类只保存 `_data`、`_cur_len`、`_total_len`，没有 `MsgId()` 和 `Flags()` 方法。虽然实际对象是 `RecvNode`，但变量静态类型是 `MsgNode`，编译器只能看到 `MsgNode` 的接口，因此报错。

### 修复

把接收包头成员声明改为 `RecvNode`：

```cpp
std::shared_ptr<RecvNode> Recv_node_;
std::shared_ptr<RecvNode>head_;
```

这样声明类型与实际创建对象、后续访问接口一致。

## 问题 4：GameServer 退出时发生 SIGSEGV

### 现象

`GameServer` 编译成功后，使用短时间启动验证：

```bash
timeout 5s ./build/linux-debug/GameServer
```

服务可以启动并监听端口，但收到 `timeout` 发送的 SIGTERM 后出现：

```text
timeout: the monitored command dumped core
```

### 调试结果

使用 gdb 复现并查看回溯，崩溃发生在进程退出阶段：

```text
Singleton<LogicRouter>::~Singleton()
spdlog::info(...)
spdlog::sinks::sink::should_log(...)
```

### 出错位置

- `GameServer/include/Singleton.h`
- `GateServer/include/Singleton.h`

原析构函数：

```cpp
~Singleton() {
    spdlog::info("this is instance distruct");
}
```

### 根因

`Singleton<LogicRouter>::instance_` 是静态对象，进程退出时会参与静态析构。此时 `spdlog` 的全局资源可能已经被析构，继续调用 `spdlog::info` 会访问已经无效的日志 sink，从而导致段错误。

### 修复

单例基类析构函数不再在静态析构阶段写日志：

```cpp
~Singleton() = default;
```

同时删除 `Singleton.h` 中不再使用的 `#include <spdlog/spdlog.h>`。

## 最终验证

### 编译验证

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug --parallel 1
```

结果：

```text
[100%] Built target GateServer
```

`GameServer` 和 `GateServer` 均编译成功。

### GameServer 启动与退出验证

```bash
timeout 5s ./build/linux-debug/GameServer
```

结果：

```text
GameServer started, logic_shards=8, gateway_links=64
GameServer listening on 0.0.0.0:50051
GameServer received signal 15, stopping
GameServer stopped
```

修复后没有再出现 core dump。

### GateServer 启动与退出验证

```bash
timeout 5s ./build/linux-debug/GateServer
```

结果：

```text
GateServer listening on 0.0.0.0:8888
GateServer received signal 15, stopping
```

服务可以启动并响应 SIGTERM。短时间停止时出现的 `Operation canceled` 是未完成异步连接被取消后的日志，不是编译错误。

### 测试

```bash
ctest --test-dir build/linux-debug --output-on-failure
```

结果：

```text
No tests were found!!!
```

当前 CMake 构建目录没有配置测试用例。

## 修改文件汇总

- `GameServer/include/Singleton.h`
- `GateServer/include/Singleton.h`
- `GameServer/include/GatewayLinkSession.h`
- `GateServer/include/Csession.h`
- `GateServer/include/ClientSession.h`

