#include "../include/Const.h"
#include <boost/asio/impl/write_at.hpp>
#include <boost/asio/io_context.hpp>
#include"../include/Cserver.h"
#include"../include/IOservicePool.h"

Cserver::Cserver(asio::io_context& context,unsigned short port):context_(context),port_(port)
,acceptor_(context,{tcp::v4(),port}){
    context_.run();
    HandleAccept();
}

void Cserver::StartAccept(){
    asio::io_context& ioc=IOservicePool::Getinstance()->Getconnection();
    tcp::socket sock(ioc);
    acceptor_.async_accept(sock,[this](){});
}


