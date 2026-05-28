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

struct LogicTask {
    MsgId msg_id {};
    Uid uid {};
    SeqId seq {};
    std::uint64_t client_frame {};
    std::shared_ptr<const RecvNode> body;
};

struct NetworkTask {
    MsgId msg_id {};
    SeqId seq {};
    std::vector<Uid> target_uids;
    std::shared_ptr<SendNode> body;
};
