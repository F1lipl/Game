#include "../include/Cserver.h"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <cstddef>
#include<spdlog/spdlog.h>
#include"../include/Csession.h"
#include "../include/Router.h"


Cserver::Cserver(boost::asio::io_context& ioc,unsigned short port):ioc_(ioc),
port_(port),
acceptor_(ioc,{tcp::v4(),port}),
shards_(WORK_SHARD_NUMBER),
idx_(0)
{
}

    
void Cserver::start()
{   
    stopping_ = false;
    ClientIngressRouter::Init();
    BackendIngressRouter::Init();

    for(size_t i=0;i<WORK_SHARD_NUMBER;++i){
        shards_[i].start();
    }
    StartAccept();
}

void Cserver::stop()
{
    if (stopping_) {
        return;
    }

    stopping_ = true;

    boost::system::error_code ec;
    acceptor_.cancel(ec);
    ec.clear();
    acceptor_.close(ec);

    for (auto& shard : shards_) {
        shard.stop();
    }
}

WorkShard* Cserver::get_shard(){
    auto * shard=&shards_[idx_];
    idx_=(idx_+1)%WORK_SHARD_NUMBER;
    return shard;
}

//to do
void Cserver::StartAccept(){
    if (stopping_ || !acceptor_.is_open()) {
        return;
    }

    auto* shard=get_shard();
    auto&ioc= shard->get_io_context();
    auto ptr=std::make_shared<Csession>(shard,ioc);
    acceptor_.async_accept(ptr->get_socket(),[shard,ptr,this](boost::system::error_code ec){
        if(ec){
            if (stopping_ || ec == boost::asio::error::operation_aborted) {
                return;
            }
            spdlog::error("connect is error,error is {}",ec.message());
        }
        else{
            shard->add_user_session(ptr->get_uuid(), ptr);
            ptr->start();
        }

        StartAccept();
    });


}
