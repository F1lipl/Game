cmake_minimum_required(VERSION 3.20)

project(RtsGameBackend LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
find_package(Protobuf CONFIG QUIET)
if(NOT Protobuf_FOUND)
    find_package(Protobuf REQUIRED)
endif()
find_package(Boost REQUIRED COMPONENTS system)
find_package(spdlog CONFIG REQUIRED)

if(TARGET protobuf::libprotobuf)
    set(RTS_PROTOBUF_LIB protobuf::libprotobuf)
elseif(TARGET Protobuf::libprotobuf)
    set(RTS_PROTOBUF_LIB Protobuf::libprotobuf)
else()
    set(RTS_PROTOBUF_LIB ${Protobuf_LIBRARIES})
endif()

if(TARGET protobuf::protoc)
    set(RTS_PROTOC_COMMAND protobuf::protoc)
elseif(DEFINED Protobuf_PROTOC_EXECUTABLE)
    set(RTS_PROTOC_COMMAND ${Protobuf_PROTOC_EXECUTABLE})
elseif(DEFINED PROTOBUF_PROTOC_EXECUTABLE)
    set(RTS_PROTOC_COMMAND ${PROTOBUF_PROTOC_EXECUTABLE})
else()
    message(FATAL_ERROR "protoc executable not found by CMake")
endif()

add_library(rts_project_options INTERFACE)
target_compile_definitions(rts_project_options INTERFACE
    BOOST_ASIO_HAS_CO_AWAIT
)

if(WIN32)
    target_compile_definitions(rts_project_options INTERFACE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00
    )
endif()

set(RTS_PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/proto/rts.proto)
set(RTS_PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/cpp)
set(RTS_PROTO_GEN_CC ${RTS_PROTO_GEN_DIR}/rts.pb.cc)
set(RTS_PROTO_GEN_H ${RTS_PROTO_GEN_DIR}/rts.pb.h)

add_custom_command(
    OUTPUT ${RTS_PROTO_GEN_CC} ${RTS_PROTO_GEN_H}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${RTS_PROTO_GEN_DIR}
    COMMAND ${RTS_PROTOC_COMMAND}
        --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
        --cpp_out=${RTS_PROTO_GEN_DIR}
        ${RTS_PROTO_FILE}
    DEPENDS ${RTS_PROTO_FILE}
    VERBATIM
)

add_library(rts_proto STATIC
    ${RTS_PROTO_GEN_CC}
    ${RTS_PROTO_GEN_H}
)

target_include_directories(rts_proto PUBLIC
    ${RTS_PROTO_GEN_DIR}
)

target_link_libraries(rts_proto PUBLIC
    ${RTS_PROTOBUF_LIB}
)

if(Protobuf_INCLUDE_DIRS)
    target_include_directories(rts_proto PUBLIC ${Protobuf_INCLUDE_DIRS})
endif()

add_library(GameServerLib STATIC
    GameServer/source/GameServer.cc
    GameServer/source/GatewayLinkSession.cc
    GameServer/source/LogicRouter.cc
    GameServer/source/LogicShard.cc
    GameServer/source/MetricsServer.cc
    GameServer/source/MsgNode.cc
    GameServer/source/NetworkRouter.cc
    GameServer/source/NetworkShard.cc
)

target_include_directories(GameServerLib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/GameServer/include
)

target_link_libraries(GameServerLib PUBLIC
    rts_project_options
    rts_proto
    Boost::system
    spdlog::spdlog
    Threads::Threads
)

add_executable(GameServer
    GameServer/source/GameServerMain.cc
)

target_link_libraries(GameServer PRIVATE
    GameServerLib
)

add_library(GateServerLib STATIC
    GateServer/source/ClientSession.cc
    GateServer/source/Cserver.cc
    GateServer/source/Csession.cc
    GateServer/source/GateSessionRegistry.cc
    GateServer/source/GateProtocol.cc
    GateServer/source/GameServerConnPool.cc
    GateServer/source/LogicRouter.cc
    GateServer/source/MsgNode.cc
    GateServer/source/Router.cc
    GateServer/source/WorkShard.cc
)

target_include_directories(GateServerLib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/GateServer/include
)

target_link_libraries(GateServerLib PUBLIC
    rts_project_options
    rts_proto
    Boost::system
    spdlog::spdlog
    Threads::Threads
)

add_executable(GateServer
    GateServer/source/GateServerMain.cc
)

target_link_libraries(GateServer PRIVATE
    GateServerLib
)

# ---- Load testing tool ----
add_executable(loadtest
    tools/loadtest.cc
)

target_include_directories(loadtest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(loadtest PRIVATE
    rts_proto
    Boost::system
    Threads::Threads
)

# End-to-end load tool (acts as real clients against the GateServer)
add_executable(e2e_loadtest
    tools/e2e_loadtest.cc
)

target_include_directories(e2e_loadtest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(e2e_loadtest PRIVATE
    rts_proto
    Boost::system
    Threads::Threads
)

# ---- Tests ----
# DungeonRoom is pure simulation logic (no proto/boost), so the test target
# only needs Catch2 + the GameServer headers.
find_package(Catch2 3 QUIET)
if(Catch2_FOUND)
    enable_testing()

    add_executable(DungeonRoomTest
        tests/DungeonRoomTest.cc
    )

    target_include_directories(DungeonRoomTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/GameServer/include
    )

    target_link_libraries(DungeonRoomTest PRIVATE
        Catch2::Catch2WithMain
    )

    list(APPEND CMAKE_MODULE_PATH ${Catch2_DIR})
    include(Catch)
    catch_discover_tests(DungeonRoomTest)
else()
    message(STATUS "Catch2 not found; skipping tests")
endif()
