#pragma once

#include "GameServerTypes.h"
#include "GatewayLinkSession.h"
#include "GameServer.h"

#include <boost/asio.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

enum class SlotState {
    Empty,
    Accepting,
    Connected,
    Closing,
    Closed,
};

struct GatewayLinkSlot {
    SlotState state {SlotState::Empty};
    LinkId link_id {};
    std::uint64_t generation {};
    std::shared_ptr<GatewayLinkSession> session;
};

class NetworkShard {
public:
    NetworkShard(GameServer* server,
                 boost::asio::io_context& ioc,
                 std::size_t max_links);

    void Start();
    void Stop();

    std::shared_ptr<GatewayLinkSession> CreateAcceptSession();
    void OnAcceptSuccess(std::size_t slot_id, std::uint64_t generation);
    void OnAcceptFailed(std::size_t slot_id, std::uint64_t generation);

    void OnPacket(LinkId link_id,
                  MsgId msg_id,
                  SeqId seq,
                  std::shared_ptr<const RecvNode> body);

    void PostTask(NetworkTask task);

    void OnSessionClosed(std::size_t slot_id, std::uint64_t generation);

private:
    void HandleNetworkTask(NetworkTask task);

    void HandleEnterDungeon(LinkId link_id,
                            MsgId msg_id,
                            SeqId seq,
                            std::shared_ptr<const RecvNode> body);

    void HandlePlayerInput(LinkId link_id,
                           MsgId msg_id,
                           SeqId seq,
                           std::shared_ptr<const RecvNode> body);

    std::optional<std::size_t> FindAvailableSlot() const;
    std::shared_ptr<GatewayLinkSession> SelectAvailableSession();

    ShardId PickLogicShard();
    std::optional<ShardId> FindLogicShard(Uid uid) const;
    void BindUidToLogicShard(Uid uid, ShardId shard_id);
    void UnbindUid(Uid uid);

    Uid ParseUid(const std::shared_ptr<const RecvNode>& body);
    std::shared_ptr<SendNode> BuildGameToGatewayEnvelope(const NetworkTask& task);

private:
    GameServer* server_ {};
    boost::asio::io_context& ioc_;

    std::vector<GatewayLinkSlot> slots_;
    std::size_t rr_idx_ {0};
    std::size_t next_logic_idx_ {0};

    std::unordered_map<Uid, ShardId> uid_to_logic_shard_;

    bool stopping_ {false};
};
