#pragma once
#include "Const.h"
#include <boost/asio/detail/call_stack.hpp>
#include <boost/asio/io_context.hpp>
#include <cstddef>
#include <thread>
#include <vector>
#include"Singleton.h"
#include<atomic>
#include<memory>
class IOservicePool:public Singleton<IOservicePool>{
    friend class Singleton<IOservicePool>;
public:
    IOservicePool();
    ~IOservicePool();
    using Work = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
    using WorkPtr = std::unique_ptr<Work>;
    boost::asio::io_context& Getconnection();
    void stop();

private:

std::vector<std::thread>threads_;
std::vector<boost::asio::io_context>iocontext_;
std::vector<WorkPtr>works_;
int thread_cnt_;
size_t index_;
};