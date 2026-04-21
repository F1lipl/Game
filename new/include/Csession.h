#pragma once
#include "Const.h"
#include "WorkShard.h"
#include <boost/asio/io_context.hpp>
#include<boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>
#include <boost/system/detail/error_code.hpp>
#include <spdlog/spdlog.h>

class Csession:public std::enable_shared_from_this<Csession>
{

public:
    Csession(WorkShard* shard,boost::asio::io_context& ioc);
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
    void close(){
        if (state_== Session_state::Closing ||
        state_ == Session_state::Closed) {
        return;
        }
        Set_state(Session_state::Closing);
        spdlog::info("sockset {} is closing",uuid_);
        timer_.cancel();
        boost::system::error_code ec;
        socket_.cancel(ec);
        if(ec){
            spdlog::error("session {} cancel error {}",uuid_,ec.message());
        }
        ec.clear();
        socket_.close(ec);
        if(ec){
            spdlog::error("session {} close error {}",uuid_,ec.message());
        }
        Set_state(Session_state::Closed);
        spdlog::info("session {} is closed",uuid_);
    }



private:
    WorkShard* shard_;
    std::string uuid_;
    boost::asio::ip::tcp::socket socket_;
    char* buffer_;
    boost::asio::steady_timer timer_;
    uint8_t state_;//状态
};