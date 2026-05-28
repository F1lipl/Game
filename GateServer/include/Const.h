#pragma once
#include <cstddef>
#include <chrono>
#include <cstdint>
#include<spdlog/spdlog.h>
#include"IniConfig.h"
#include "../../common/Protocol.h"


constexpr size_t Buffer_size = rts::protocol::kMaxPacketBodyLen;
constexpr std::chrono::seconds HEART_TIMEOUT{30};
constexpr std::chrono::seconds KEEP_ALIVE_TIME{15};
 enum  Session_state:uint8_t{
        Invalid,//初始化状态，
        Conected,//tcp已经连接，等待用户登录
        LoggedIn,//用户登录成功
        Closing,//正在关闭
        Closed//已经关闭
    };

constexpr size_t HEAD_TOTAL_LEN = rts::protocol::kPacketHeaderLen;
constexpr size_t HEAD_MAGIC_LEN = rts::protocol::kPacketMagicLen;
constexpr size_t HEAD_ID_LEN = rts::protocol::kPacketMsgIdLen;
constexpr size_t HEAD_FLAGS_LEN = rts::protocol::kPacketFlagsLen;
constexpr size_t HEAD_DATA_LEN = rts::protocol::kPacketBodyLenLen;
constexpr size_t HEAD_MAGIC_OFFSET = rts::protocol::kPacketMagicOffset;
constexpr size_t HEAD_ID_OFFSET = rts::protocol::kPacketMsgIdOffset;
constexpr size_t HEAD_FLAGS_OFFSET = rts::protocol::kPacketFlagsOffset;
constexpr size_t HEAD_DATA_OFFSET = rts::protocol::kPacketBodyLenOffset;
constexpr size_t CONNECTION_NUMBER=8;
constexpr size_t GAMESERVER_CONN_CNT=8;
constexpr size_t WORK_SHARD_NUMBER=8;
constexpr std::chrono::seconds LINK_DETECTION_TIME=std::chrono::seconds(1);
enum ClientSession_state:uint8_t{
    Connecting,//初始化正在连接游戏服务器
    Connected,//连接正常，空闲，正常在连接池里
    Busy,//正在发送数据

    Timeout,//心跳超时
    Error,//读写异常
    closing,//正在关闭
    closed,//已经关闭

};
