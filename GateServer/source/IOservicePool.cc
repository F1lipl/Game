#include"../include/IOservicePool.h"
#include <boost/asio/io_context.hpp>
#include <cstddef>
#include <memory>
#include <spdlog/spdlog.h>
#include <thread>

IOservicePool::IOservicePool():thread_cnt_(2*(std::thread::hardware_concurrency())),
iocontext_(2*(std::thread::hardware_concurrency())),
index_(0)
{
    spdlog::info("IOservicePool is working,threads nums is {}",thread_cnt_);
    for(size_t i=0;i<thread_cnt_;++i){
        works_[i]=std::move(std::unique_ptr<Work>(new Work(iocontext_[i].get_executor())));
        threads_.emplace_back([i,this](){
            this->iocontext_[i].run();
        });

    }
}

void IOservicePool::stop(){
    for(size_t i=0;i<thread_cnt_;++i){
        works_[i]->reset();//释放对象 
    }
    for(size_t i=0;i<thread_cnt_;++i){
        if(threads_[i].joinable()){
            threads_[i].join();
        }
    }
}

IOservicePool::~IOservicePool(){
    stop();
    spdlog::info("IoservicePool destruct");
}


boost::asio::io_context& IOservicePool::Getconnection(){
    if(index_==thread_cnt_)index_=0;
    return  iocontext_[index_++];
}