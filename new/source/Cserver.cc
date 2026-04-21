#include "../include/Cserver.h"
#include <boost/asio/io_context.hpp>
#include <cstddef>
#include<spdlog/spdlog.h>



Cserver::Cserver(boost::asio::io_context& ioc,unsigned short port):ioc_(ioc),
port_(port),
acceptor_(ioc,{tcp::v4(),port}),
shards_(WORK_SHARD_NUMBER),
idx_(0)
{
    ioc_.run();
}

    
void Cserver::start()
{   
    for(size_t i=0;i<WORK_SHARD_NUMBER;++i){
        shards_[i].start();
    }
    StartAccept();
}

boost::asio::io_context& Cserver::get_connection(){
    auto&ioc= shards_[idx_].get_io_context();
    idx_=(idx_+1)%WORK_SHARD_NUMBER;
    return ioc;
}

//to do
void Cserver::StartAccept(){
    // asio::io_context& ioc=
    // auto ptr=std::make_shared<Csession>(this,ioc);
    // acceptor_.async_accept(ptr->get_socket(),[ptr,this](boost::system::error_code ec){
    //     if(ec){
    //         spdlog::error("connect is error,error is {}",ec.what());
    //     }
    //     else{
    //         // 启动 session
    //         boost::asio::co_spawn(ptr->get_socket().get_executor(), ptr->start(), boost::asio::detached);
    //         StartAccept();
    //     }
    // });



}