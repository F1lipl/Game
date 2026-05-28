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
