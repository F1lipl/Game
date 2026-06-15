#include"../include/WorkShard.h"
#include "../include/Csession.h"
#include "../include/GameServerConnPool.h"
#include "../include/GateProtocol.h"
#include <memory>
#include <thread>
#include <utility>

WorkShard::WorkShard() = default;

WorkShard::~WorkShard() {
    stop();
}

boost::asio::io_context& WorkShard::get_io_context() {
    return ioc_;
}

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
    if (b_stop_) {
        return;
    }
    b_stop_=true;
    worker_.reset();
    if (ConnPool_) {
        ConnPool_->Stop();
    }
    ioc_.stop();
    if(thread_.joinable())thread_.join();
}

void WorkShard::PostMessage(std::shared_ptr<SendNode>node){
    if (!ConnPool_) {
        return;
    }
    ConnPool_->PostMessage(std::move(node));
}

void WorkShard::NotifyGameServerDisconnect(uid id){
    if (!ConnPool_ || id == 0) {
        return;
    }
    auto packet = gate::protocol::BuildClientDisconnectedNtf(id);
    if (packet) {
        ConnPool_->PostMessage(std::move(packet));
    }
}

bool WorkShard::SendToUid(uid id, std::shared_ptr<SendNode> node) {
    auto session = FindSessionByUid(id);
    if (!session || !node) {
        return false;
    }

    session->SendData(std::move(node));
    return true;
}

std::size_t WorkShard::Broadcast(std::shared_ptr<SendNode> node) {
    if (!node) {
        return 0;
    }

    std::size_t sent = 0;
    for (auto& [uuid, session] : user_session_mgr) {
        if (!session) {
            continue;
        }

        session->SendData(node);
        ++sent;
    }

    return sent;
}

std::shared_ptr<Csession> WorkShard::FindSessionByUid(uid id) {
    auto uid_it = user_id_mgr.find(id);
    if (uid_it == user_id_mgr.end()) {
        return nullptr;
    }

    auto session_it = user_session_mgr.find(uid_it->second);
    if (session_it == user_session_mgr.end()) {
        return nullptr;
    }

    return session_it->second;
}

void WorkShard::delete_user_session(std::string name) {
    for (auto it = user_id_mgr.begin(); it != user_id_mgr.end();) {
        if (it->second == name) {
            it = user_id_mgr.erase(it);
        } else {
            ++it;
        }
    }

    user_session_mgr.erase(name);
}
