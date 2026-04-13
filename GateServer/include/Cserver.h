#pragma once
#include"Const.h"
#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include "Csession.h"
#include <boost/asio/experimental/awaitable_operators.hpp>
#include<boost/asio/experimental/channel.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio.hpp>
namespace asio=boost::asio;
using boost::asio::ip::tcp;

class Csession;

class Cserver{
public:
    Cserver(asio::io_context& context,unsigned short port);
    ~Cserver();
    asio::awaitable<void> Set_session_id(std::string,std::shared_ptr<Csession>);
    asio::awaitable<std::shared_ptr<Csession>> Get_session(std::string);
    asio::awaitable<void>Delete_session(std::string uuid);

private:
    void StartAccept();
    asio::io_context& context_;
    tcp::acceptor acceptor_;
    std::unordered_map<std::string, std::shared_ptr<Csession>>session_;
    // std::mutex mutex_;//管理session类，因为session是跑在不同线程的，所以对map进行修改的时候需要加锁,但是因为用协程去并发，所以千万不能加传统的阻塞mutex
    boost::asio::strand<boost::asio::io_context::executor_type>strand_;//这是boost库提供的串行执行器，保证协程串行的执行函数
    unsigned short port_;




};