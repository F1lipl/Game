#include "../include/GameServer.h"

#include "../include/LogicRouter.h"
#include "../include/LogicShard.h"
#include "../include/NetworkShard.h"

#include <spdlog/spdlog.h>

#include <stdexcept>
#include <utility>

GameServer::GameServer(std::size_t logic_shard_count,
                       std::size_t network_shard_count,
                       std::size_t gateway_link_count)
    : logic_shard_count_(logic_shard_count),
      network_shard_count_(network_shard_count),
      gateway_link_count_(gateway_link_count) {
    if (logic_shard_count_ == 0) {
        throw std::invalid_argument("logic_shard_count must be greater than 0");
    }
    if (network_shard_count_ == 0) {
        throw std::invalid_argument("network_shard_count must be greater than 0");
    }
    if (gateway_link_count_ == 0) {
        throw std::invalid_argument("gateway_link_count must be greater than 0");
    }

    network_shards_.reserve(network_shard_count_);
    for (std::size_t i = 0; i < network_shard_count_; ++i) {
        network_shards_.push_back(std::make_unique<NetworkShard>(
            this, static_cast<NetworkShardId>(i), gateway_link_count_));
    }

    logic_shards_.reserve(logic_shard_count_);
    for (std::size_t i = 0; i < logic_shard_count_; ++i) {
        logic_shards_.push_back(std::make_unique<LogicShard>(
            this, static_cast<ShardId>(i), logic_shard_count_));
    }
}

GameServer::~GameServer() {
    Stop();
}

void GameServer::Start(const std::string& listen_ip, unsigned short port) {
    stopping_ = false;

    LogicRouter::Getinstance()->Init();

    for (auto& shard : logic_shards_) {
        if (shard) {
            shard->start();
        }
    }

    for (auto& shard : network_shards_) {
        if (shard) {
            shard->Start(listen_ip, port);
        }
    }

    spdlog::info("GameServer started, logic_shards={}, network_shards={}, gateway_links={}/shard",
                 logic_shards_.size(), network_shards_.size(), gateway_link_count_);
}

void GameServer::Stop() {
    if (stopping_) {
        return;
    }
    stopping_ = true;

    for (auto& shard : network_shards_) {
        if (shard) {
            shard->Stop();
        }
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

std::size_t GameServer::NetworkShardCount() const {
    return network_shards_.size();
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

void GameServer::PostToNetwork(NetworkShardId net_shard_id, NetworkTask task) {
    if (stopping_) {
        return;
    }
    if (net_shard_id >= network_shards_.size() || !network_shards_[net_shard_id]) {
        return;
    }
    network_shards_[net_shard_id]->PostTask(std::move(task));
}
