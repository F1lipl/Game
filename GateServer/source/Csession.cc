#include "../include/Csession.h"
#include "../include/Cserver.h"
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/detail/impl/socket_ops.ipp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/impl/read.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/detail/error_code.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid.hpp>
#include<boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <spdlog/spdlog.h>


std::string create_uuid(){
    boost::uuids::uuid u=boost::uuids::random_generator()();
    return boost::uuids::to_string(u);
}


Csession::Csession(Cserver* server,boost::asio::io_context& ioc):socket_(ioc),
server_(server),
timer_(ioc),
state_(Session_state::Invalid),
uuid_(create_uuid()),
buffer_(new char[Buffer_size])
{
    // 不要在构造函数中调用 shared_from_this()
    // server_->Set_session_id(uuid_,shared_from_this());
};

boost::asio::awaitable<void> Csession::start(){
    Set_state(Session_state::Conected);
    auto ptr = shared_from_this();
    last_recv_time = std::chrono::steady_clock::now();
    co_await server_->Set_session_id(uuid_, ptr);  // 协程调用
    boost::asio::co_spawn(socket_.get_executor(), start_heartbeat(), boost::asio::detached);//开启心跳检测
    boost::asio::co_spawn(socket_.get_executor(), work(), boost::asio::detached);
    co_return;
}


boost::asio::awaitable<void> Csession::start_heartbeat(){
    auto ptr=shared_from_this();
    if(is_clothing())co_return;//如果连接正在断开，就直接取消定时器；
    timer_.expires_after(HEART_TIMEOUT);//设置超时时间
    boost::system::error_code ec;
    co_await timer_.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable,ec));

    if(ec){//说明在超时时间内，收到了信息，定时任务被取消了
        co_await start_heartbeat();
        co_return;
    }
    
    // 执行到这里说明超时了，直接关掉session的连接；
    co_await close();
    co_return;
}

boost::asio::awaitable<void> Csession::close(){
    Set_state(Session_state::Closing);
    co_await server_->Delete_session(uuid_);
    timer_.cancel();
    boost::system::error_code ec;
    socket_.cancel(ec);
    spdlog::info("session {} is closing",uuid_);
    socket_.close();
    co_return;
}

//读头->读数据->抛出给业务层->写回包
boost::asio::awaitable<void> Csession::work(){
    size_t data_len=co_await ReadHead();
    co_await ReadData(data_len);

    co_return;
}

boost::asio::awaitable<size_t>Csession::ReadHead(){
    auto self =shared_from_this();
    memset(buffer_,0,Buffer_size);
    auto [ec,lenth]=co_await boost::asio::async_read(socket_,boost::asio::buffer(buffer_,HEAD_TOTAL_LEN),boost::asio::as_tuple(boost::asio::use_awaitable));
    if(ec){
        spdlog::error("session id {} read head error",uuid_);
        co_await close();
    }
    // 转化字节序
    if(lenth!=HEAD_TOTAL_LEN){
        spdlog::error("recv head lenth false,session id is {}",uuid_);
        co_await close();
    }
    Recv_node_->Clear();
    memcpy(Recv_node_->_data,buffer_,HEAD_TOTAL_LEN);
    short msg_id=0;
    memcpy(&msg_id,Recv_node_->_data,HEAD_ID_LEN);
    // 转成本地字节序
    msg_id=boost::asio::detail::socket_ops::network_to_host_short(msg_id);
    if(msg_id!=HEAD_ID_LEN){
        spdlog::error("session id {} read head error",uuid_);
        co_await close();
    }
    short data_len=0;
    memcpy(&data_len,buffer_+HEAD_ID_LEN,HEAD_DATA_LEN);
    data_len=boost::asio::detail::socket_ops::network_to_host_short(data_len);
    if(data_len>Buffer_size){
        spdlog::error("session id {},recv package len is too long",uuid_);
        co_await close();
    }
    spdlog::info("session id {} recv package,msg id is {},data len is {}",uuid_,msg_id,data_len);
    Recv_node_=std::make_shared<RecvNode>(HEAD_TOTAL_LEN,msg_id);
    
    co_return data_len;
}


boost ::asio::awaitable<void> Csession::ReadData(size_t len){
    memset(buffer_,0,Buffer_size);
    auto [ec,recv_data_len]=co_await boost::asio::async_read(socket_,boost::asio::buffer(buffer_,len),boost::asio::as_tuple(boost::asio::use_awaitable));
    if(ec){
        spdlog::error("session id {} read data error",uuid_);
        co_await close();
        co_return;
    }
    if(recv_data_len!=len){
        spdlog::error("session id {} recv data len is error",uuid_);
        co_await close();
    }
    memcpy(data_node_->_data,buffer_,len);
    co_return;
}