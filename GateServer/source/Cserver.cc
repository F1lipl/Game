#include "../include/Const.h"
#include <boost/asio/awaitable.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/impl/write_at.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/system_executor.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/detail/error_code.hpp>
#include <cstdio>
#include <memory>
#include <spdlog/spdlog.h>
#include <string>
#include"../include/Cserver.h"
#include"../include/IOservicePool.h"

Cserver::Cserver(asio::io_context& context,unsigned short port):context_(context),port_(port)
,acceptor_(context,{tcp::v4(),port}),
strand_(context.get_executor())
{
    context_.run();
    StartAccept();
}

asio::awaitable<void> Cserver::Set_session_id(std::string id, std::shared_ptr<Csession> ptr)
{
    co_await asio::dispatch(strand_, asio::use_awaitable);
    session_[std::move(id)] = std::move(ptr);
    co_return;
}


asio::awaitable<std::shared_ptr<Csession>> Cserver::Get_session(std::string id){
    co_await asio::dispatch(strand_,asio::use_awaitable);
    co_return session_[id];
}

asio::awaitable<void> Cserver::Delete_session(std::string uuid){
    co_await asio::dispatch(strand_,boost::asio::use_awaitable);
    if(session_.find(uuid)!=session_.end())session_.erase(uuid);
    co_return;
}

void Cserver::StartAccept(){
    asio::io_context& ioc=IOservicePool::Getinstance()->Getconnection();
    auto ptr=std::make_shared<Csession>(this,ioc);
    acceptor_.async_accept(ptr->get_socket(),[ptr,this](boost::system::error_code ec){
        if(ec){
            spdlog::error("connect is error,error is {}",ec.what());
        }
        else{
            // 启动 session
            boost::asio::co_spawn(ptr->get_socket().get_executor(), ptr->start(), boost::asio::detached);
            StartAccept();
        }
    });

}


