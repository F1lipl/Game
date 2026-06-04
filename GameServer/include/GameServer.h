#pragma once
#include "GameServerTypes.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

class NetworkShard;
class LogicShard;

class GameServer {
public:
    GameServer(std::size_t logic_shard_count,
               std::size_t network_shard_count,
               std::size_t gateway_link_count);
    ~GameServer();

    void Start(const std::string& listen_ip, unsigned short port);
    void Stop();

    std::size_t LogicShardCount() const;
    std::size_t NetworkShardCount() const;

    void PostToLogic(ShardId shard_id, LogicTask task);
    void PostToNetwork(NetworkShardId net_shard_id, NetworkTask task);

private:
    std::vector<std::unique_ptr<NetworkShard>> network_shards_;
    std::vector<std::unique_ptr<LogicShard>> logic_shards_;

    std::size_t logic_shard_count_ {};
    std::size_t network_shard_count_ {};
    std::size_t gateway_link_count_ {};
    bool stopping_ {false};
};
