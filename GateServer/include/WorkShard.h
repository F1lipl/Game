#pragma once
#include"Const.h"
// #include "GameServerConnPool.h"
#include <algorithm>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/detail/service_registry.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <climits>
#include <cstdint>
#include <memory>
#include <ranges>
#include <string>
#include <thread>
#include <unordered_map>
class GameServerConnPool;
class Csession;
class SendNode;
class WorkShard{
public:
    WorkShard();
    ~WorkShard();
    using uid=std::uint64_t;
    using work=boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
    boost::asio::io_context& get_io_context();
    void start();
    void stop();
    void PostMessage(std::shared_ptr<SendNode>);
    void delete_user_session(std::string name);
    boost::asio::io_context& GetConnection(){
        return ioc_;
    }
    void add_user_session(std::string name,std::shared_ptr<Csession>session){
        user_session_mgr[name]=session;
    }

    void add_uid(uid id,std::string uuid){
        user_id_mgr[id]=uuid;
    }
    void delete_uid(uid id){
        if(user_id_mgr.find(id)!=user_id_mgr.end()){
            user_id_mgr.erase(id);
        }
        return;
    }
    

private:
    boost::asio::io_context ioc_;
    std::unique_ptr<work>worker_;
    std::unique_ptr<GameServerConnPool>ConnPool_;
    std::unordered_map<std::string,std::shared_ptr<Csession>>user_session_mgr;
    std::unordered_map<uid,std::string>user_id_mgr;
    std::thread thread_;  
};