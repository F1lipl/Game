#pragma once

#include "GameServerTypes.h"

#include <functional>
#include <unordered_map>

class NetworkShard;

class NetworkRouter {
public:
    using Handler = std::function<void(NetworkShard&, NetworkTask)>;

    static void Init();

    static void HandleTask(NetworkShard& shard, NetworkTask task);

private:
    static void Register(MsgId msg_id, Handler handler);

private:
    static std::unordered_map<MsgId, Handler> handlers_;
};
