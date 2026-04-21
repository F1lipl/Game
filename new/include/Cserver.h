#pragma once
#include "Const.h"
#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include<boost/asio/experimental/channel.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio.hpp>
#include <cstddef>
#include <vector>
#include"WorkShard.h"

namespace asio=boost::asio;
using boost::asio::ip::tcp;

class WorkShard;

class Cserver{
public:
    Cserver(asio::io_context& ioc,unsigned short port);

    void start();
private:
    void StartAccept();
    void StartConnect();
    boost::asio::io_context& get_connection();
    asio::io_context& ioc_;
    tcp::acceptor acceptor_;
    unsigned short port_;
    std::vector<WorkShard>shards_;
    size_t idx_;
};