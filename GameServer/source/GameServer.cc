#include "../include/GameServer.h"

#include "../include/LogicRouter.h"
#include "../include/LogicShard.h"
#include "../include/NetworkShard.h"

#include <spdlog/spdlog.h>

#include <stdexcept>
#include <utility>

GameServer::GameServer(boost::asio::io_context& network_ioc,
                       std::size_t logic_shard_count,
                       std::size_t gateway_link_count)
    : network_ioc_(network_ioc),
      logic_shard_count_(logic_shard_count),
      gateway_link_count_(gateway_link_count) {
    if (logic_shard_count_ == 0) {
        throw std::invalid_argument("logic_shard_count must be greater than 0");
    }

    if (gateway_link_count_ == 0) {
        throw std::invalid_argument("gateway_link_count must be greater than 0");
    }

    network_shard_ = std::make_unique<NetworkShard>(
        this,
        network_ioc_,
        gateway_link_count_);

    logic_shards_.reserve(logic_shard_count_);
    for (std::size_t i = 0; i < logic_shard_count_; ++i) {
        logic_shards_.push_back(std::make_unique<LogicShard>(this));
    }
}

GameServer::~GameServer() {
    Stop();
}

void GameServer::Start() {
    if (stopping_) {
        stopping_ = false;
    }

    LogicRouter::Getinstance()->Init();

    if (network_shard_) {
        network_shard_->Start();
    }

    for (auto& shard : logic_shards_) {
        if (shard) {
            shard->start();
        }
    }

    spdlog::info("GameServer started, logic_shards={}, gateway_links={}",
                 logic_shards_.size(), gateway_link_count_);
}

void GameServer::Stop() {
    if (stopping_) {
        return;
    }

    stopping_ = true;

    if (network_shard_) {
        network_shard_->Stop();
    }

    for (auto& shard : logic_shards_) {
        if (shard) {
            shard->stop();
        }
    }

    spdlog::info("GameServer stopped");
}

std::size_t GameServer::LogicShardCount() const {
    return logic_shards_.size();
}

void GameServer::PostToLogic(ShardId shard_id, LogicTask task) {
    if (stopping_) {
        return;
    }

    if (shard_id >= logic_shards_.size() || !logic_shards_[shard_id]) {
        spdlog::warn("PostToLogic failed: invalid shard id {}", shard_id);
        return;
    }

    logic_shards_[shard_id]->postTask(std::move(task));
}

void GameServer::PostToNetwork(NetworkTask task) {
    if (stopping_ || !network_shard_) {
        return;
    }

    network_shard_->PostTask(std::move(task));
}

NetworkShard& GameServer::GetNetworkShard() {
    if (!network_shard_) {
        throw std::runtime_error("NetworkShard is not initialized");
    }

    return *network_shard_;
}
