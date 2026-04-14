#pragma once
#include"Const.h"
#include"Cserver.h"
#include <boost/asio/awaitable.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include<queue>
#include"MsgNode.h"
//接收数据，心跳保活，
class Cserver;
class ClientSession:public enable_shared_from_this<ClientSession>
{

public:
    ClientSession(boost::asio::io_context& ioc,Cserver* server);
    ~ClientSession();
    void start();
    boost::asio::awaitable<void> Set_State(uint8_t state){
        co_await asio::dispatch(strand_, asio::use_awaitable);
        state_=state;
        co_return;
    }
    boost::asio::awaitable<uint8_t> Get_state(){
        co_await boost::asio::dispatch(strand_,boost::asio::use_awaitable);
        co_return state_;
    }


private:
    boost::asio::awaitable<void> work();
    boost::asio::awaitable<void>keep_alive();
    boost::asio::awaitable<void>set_time_stamp(std::chrono::steady_clock::time_point time);
    boost::asio::awaitable<std::chrono::steady_clock::time_point>get_last_recv_time();
    boost::asio::awaitable<void>close();
    boost::asio::awaitable<size_t>Readhead();
    Cserver* server_;
    boost::asio::steady_timer timer_;//心跳保活
    queue<SendNode>send_que_;
    boost::asio::ip::tcp::socket socket_;
    char* buffer_;
    uint8_t state_;
    //消息头缓冲区，消息缓冲区
    std::shared_ptr<MsgNode>head_;
    std::shared_ptr<RecvNode>body_;

    //由于Csession也会用socket发信息，所以完了保证并发不会出错，必须用strand_限制每次都只能有一个协程去修改send_que_;
    boost::asio::strand<boost::asio::io_context::executor_type>strand_;
    std::chrono::steady_clock::time_point last_recv_time_;//最后一次接收信息的时间戳

};