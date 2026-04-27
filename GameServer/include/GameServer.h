#pragma once
#include "GameServerTypes.h"
// #include "LogicShard.h"

#include <boost/asio.hpp>

#include <cstddef>
#include <memory>
#include <vector>

class NetworkShard;
class GameServer {
public:
    GameServer(boost::asio::io_context& network_ioc,
               std::size_t logic_shard_count,
               std::size_t gateway_link_count);

    void Start();
    void Stop();

    std::size_t LogicShardCount() const;

    void PostToLogic(ShardId shard_id, LogicTask task);
    void PostToNetwork(NetworkTask task);

    NetworkShard& GetNetworkShard();

private:
    boost::asio::io_context& network_ioc_;

    std::unique_ptr<NetworkShard> network_shard_;
    // std::vector<std::unique_ptr<LogicShard>> logic_shards_;

    std::size_t gateway_link_count_ {};
    bool stopping_ {false};
};
