#include "../include/NetworkShard.h"
#include "../include/MsgNode.h"
#include "../../common/ProtoCodec.h"
#include "rts.pb.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <spdlog/spdlog.h>

#include <sys/socket.h> // SO_REUSEPORT

#include <chrono>
#include <string>
#include <string_view>

namespace {

// SO_REUSEPORT lets every network shard bind the same port; the kernel load
// balances incoming connections across the shards' acceptors.
using reuse_port = boost::asio::detail::socket_option::boolean<SOL_SOCKET, SO_REUSEPORT>;

std::string_view BodyView(const std::shared_ptr<const RecvNode>& body) {
    if (!body || body->_data == nullptr || body->_total_len == 0) {
        return {};
    }
    return std::string_view(body->_data, body->_total_len);
}

bool ParseGateToGameEnvelope(const std::shared_ptr<const RecvNode>& body,
                             rts::v1::GateToGameEnvelope& envelope) {
    if (!rts::protocol::ParseProtoFromBytes(BodyView(body), envelope)) {
        spdlog::warn("parse GateToGameEnvelope failed");
        return false;
    }
    if (envelope.inner_msg_id() == 0) {
        spdlog::warn("GateToGameEnvelope missing inner_msg_id");
        return false;
    }
    return true;
}

std::uint64_t NowUnixMs() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

std::shared_ptr<SendNode> BuildPacket(MsgId msg_id,
                                      const std::string& payload,
                                      std::uint16_t flags = rts::protocol::kPacketFlagNone) {
    return std::make_shared<SendNode>(
        payload.empty() ? nullptr : payload.data(),
        static_cast<std::uint32_t>(payload.size()),
        static_cast<std::uint16_t>(msg_id),
        flags);
}

std::string ExtractPacketPayload(const std::shared_ptr<SendNode>& packet) {
    if (!packet || packet->_data == nullptr || packet->_total_len == 0) {
        return {};
    }
    if (packet->_total_len >= HEAD_TOTAL_LEN) {
        const auto* data = reinterpret_cast<const unsigned char*>(packet->_data);
        const auto magic = static_cast<std::uint16_t>((data[0] << 8) | data[1]);
        if (magic == rts::protocol::kPacketMagic) {
            return std::string(packet->_data + HEAD_TOTAL_LEN,
                               packet->_total_len - HEAD_TOTAL_LEN);
        }
    }
    return std::string(packet->_data, packet->_total_len);
}

} // namespace

NetworkShard::NetworkShard(GameServer* server,
                           NetworkShardId shard_id,
                           std::size_t max_links)
    : server_(server),
      shard_id_(shard_id),
      retry_timer_(ioc_),
      slots_(max_links) {}

NetworkShard::~NetworkShard() {
    Stop();
}

void NetworkShard::Start(const std::string& listen_ip, unsigned short port) {
    using boost::asio::ip::tcp;

    stopping_ = false;
    ioc_.restart();

    acceptor_ = std::make_unique<tcp::acceptor>(ioc_);
    tcp::endpoint endpoint(boost::asio::ip::make_address(listen_ip), port);
    acceptor_->open(endpoint.protocol());
    acceptor_->set_option(tcp::acceptor::reuse_address(true));
    acceptor_->set_option(reuse_port(true));
    acceptor_->bind(endpoint);
    acceptor_->listen(boost::asio::socket_base::max_listen_connections);

    work_guard_ = std::make_unique<boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type>>(ioc_.get_executor());

    boost::asio::post(ioc_, [this]() { DoAccept(); });

    thread_ = std::thread([this]() { ioc_.run(); });
}

