# RTS 游戏后端 (GameServer + GateServer) 容器镜像。
# 两个进程跑在同一镜像内: 它们通过 127.0.0.1:50051 通信, 属于同一个部署单元,
# 所以无需改任何配置 (config/config.ini 里 GameServer host 本就是 127.0.0.1)。
FROM ubuntu:22.04

# 低内存机器可在构建时传 --build-arg BUILD_JOBS=1
ARG BUILD_JOBS=2
ENV DEBIAN_FRONTEND=noninteractive

# 构建依赖 (见 BUILD.md): 编译器/构建工具 + Boost + spdlog + protobuf + Catch2
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        libboost-all-dev \
        libspdlog-dev \
        protobuf-compiler \
        libprotobuf-dev \
        catch2 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . /app

# Release 构建; CMake 会自动从 proto/rts.proto 生成 C++ 代码。
# 只编两个服务器目标 (跳过测试/压测, 省时省内存)。
RUN cmake --preset linux-release \
    && cmake --build build/linux-release --target GameServer GateServer --parallel ${BUILD_JOBS}

RUN chmod +x /app/docker/entrypoint.sh

# 8888 = GateServer(面向客户端)。50051 = GameServer(仅容器内部, 不对外暴露)
EXPOSE 8888

ENTRYPOINT ["/app/docker/entrypoint.sh"]
