#include "../include/GateSessionRegistry.h"

#include "../include/Csession.h"
#include "../include/WorkShard.h"

#include <boost/asio/post.hpp>
#include <spdlog/spdlog.h>

#include <utility>
#include <vector>

GateSessionRegistry& GateSessionRegistry::Instance() {
    static GateSessionRegistry registry;
    return registry;
}

void GateSessionRegistry::Register(uid id,
                                   WorkShard* shard,
                                   std::shared_ptr<Csession> session) {
    if (id == 0 || shard == nullptr || !session) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    sessions_[id] = Entry {
        shard,
        session,
        session->get_session_id(),
    };
}

void GateSessionRegistry::Unregister(uid id, std::uint64_t session_id) {
    if (id == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) {
        return;
    }

    if (session_id != 0 && it->second.session_id != session_id) {
        return;
    }

    sessions_.erase(it);
}

bool GateSessionRegistry::SendToUid(uid id, std::shared_ptr<SendNode> node) {
    if (id == 0 || !node) {
        return false;
    }

    WorkShard* target_shard = nullptr;
    std::shared_ptr<Csession> target_session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(id);
        if (it == sessions_.end()) {
            return false;
        }

        target_session = it->second.session.lock();
        if (!target_session) {
            sessions_.erase(it);
            return false;
        }

        target_shard = it->second.shard;
    }

    if (target_shard == nullptr) {
        return false;
    }

    boost::asio::post(
        target_shard->get_io_context(),
        [target_session = std::move(target_session),
         node = std::move(node)]() mutable {
            target_session->SendData(std::move(node));
        });

    return true;
}

std::size_t GateSessionRegistry::Broadcast(std::shared_ptr<SendNode> node) {
    if (!node) {
        return 0;
    }

    std::vector<std::pair<WorkShard*, std::shared_ptr<Csession>>> targets;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            auto session = it->second.session.lock();
            if (!session) {
                it = sessions_.erase(it);
                continue;
            }

            if (it->second.shard != nullptr) {
                targets.emplace_back(it->second.shard, std::move(session));
            }

            ++it;
        }
    }

    for (auto& [shard, session] : targets) {
        boost::asio::post(
            shard->get_io_context(),
            [session = std::move(session), node]() mutable {
                session->SendData(node);
            });
    }

    return targets.size();
}
