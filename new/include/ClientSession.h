#pragma once
#include<boost/asio.hpp>
#include"Const.h"
#include "WorkShard.h"
#include <boost/asio/awaitable.hpp>
#include<queue>
class RecvNode;
class MsgNode;
class SendNode;
class ClientSession:public std::enable_shared_from_this<ClientSession>
{

public:
    ClientSession(boost::asio::io_context& ioc,WorkShard* Shard);
    ~ClientSession();
    void start();
    void close();
    uint8_t get_state(){
        return state_;
    }
    void SendData(std::shared_ptr<SendNode>);


private:
    boost::asio::awaitable<void>keep_alive();
    void set_time_stamp(std::chrono::steady_clock::time_point time);
    std::chrono::steady_clock::time_point get_last_recv_time();
    boost::asio::awaitable<size_t>Readhead();
    boost::asio::awaitable<bool>ReadData(size_t);
    boost::asio::awaitable<void>start_write_loop();
    boost::asio::awaitable<void>HandleRead();
    WorkShard* shard_;
    boost::asio::steady_timer timer_;//心跳保活
    std::queue<std::shared_ptr<SendNode>>send_que_;//发送队列
    boost::asio::ip::tcp::socket socket_;
    char* buffer_;
    uint8_t state_;
    //消息头缓冲区，消息缓冲区
    std::shared_ptr<MsgNode>head_;
    std::shared_ptr<RecvNode>body_;
    
    //由于Csession也会用socket发信息，所以完了保证并发不会出错，必须用strand_限制每次都只能有一个协程去修改send_que_;
    // boost::asio::strand<boost::asio::io_context::executor_type>strand_;
    bool is_writing_;
    std::chrono::steady_clock::time_point last_recv_time_;//最后一次接收信息的时间戳

};