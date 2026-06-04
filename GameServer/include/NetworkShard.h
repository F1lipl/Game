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

namespace rts::v1 {
class GateToGameEnvelope;
}

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

struct GatewayRoute {
    LinkId link_id {};
    std::uint64_t generation {};
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

    void HandleRoomLifecycle(LinkId link_id,
                             MsgId msg_id,
                             SeqId seq,
                             std::shared_ptr<const RecvNode> body);

    void HandleEnterDungeon(LinkId link_id,
                            MsgId msg_id,
                            SeqId seq,
                            std::shared_ptr<const RecvNode> body);

    void HandlePlayerInput(LinkId link_id,
                           MsgId msg_id,
                           SeqId seq,
                           std::shared_ptr<const RecvNode> body);

    void HandleGateLinkHello(LinkId link_id,
                             std::shared_ptr<const RecvNode> body);

    void HandlePingReq(LinkId link_id,
                       std::shared_ptr<const RecvNode> body);

    void SendToGatewayLink(LinkId link_id, std::shared_ptr<SendNode> packet);

    std::optional<std::size_t> FindAvailableSlot() const;
    std::shared_ptr<GatewayLinkSession> SelectAvailableSession();
    std::shared_ptr<GatewayLinkSession> FindSessionByRoute(const GatewayRoute& route) const;

    ShardId PickLogicShard();
    ShardId ResolveLogicShard(MsgId msg_id,
                              Uid uid,
                              const rts::v1::GateToGameEnvelope& envelope);
    std::optional<ShardId> FindLogicShard(Uid uid) const;
    void BindUidToLogicShard(Uid uid, ShardId shard_id);
    void UnbindUid(Uid uid);
    void BindUidToGatewayLink(Uid uid, LinkId link_id);
    void UnbindGatewayLink(std::size_t slot_id, std::uint64_t generation);

    Uid ParseUid(const std::shared_ptr<const RecvNode>& body);
    std::shared_ptr<SendNode> BuildGameToGatewayEnvelope(
        const NetworkTask& task,
        const std::vector<Uid>& target_uids) const;

private:
    GameServer* server_ {};
    boost::asio::io_context& ioc_;

    std::vector<GatewayLinkSlot> slots_;
    std::size_t rr_idx_ {0};
    std::size_t next_logic_idx_ {0};

    std::unordered_map<Uid, ShardId> uid_to_logic_shard_;
    std::unordered_map<Uid, GatewayRoute> uid_to_gateway_route_;

    bool stopping_ {false};
};
