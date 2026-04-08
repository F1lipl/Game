#pragma once
#include"Const.h"
#include <boost/asio/io_context.hpp>
#include <mutex>
#include <unordered_map>
#include "Csession.h"


namespace asio=boost::asio;
using boost::asio::ip::tcp;

class Csession;

class Cserver{
public:
    Cserver(asio::io_context& context,unsigned short port);
    ~Cserver();

private:
    void HandleAccept();
    void StartAccept();
    asio::io_context& context_;
    tcp::acceptor acceptor_;
    std::unordered_map<std::string, Csession>session_;
    std::mutex mutex_;
    unsigned short port_;



};