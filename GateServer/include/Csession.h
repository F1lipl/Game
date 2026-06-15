#pragma once
#include "Const.h"
#include "WorkShard.h"
#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include<boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>
#include <boost/system/detail/error_code.hpp>
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <queue>
#include <spdlog/spdlog.h>

class WorkShard;
class RecvNode;
class SendNode;
class MsgNode;
class Csession:public std::enable_shared_from_this<Csession>
{

public:
    using uid=std::uint64_t;
    Csession(WorkShard* shard,boost::asio::io_context& ioc);
    ~Csession();
    boost::asio::ip::tcp::socket& get_socket(){
        return socket_;
    }
    
    void Set_state(uint8_t state){
        state_=state;
    }
     bool is_clothing()const{
        return state_==Session_state::Closing;
    }
    bool is_connectd() const{
        return state_==Session_state::Conected;
    }
    void close();
    void start();
    //to do post_to_queue 逻辑层调用，把解析的包回给队列里
    std::string get_uuid() const {
        return uuid_;
    }
    uid get_uid()const{
        return uid_;
    }
    std::uint64_t get_session_id() const {
        return static_cast<std::uint64_t>(std::hash<std::string>{}(uuid_));
    }
    void BindUid(uid id);
    void SendData(std::shared_ptr<SendNode>);

private:
    boost::asio::awaitable<void>handle_read();
    boost::asio::awaitable<void> start_heartbeat();//心跳检测 
    boost::asio::awaitable<size_t> ReadHead();
    boost::asio::awaitable<bool>ReadData(size_t len);
    boost::asio::awaitable<void>start_write_loop();
    WorkShard* shard_;
    std::string uuid_;
    boost::asio::ip::tcp::socket socket_;
    char* buffer_;
    boost::asio::steady_timer timer_;
    uint8_t state_;//状态
    std::shared_ptr<RecvNode>Recv_node_;//包头的缓冲区
    std::shared_ptr<RecvNode>data_node_;//数据的缓冲区
    std::queue<std::shared_ptr<SendNode>>send_que_;
    std::uint64_t send_dropped_ {}; // 背压丢包计数
    bool is_writing_;
    uid uid_;
};
