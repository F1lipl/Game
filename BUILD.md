# Linux Build

Install dependencies on the Linux server:

```bash
sudo apt update
sudo apt install -y cmake g++ libboost-all-dev libspdlog-dev protobuf-compiler libprotobuf-dev
```

Generate Unity C# protobuf code when the client schema changes:

```bash
cd Game
./tools/generate_proto.sh
```

The backend C++ protobuf files are generated automatically by CMake into the build directory. Configure and build:

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug -j
```

Run locally on the Linux server:

```bash
./build/linux-debug/GameServer
./build/linux-debug/GateServer
```

Release build:

```bash
cmake --preset linux-release
cmake --build --preset linux-release -j
```

## 低内存机器上编译(避免 OOM 卡死)

并行编译会同时拉起多个 g++,而本项目单个翻译单元很吃内存(Boost.Asio 协程头、
protobuf 生成代码、较大的 `DungeonRoom.h` 头文件)。在内存小的服务器上,内存一满
就会疯狂换页或被 OOM killer 杀掉,表现就是"卡死"。按性价比这么处理:

**1. 单线程编译(最先试)** —— 一次只编一个文件,峰值内存最小:

```bash
cmake --build --preset linux-release -j 1     # 或 --parallel 1
```

**2. 加一块 swap(防止硬卡死/被杀)**:

```bash
sudo fallocate -l 4G /swapfile
sudo chmod 600 /swapfile && sudo mkswap /swapfile && sudo swapon /swapfile
free -h
```

通常 `-j1` + swap 就够了。如果 `-j1` 仍在某个文件(多为 `rts.pb.cc` 或 `LogicShard.cc`)上爆内存,再叠加下面这些:

**3. 降低单个文件的编译内存**(降优化等级 + 去调试信息):

```bash
cmake -S . -B build/lowmem -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS="-O1 -g0"
cmake --build build/lowmem -j 1
```

**4. 只编需要的目标**(不必把网关/测试/压测器都编):

```bash
cmake --build build/lowmem --target GameServer -j 1
```

**5. 换更省内存的链接器**(链接也会有内存尖峰):

```bash
cmake -S . -B build/lowmem -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=gold"   # 或 lld
```

**6. 或者干脆别在小机器上编**:在开发机/大点的机器上编好,把可执行文件 `scp`
到服务器运行即可(系统/库版本一致)。编译是开发期的事,生产小机器没必要扛。

经验值:本项目单个 g++ 进程峰值约 0.5–1.5GB,所以并行度大致按 `可用内存GB / 1.5`
来定;1–2GB 内存的机器就老老实实 `-j 1`。

