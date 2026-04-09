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
#include<thread>
#include<vector>


constexpr size_t Buffer_size=1024;
constexpr std::chrono::seconds HEART_TIMEOUT{30};
 enum  Session_state:uint8_t{
        Invalid,//初始化状态，
        Conected,//tcp已经连接，等待用户登录
        LoggedIn,//用户登录成功
        Closing,//正在关闭
        Closed//已经关闭
    };
