#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include "../../common/Protocol.h"

class RecvNode;
class SendNode;

using Uid = std::uint64_t;
using LinkId = std::uint32_t;
using ShardId = std::uint32_t;
using NetworkShardId = std::uint32_t;
using SeqId = std::uint64_t;

using MsgId = rts::protocol::MsgId;

// 回包用的"路由坐标"(纯数字): uid 的连接在哪个网络 shard 的哪条链路上。
// 由网络 shard 在入站时填好, 存进逻辑 shard; 回包时按它找到 session。
struct GatewayRoute {
    NetworkShardId net_shard {};
    LinkId link_id {};
    std::uint64_t generation {};
};

struct LogicTask {
    MsgId msg_id {};
    Uid uid {};
    SeqId seq {};
    std::uint64_t client_frame {};
    GatewayRoute origin {};
    std::shared_ptr<const RecvNode> body;
};

// 出站时按链路分组: 同一条链路上的多个 uid 合成一个 GameToGateEnvelope。
struct NetworkSendTarget {
    LinkId link_id {};
    std::uint64_t generation {};
    std::vector<Uid> uids;
};

// 一个 NetworkTask 只发往单个网络 shard (其内含若干链路目标)。
struct NetworkTask {
    MsgId msg_id {};
    SeqId seq {};
    std::vector<NetworkSendTarget> targets;
    std::shared_ptr<SendNode> body;
};