void NetworkShard::Stop() {
    if (stopping_.exchange(true)) {
        return;
    }

    boost::asio::post(ioc_, [this]() {
        boost::system::error_code ec;
        if (acceptor_) {
            acceptor_->cancel(ec);
            acceptor_->close(ec);
        }
        retry_timer_.cancel(ec);
        for (auto& slot : slots_) {
            if (slot.session) {
                slot.session->Close();
                slot.session.reset();
            }
            slot.state = SlotState::Closed;
        }
    });

    work_guard_.reset();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void NetworkShard::DoAccept() {
    if (stopping_ || !acceptor_ || !acceptor_->is_open()) {
        return;
    }

    auto slot_idx = FindAvailableSlot();
    if (!slot_idx.has_value()) {
        retry_timer_.expires_after(std::chrono::milliseconds(50));
        retry_timer_.async_wait([this](const boost::system::error_code& ec) {
            if (!ec && !stopping_) {
                DoAccept();
            }
        });
        return;
    }

    auto& slot = slots_[*slot_idx];
    slot.generation++;
    slot.link_id = static_cast<LinkId>(*slot_idx);
    slot.state = SlotState::Accepting;

    auto session = std::make_shared<GatewayLinkSession>(this, ioc_);
    session->BindSlot(*slot_idx, slot.generation);
    slot.session = session;

    const auto idx = *slot_idx;
    const auto gen = slot.generation;

    acceptor_->async_accept(
        session->get_socket(),
        [this, session, idx, gen](const boost::system::error_code& ec) {
            auto& s = slots_[idx];
            if (ec) {
                if (s.generation == gen) {
                    if (s.session) {
                        s.session->Close();
                        s.session.reset();
                    }
                    s.state = SlotState::Closed;
                }
                if (ec != boost::asio::error::operation_aborted && !stopping_) {
                    DoAccept();
                }
                return;
            }

            if (s.generation == gen && s.state == SlotState::Accepting && s.session) {
                s.state = SlotState::Connected;
                s.session->Start();
                spdlog::debug("net shard {} accepted link slot {}", shard_id_, idx);
            }
            if (!stopping_) {
                DoAccept();
            }
        });
}

void NetworkShard::OnSessionClosed(std::size_t slot_id, std::uint64_t generation) {
    if (slot_id >= slots_.size()) {
        return;
    }
    auto& slot = slots_[slot_id];
    if (slot.generation != generation) {
        return;
    }
    slot.state = SlotState::Closed;
    slot.session.reset();
    spdlog::debug("net shard {} link slot {} closed", shard_id_, slot_id);
}

std::optional<std::size_t> NetworkShard::FindAvailableSlot() const {
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        const auto& slot = slots_[i];
        if (slot.state == SlotState::Empty || slot.state == SlotState::Closed) {
            return i;
        }
    }
    return std::nullopt;
}

void NetworkShard::OnPacket(LinkId link_id,
                            MsgId msg_id,
                            SeqId seq,
                            std::shared_ptr<const RecvNode> body) {
    if (stopping_) {
        return;
    }

    auto dispatch_msg_id = msg_id;
    auto dispatch_seq = seq;

    if (msg_id == MsgId::GateToGameEnvelope) {
        rts::v1::GateToGameEnvelope envelope;
        if (!ParseGateToGameEnvelope(body, envelope)) {
            return;
        }
        dispatch_msg_id = static_cast<MsgId>(envelope.inner_msg_id());
        dispatch_seq = envelope.client_seq();
    }

    switch (dispatch_msg_id) {
    case MsgId::GateLinkHello:
        HandleGateLinkHello(link_id, std::move(body));
        break;
    case MsgId::PingReq:
        HandlePingReq(link_id, std::move(body));
        break;
    case MsgId::CreateRoomReq:
    case MsgId::JoinRoomReq:
    case MsgId::LeaveRoomReq:
    case MsgId::PlayerReadyReq:
    case MsgId::EnterBattleReq:
    case MsgId::MoveCmd:
    case MsgId::AttackCmd:
    case MsgId::SkillCmd:
    case MsgId::HarvestCmd:
    case MsgId::StoreResourceCmd:
    case MsgId::PickupResourceCmd:
    case MsgId::BuildCmd:
    case MsgId::ConstructCmd:
    case MsgId::TrainUnitCmd:
    case MsgId::StopCmd:
        ForwardToLogic(link_id, dispatch_msg_id, dispatch_seq, std::move(body));
        break;
    default:
        spdlog::warn("net shard {} unknown msg_id {}", shard_id_,
                     static_cast<std::uint16_t>(dispatch_msg_id));
        break;
    }
}

void NetworkShard::ForwardToLogic(LinkId link_id,
                                  MsgId msg_id,
                                  SeqId seq,
                                  std::shared_ptr<const RecvNode> body) {
    rts::v1::GateToGameEnvelope envelope;
    if (!ParseGateToGameEnvelope(body, envelope)) {
        return;
    }
    const auto uid = envelope.uid();
    if (uid == 0) {
        spdlog::warn("net shard {} forward failed: invalid uid", shard_id_);
        return;
    }
    if (!server_ || server_->LogicShardCount() == 0) {
        spdlog::warn("net shard {} forward failed: no logic shard", shard_id_);
        return;
    }

    const auto logic_shard_id = ResolveLogicShard(msg_id, envelope);

    LogicTask task;
    task.msg_id = msg_id;
    task.uid = uid;
    task.seq = seq;
    task.origin = MakeRoute(link_id);
    task.body = std::move(body);

    server_->PostToLogic(logic_shard_id, std::move(task));
}

