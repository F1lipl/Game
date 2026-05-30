#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

class Csession;
class SendNode;
class WorkShard;

class GateSessionRegistry {
public:
    using uid = std::uint64_t;

    static GateSessionRegistry& Instance();

    void Register(uid id, WorkShard* shard, std::shared_ptr<Csession> session);
    void Unregister(uid id, std::uint64_t session_id);

    bool SendToUid(uid id, std::shared_ptr<SendNode> node);
    std::size_t Broadcast(std::shared_ptr<SendNode> node);

private:
    struct Entry {
        WorkShard* shard {};
        std::weak_ptr<Csession> session;
        std::uint64_t session_id {};
    };

    GateSessionRegistry() = default;

    std::mutex mutex_;
    std::unordered_map<uid, Entry> sessions_;
};
