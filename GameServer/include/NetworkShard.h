#pragma once
#include "GameServerTypes.h"
#include "GatewayLinkSession.h"
#include "GameServer.h"
#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
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

// One network shard = one io_context + one thread + one acceptor (SO_REUSEPORT)
// + its own slice of gateway link slots. Everything runs on its single thread,
// so its state is lock-free. uid->route lives in the logic shards, not here.
class NetworkShard {
public:
    NetworkShard(GameServer* server,
                 NetworkShardId shard_id,
                 std::size_t max_links);
    ~NetworkShard();

    void Start(const std::string& listen_ip, unsigned short port);
    void Stop();

    NetworkShardId Id() const { return shard_id_; }

    // egress: posted from logic shards, runs on this shard's thread
    void PostTask(NetworkTask task);

    // called by GatewayLinkSession on this shard's thread
    void OnPacket(LinkId link_id,
                  MsgId msg_id,
                  SeqId seq,
                  std::shared_ptr<const RecvNode> body);
    void OnSessionClosed(std::size_t slot_id, std::uint64_t generation);

private:
    void DoAccept();
    void HandleNetworkTask(NetworkTask task);

    void ForwardToLogic(LinkId link_id,
                        MsgId msg_id,
                        SeqId seq,
                        std::shared_ptr<const RecvNode> body);
    void HandleGateLinkHello(LinkId link_id,
                             std::shared_ptr<const RecvNode> body);
    void HandlePingReq(LinkId link_id,
                       std::shared_ptr<const RecvNode> body);

    void SendToGatewayLink(LinkId link_id, std::shared_ptr<SendNode> packet);

    std::optional<std::size_t> FindAvailableSlot() const;
    ShardId PickLogicShard();
    ShardId ResolveLogicShard(MsgId msg_id, const rts::v1::GateToGameEnvelope& envelope);
    GatewayRoute MakeRoute(LinkId link_id) const;
    std::shared_ptr<SendNode> BuildGameToGatewayEnvelope(
        const NetworkTask& task,
        const std::vector<Uid>& target_uids) const;

private:
    GameServer* server_ {};
    NetworkShardId shard_id_ {};

    boost::asio::io_context ioc_;
    std::unique_ptr<boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type>> work_guard_;
    std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
    boost::asio::steady_timer retry_timer_;
    std::thread thread_;

    std::vector<GatewayLinkSlot> slots_;
    std::size_t rr_idx_ {0};
    std::size_t next_logic_idx_ {0};

    std::atomic<bool> stopping_ {true};
};
