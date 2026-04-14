#include"../include/ClientSession.h"
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/detail/error_code.hpp>
#include <boost/tuple/detail/tuple_basic.hpp>
#include <chrono>
#include <spdlog/spdlog.h>

ClientSession::ClientSession(boost::asio::io_context &ioc,Cserver *server):
socket_(ioc),
buffer(new char[Buffer_size]),
state_(ClientSession_state::Connecting),
server_(server),
timer_(ioc),
strand_(ioc.get_executor()){

}


void ClientSession::start(){
    boost::asio::co_spawn(socket_.get_executor(),ClientSession::work(),boost::asio::detached);
    boost::asio::co_spawn(socket_.get_executor(),ClientSession::keep_alive(),boost::asio::detached);
}

//每十五秒苏醒一次，向服务器发送一次ping包
// boost::asio::awaitable<void> ClientSession::keep_alive(){
//     uint8_t state=co_await Get_state();
//     if(state!=ClientSession_state::Connected)co_return;
//     // sendfile(ping)
//     //发送一个ping包，十五秒以后苏醒，检测一下是否接收到数据包了，如果接受数据包的时间点，之间超过十五秒直接关闭连接
//     timer_.expires_after(KEEP_ALIVE_TIME);

//     co_await timer_.async_wait(boost::asio::use_awaitable);
//     auto time_stamp=co_await get_last_recv_time();
//     auto now=std::chrono::steady_clock::now();
//     if(now-time_stamp<=std::chrono::seconds(15)){
//         co_await keep_alive();
//     }
//     else{
//         co_await Set_State(ClientSession_state::Timeout);
//         co_return;
//     }

    
// }递归可能导致栈溢出
boost::asio::awaitable<void> ClientSession::keep_alive(){
    while(true){
       co_await boost::asio::dispatch(strand_,boost::asio::use_awaitable);
       if(state_!=ClientSession_state::Connected)co_return;

       timer_.expires_after(KEEP_ALIVE_TIME);
        // sendfile
       boost::system::error_code ec; 
       co_await timer_.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
       if(ec){
            if(ec==boost::asio::error::operation_aborted){
                spdlog::info("timer canceled, keep-alive exit");
                co_return; 
            }  
            else{
                co_await Set_State(ClientSession_state::Error);
                co_return;
            }
       }

       auto now=std::chrono::steady_clock::now();
       if(now-last_recv_time_>std::chrono::seconds(15)){
            co_await Set_State(ClientSession_state::Timeout);
            // close();
            co_return;
       }    
    }
}

boost::asio::awaitable<std::chrono::steady_clock::time_point> ClientSession::get_last_recv_time(){
    co_await boost::asio::dispatch(strand_,boost::asio::use_awaitable);
    auto time=last_recv_time_;
    co_return time;
}
boost::asio::awaitable<void> ClientSession::set_time_stamp(std::chrono::steady_clock::time_point time){
    co_await boost::asio::dispatch(strand_,boost::asio::use_awaitable);
    last_recv_time_=time;
    co_return;
}