void NetworkShard::HandleGateLinkHello(LinkId link_id,
                                       std::shared_ptr<const RecvNode> body) {
    rts::v1::GateLinkHello hello;
    if (!rts::protocol::ParseProtoFromBytes(BodyView(body), hello)) {
        spdlog::warn("net shard {} parse GateLinkHello failed on link {}", shard_id_, link_id);
        return;
    }
    spdlog::debug("net shard {} gate link hello, link={}, gate_id={}, link_index={}",
                  shard_id_, link_id, hello.gate_id(), hello.link_index());
}

void NetworkShard::HandlePingReq(LinkId link_id,
                                 std::shared_ptr<const RecvNode> body) {
    rts::v1::PingReq ping;
    if (!rts::protocol::ParseProtoFromBytes(BodyView(body), ping)) {
        spdlog::warn("net shard {} parse PingReq failed on link {}", shard_id_, link_id);
        return;
    }

    rts::v1::PongRsp pong;
    pong.set_client_time_ms(ping.client_time_ms());
    pong.set_server_time_ms(NowUnixMs());

    std::string payload;
    if (!rts::protocol::SerializeProtoToString(pong, payload)) {
        spdlog::error("net shard {} serialize PongRsp failed", shard_id_);
        return;
    }

    SendToGatewayLink(link_id, BuildPacket(MsgId::PongRsp, payload));
}

void NetworkShard::SendToGatewayLink(LinkId link_id, std::shared_ptr<SendNode> packet) {
    if (!packet || link_id >= slots_.size()) {
        return;
    }
    auto& slot = slots_[link_id];
    if (slot.state != SlotState::Connected || !slot.session || !slot.session->IsAvailable()) {
        spdlog::warn("net shard {} send to link {} failed: unavailable", shard_id_, link_id);
        return;
    }
    slot.session->PostSend(std::move(packet));
}

void NetworkShard::PostTask(NetworkTask task) {
    boost::asio::post(ioc_, [this, task = std::move(task)]() mutable {
        HandleNetworkTask(std::move(task));
    });
}

void NetworkShard::HandleNetworkTask(NetworkTask task) {
    if (stopping_) {
        return;
    }

    for (const auto& tgt : task.targets) {
        if (tgt.link_id >= slots_.size()) {
            continue;
        }
        auto& slot = slots_[tgt.link_id];
        if (slot.generation != tgt.generation ||
            slot.state != SlotState::Connected ||
            !slot.session ||
            !slot.session->IsAvailable()) {
            continue; // stale route or link gone -> drop (client reconnect handles it)
        }

        auto packet = BuildGameToGatewayEnvelope(task, tgt.uids);
        if (packet) {
            slot.session->PostSend(std::move(packet));
        }
    }
}

ShardId NetworkShard::PickLogicShard() {
    const auto count = server_ ? server_->LogicShardCount() : 0;
    if (count == 0) {
        return 0;
    }
    return static_cast<ShardId>(next_logic_idx_++ % count);
}

ShardId NetworkShard::ResolveLogicShard(MsgId msg_id,
                                        const rts::v1::GateToGameEnvelope& envelope) {
    if (!server_) {
        return 0;
    }
    const auto count = server_->LogicShardCount();
    if (count == 0) {
        return 0;
    }

    if (msg_id == MsgId::CreateRoomReq) {
        return PickLogicShard();
    }

    std::uint64_t room_id = envelope.room_id();
    if (room_id == 0) {
        rts::v1::RoomRouteHint hint;
        if (rts::protocol::ParseProtoFromBytes(envelope.payload(), hint)) {
            room_id = hint.room_id();
        }
    }
    if (room_id != 0) {
        return static_cast<ShardId>(room_id % count);
    }
    return 0;
}

GatewayRoute NetworkShard::MakeRoute(LinkId link_id) const {
    GatewayRoute route;
    route.net_shard = shard_id_;
    route.link_id = link_id;
    if (link_id < slots_.size()) {
        route.generation = slots_[link_id].generation;
    }
    return route;
}

std::shared_ptr<SendNode> NetworkShard::BuildGameToGatewayEnvelope(
    const NetworkTask& task,
    const std::vector<Uid>& target_uids) const {
    rts::v1::GameToGateEnvelope envelope;
    for (auto uid : target_uids) {
        envelope.add_target_uids(uid);
    }
    envelope.set_inner_msg_id(static_cast<std::uint16_t>(task.msg_id));
    envelope.set_server_tick(task.seq);
    envelope.set_payload(ExtractPacketPayload(task.body));

    std::string payload;
    if (!rts::protocol::SerializeProtoToString(envelope, payload)) {
        spdlog::error("net shard {} serialize GameToGateEnvelope failed", shard_id_);
        return nullptr;
    }

    return std::make_shared<SendNode>(
        payload.empty() ? nullptr : payload.data(),
        static_cast<std::uint32_t>(payload.size()),
        static_cast<std::uint16_t>(MsgId::GameToGateEnvelope));
}
