#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include<memory>
#include<mutex>
#include<iostream>
#include<spdlog/spdlog.h>
#include<boost/asio.hpp>
#include <boost/asio/detached.hpp>
#include<boost/asio/ip/tcp.hpp>
#include <string>
#include<thread>
#include<vector>

class Cerver;


constexpr size_t Buffer_size=1024;
constexpr std::chrono::seconds HEART_TIMEOUT{30};
constexpr std::chrono::seconds KEEP_ALIVE_TIME{15};
 enum  Session_state:uint8_t{
        Invalid,//初始化状态，
        Conected,//tcp已经连接，等待用户登录
        LoggedIn,//用户登录成功
        Closing,//正在关闭
        Closed//已经关闭
    };

constexpr size_t HEAD_TOTAL_LEN=4;
constexpr size_t HEAD_ID_LEN=2;
constexpr size_t HEAD_DATA_LEN=2;
constexpr size_t CONNECTION_NUMBER=8;
constexpr size_t GAMESERVER_CONN_CNT=8;
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