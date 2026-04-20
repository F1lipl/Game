#include"../include/WorkShard.h"
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <memory>
#include <thread>

using work=boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
WorkShard::WorkShard(){
    ConnPool_.reset(new GameServerConnPool(server,ioc_));
     worker_ = std::make_unique<work>(ioc_.get_executor());
     thread_=std::move(thread([this](){
        ioc_.run();
     }));
}
