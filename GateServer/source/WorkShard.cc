#include"../include/WorkShard.h"
#include "../include/GameServerConnPool.h"
#include <memory>
#include <thread>
#include <utility>




void WorkShard::start(){
    if(!b_stop_)return;
    b_stop_=false;
    ConnPool_=std::make_unique<GameServerConnPool>(this,ioc_);
    ConnPool_->Init();
    worker_=std::make_unique<work>(ioc_.get_executor());
    thread_=std::move(std::thread([this](){
        ioc_.run();
    }));
}

void WorkShard::stop(){
    b_stop_=true;
    worker_.reset();
    ConnPool_->Stop();
    ioc_.stop();
    if(thread_.joinable())thread_.join();
}

void WorkShard::PostMessage(std::shared_ptr<SendNode>node){
    ConnPool_->PostMessage(std::move(node));
}
