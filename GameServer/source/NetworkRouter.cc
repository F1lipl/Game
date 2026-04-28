#include "../include/NetworkRouter.h"
#include "../include/NetworkShard.h"

#include <spdlog/spdlog.h>

std::unordered_map<MsgId, NetworkRouter::Handler> NetworkRouter::handlers_;

void NetworkRouter::Init() {
    // Register(MsgId::StateSync, [](NetworkShard& shard, NetworkTask task) {
    //     shard.HandleStateSync(std::move(task));
    // });

    // Register(MsgId::EnterDungeonRsp, [](NetworkShard& shard, NetworkTask task) {
    //     shard.HandleEnterDungeonRsp(std::move(task));
    // });
}

void NetworkRouter::Register(MsgId msg_id, Handler handler) {
    handlers_[msg_id] = std::move(handler);
}

void NetworkRouter::HandleTask(NetworkShard& shard, NetworkTask task) {
    auto iter = handlers_.find(task.msg_id);
    if (iter == handlers_.end()) {
        spdlog::warn("unknown network msg id {}", static_cast<std::uint16_t>(task.msg_id));
        return;
    }

    iter->second(shard, std::move(task));
}
