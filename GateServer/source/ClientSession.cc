#include"../include/ClientSession.h"
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/impl/read.hpp>
#include <boost/asio/impl/write.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/detail/error_code.hpp>
#include <boost/tuple/detail/tuple_basic.hpp>
#include <chrono>
#include <memory>
#include <spdlog/spdlog.h>
#include <sys/stat.h>


//写了read write 心跳检测 关闭//
ClientSession::ClientSession(boost::asio::io_context &ioc,Cserver *server):
socket_(ioc),
buffer_(new char[Buffer_size]),
state_(ClientSession_state::Connecting),
server_(server),
timer_(ioc),
is_writing_(false)

{

}


void ClientSession::start(){
    //并发建立连接
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

    
// // }递归可能导致栈溢出
// boost::asio::awaitable<void> ClientSession::keep_alive(){
//     while(true){
//        co_await boost::asio::dispatch(strand_,boost::asio::use_awaitable);
//        if(state_!=ClientSession_state::Connected&&state_!=ClientSession_state::Busy)co_return;

//        timer_.expires_after(KEEP_ALIVE_TIME);
//         // sendfile
//        boost::system::error_code ec; 
//        co_await timer_.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
//        if(ec){
//             if(ec==boost::asio::error::operation_aborted){
//                 spdlog::info("timer canceled, keep-alive exit");
//                 // co_await close();
//                 co_return; 
//             }  
//             else{
//                 co_await Set_State(ClientSession_state::Error);
//                 co_await close();
//                 co_return;
//             }
//        }
//        if(state_==ClientSession_state::Busy){
//             timer_.expires_after(std::chrono::seconds(5));
//             co_await timer_.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable,ec));
//             if(ec){
//                 if(ec==boost::asio::error::operation_aborted)co_return;
//                 co_await Set_State(ClientSession_state::Error);
//                 co_await close();
//                 co_return;
//             }
//             continue;
//        }
//        if(state_==ClientSession_state::Connected){
//             auto now=std::chrono::steady_clock::now();
//             if(now-last_recv_time_>std::chrono::seconds(15)){
//                 co_await Set_State(ClientSession_state::Timeout);
//                 co_await close();
//                 co_return;
//         }    
//        }
      
//     }
// }

boost::asio::awaitable<void> ClientSession::keep_alive() {
    auto self=shared_from_this();
    boost::system::error_code ec;
    while (true) {
        co_await boost::asio::dispatch(socket_.get_executor(), boost::asio::use_awaitable);

        if (state_ != ClientSession_state::Connected &&
            state_ != ClientSession_state::Busy) {
            co_return;
        }

        if (state_ == ClientSession_state::Busy) {
            timer_.expires_after(std::chrono::seconds(5));
            ec.clear();
            co_await timer_.async_wait(
                boost::asio::redirect_error(boost::asio::use_awaitable, ec));

            if (ec == boost::asio::error::operation_aborted) {
                co_return;
            }
            if (ec) {
               state_= ClientSession_state::Error;
                close();
                co_return;
            }

            continue;
        }

        timer_.expires_after(KEEP_ALIVE_TIME);
        ec.clear();
        co_await timer_.async_wait(
            boost::asio::redirect_error(boost::asio::use_awaitable, ec));

        if (ec == boost::asio::error::operation_aborted) {
            spdlog::info("timer canceled, keep-alive exit");
            close();
            co_return;
        }
        if (ec) {
            state_=ClientSession_state::Error;
            close();
            co_return;
        }

        auto now = std::chrono::steady_clock::now();
        if (now - last_recv_time_ > std::chrono::seconds(15)) {
            state_=ClientSession_state::Timeout;
            close();
            co_return;
        }
        //
        // 如果协议要求主动发心跳包，这里发送
    }
}


std::chrono::steady_clock::time_point  ClientSession::get_last_recv_time(){
    return last_recv_time_;
}
void ClientSession::set_time_stamp(std::chrono::steady_clock::time_point time){
    last_recv_time_=time;
    return;
}

boost::asio::awaitable<void> ClientSession::work(){
    co_await boost::asio::dispatch(socket_.get_executor(),boost::asio::use_awaitable);
    size_t len=co_await Readhead();
    co_await ReadData(len);
    //逻辑层处理
    co_return;
}

boost::asio::awaitable<size_t>ClientSession::Readhead(){
    auto self =shared_from_this();

    memset(buffer_,0,Buffer_size);
    auto [ec,lenth]=co_await boost::asio::async_read(socket_,boost::asio::buffer(buffer_,HEAD_TOTAL_LEN),boost::asio::as_tuple(boost::asio::use_awaitable));
    if(ec){
        spdlog::error(" read head error");
        close();
    }
    // 转化字节序
    if(lenth!=HEAD_TOTAL_LEN){
        spdlog::error("recv head lenth false");
        close();
    }
    head_->Clear();
    memcpy(head_->_data,buffer_,HEAD_TOTAL_LEN);
    short msg_id=0;
    memcpy(&msg_id,head_->_data,HEAD_ID_LEN);
    // 转成本地字节序
    msg_id=boost::asio::detail::socket_ops::network_to_host_short(msg_id);
    if(msg_id!=HEAD_ID_LEN){
        spdlog::error("read head error");
        close();
    }
    short data_len=0;
    memcpy(&data_len,buffer_+HEAD_ID_LEN,HEAD_DATA_LEN);
    data_len=boost::asio::detail::socket_ops::network_to_host_short(data_len);
    if(data_len>Buffer_size){
        spdlog::error("recv package len is too long");
        close();
    }
    spdlog::info(" recv package,msg id is {},data len is {}",msg_id,data_len);
    head_=std::make_shared<RecvNode>(HEAD_TOTAL_LEN,msg_id);
    
    co_return data_len;
}

boost ::asio::awaitable<void> ClientSession::ReadData(size_t len){
    memset(buffer_,0,Buffer_size);
    auto [ec,recv_data_len]=co_await boost::asio::async_read(socket_,boost::asio::buffer(buffer_,len),boost::asio::as_tuple(boost::asio::use_awaitable));
    if(ec){
        spdlog::error(" read data error");
        state_=ClientSession_state::Error;
        close();
        co_return;
    }
    if(recv_data_len!=len){
        spdlog::error("recv data len is error");
        close();
    }
    memcpy(body_->_data,buffer_,len);
    co_return;
}







void ClientSession::close(){
    auto self=shared_from_this();
    if(state_==ClientSession_state::closed)return;
    timer_.cancel();
    state_=ClientSession_state::closing;
    boost::system::error_code ec;
    socket_.close(ec);
    if (ec) {
        spdlog::error("ClientSession close socket failed: {}", ec.message());
    }
    state_ = ClientSession_state::closed;
    spdlog::info("ClientSession closed successfully");
    return;
}


// 需要同步的机制，最好放在同一个函数里这样能保证串行执行
void ClientSession::SendData(std::shared_ptr<SendNode> node) {
    auto ex = socket_.get_executor();
    // co_await boost::asio::dispatch(ex, boost::asio::use_awaitable);

    send_que_.push(std::move(node));

    if (is_writing_) {
        return;
    }

    is_writing_ = true;

    auto self = shared_from_this();
    boost::asio::co_spawn(
        ex,
        [self]() -> boost::asio::awaitable<void> {
            co_await self->start_write_loop();
        },
        boost::asio::detached);

    return;;
}

boost::asio::awaitable<void> ClientSession::start_write_loop() {
    while (!send_que_.empty() &&
           (state_ == ClientSession_state::Busy ||
            state_ == ClientSession_state::Connected)) {
        state_ = ClientSession_state::Busy;

        auto node = send_que_.front();
        send_que_.pop();

        auto [ec, write_len] =
            co_await boost::asio::async_write(
                socket_,
                boost::asio::buffer(node->_data, node->_total_len),
                boost::asio::as_tuple(boost::asio::use_awaitable));

        if (ec) {
            is_writing_ = false;
            state_ = ClientSession_state::Error;
            spdlog::error("write data is error {}", ec.message());
            close();
            co_return;
        }
    }

    is_writing_ = false;

    if (state_ == ClientSession_state::Busy) {
        state_ = ClientSession_state::Connected;
    }

    co_return;
}
