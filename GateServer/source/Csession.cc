#include "../include/Csession.h"
#include "../include/Cserver.h"
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <memory>


Csession::Csession(Cserver* server,boost::asio::io_context& ioc):socket_(ioc),
server_(server),
timer_(ioc),
state_(Session_state::Invalid)
{};


void Csession::start(){
    Set_state(Session_state::Conected);
    auto ptr=shared_from_this();
    last_recv_time=std::chrono::steady_clock::now();
    boost::asio::co_spawn(socket_.get_executor(),Csession::heartbeat(),boost::asio::detached);//开启心跳检测
}


boost::asio::awaitable<void> Csession::heartbeat(){




    co_return;
}