#include"../include/Csession.h"
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <cstddef>
#include<string>
#include<boost/asio.hpp>
#include <boost/system/detail/error_code.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid.hpp>
#include<boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

std::string create_uuid(){
    boost::uuids::uuid u=boost::uuids::random_generator()();
    return boost::uuids::to_string(u);
}


Csession::Csession(WorkShard* shard,boost::asio::io_context& ioc)
:
shard_(shard),
socket_(ioc),
uuid_(create_uuid()),
timer_(ioc),
state_(Session_state::Invalid),
buffer_(new char[Buffer_size]),
is_writing_(false),
Recv_node_(new MsgNode(HEAD_TOTAL_LEN)),
data_node_(new RecvNode(Buffer_size,-1))
{
    
}


void Csession::start(){
    Set_state(Session_state::Conected);

    boost::asio::co_spawn(socket_.get_executor(),start_heartbeat(),boost::asio::detached);
    boost::asio::co_spawn(socket_.get_executor(),handle_read(),boost::asio::detached);
}

boost::asio::awaitable<void> Csession::handle_read(){
    if(state_==Session_state::Closing||state_==Session_state::Closed)co_return;
    size_t len=co_await ReadHead();
    co_await ReadData(len);
    //to do 送进逻辑层
    co_return;
}



boost::asio::awaitable<void> Csession::start_heartbeat(){
    auto ptr=shared_from_this();
    if(is_clothing()||state_==Session_state::Closed)co_return;//如果连接正在断开，就直接取消定时器；
    timer_.expires_after(HEART_TIMEOUT);//设置超时时间
    boost::system::error_code ec;
    co_await timer_.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable,ec));

    if(ec){//说明在超时时间内，收到了信息，定时任务被取消了
        co_await start_heartbeat();
        co_return;
    }
    // 执行到这里说明超时了，直接关掉session的连接；
    close();
    co_return;
}

boost::asio::awaitable<size_t>Csession::ReadHead(){
    auto self =shared_from_this();
    memset(buffer_,0,Buffer_size);
    auto [ec,lenth]=co_await boost::asio::async_read(socket_,boost::asio::buffer(buffer_,HEAD_TOTAL_LEN),boost::asio::as_tuple(boost::asio::use_awaitable));
    if(ec){
        spdlog::error("session id {} read head error",uuid_);
        close();
    }
    // 转化字节序
    if(lenth!=HEAD_TOTAL_LEN){
        spdlog::error("recv head lenth false,session id is {}",uuid_);
        close();
    }
    Recv_node_->Clear();
    memcpy(Recv_node_->_data,buffer_,HEAD_TOTAL_LEN);
    short msg_id=0;
    memcpy(&msg_id,Recv_node_->_data,HEAD_ID_LEN);
    // 转成本地字节序
    msg_id=boost::asio::detail::socket_ops::network_to_host_short(msg_id);
    if(msg_id!=HEAD_ID_LEN){
        spdlog::error("session id {} read head error",uuid_);
        close();
    }
    short data_len=0;
    memcpy(&data_len,buffer_+HEAD_ID_LEN,HEAD_DATA_LEN);
    data_len=boost::asio::detail::socket_ops::network_to_host_short(data_len);
    if(data_len>Buffer_size){
        spdlog::error("session id {},recv package len is too long",uuid_);
        close();
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
        close();
        co_return;
    }
    if(recv_data_len!=len){
        spdlog::error("session id {} recv data len is error",uuid_);
        close();
    }
    memcpy(data_node_->_data,buffer_,len);
    co_return;
}