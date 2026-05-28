#include "../include/NetworkShard.h"
#include "../include/MsgNode.h"
#include "../../common/ProtoCodec.h"
#include "rts.pb.h"

#include <boost/asio/post.hpp>
#include <spdlog/spdlog.h>

#include <string>
#include <string_view>

namespace {

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

std::string ExtractPacketPayload(const std::shared_ptr<SendNode>& packet) {
    if (!packet || packet->_data == nullptr || packet->_total_len == 0) {
        return {};
    }

    if (packet->_total_len >= HEAD_TOTAL_LEN) {
        const auto* data = reinterpret_cast<const unsigned char*>(packet->_data);
        const auto magic = static_cast<std::uint16_t>((data[0] << 8) | data[1]);
        if (magic == rts::protocol::kPacketMagic) {
            return std::string(
                packet->_data + HEAD_TOTAL_LEN,
                packet->_total_len - HEAD_TOTAL_LEN);
        }
    }

    return std::string(packet->_data, packet->_total_len);
}

} // namespace

NetworkShard::NetworkShard(GameServer* server,
                           boost::asio::io_context& ioc,
                           std::size_t max_links)
    : server_(server),
      ioc_(ioc),
      slots_(max_links) {}

void NetworkShard::Start() {
    stopping_ = false;
}

void NetworkShard::Stop() {
    stopping_ = true;

    for (auto& slot : slots_) {
        if (slot.session) {
            slot.state = SlotState::Closing;
            slot.session->Close();
            slot.session.reset();
        }

        slot.state = SlotState::Closed;
    }

    uid_to_logic_shard_.clear();
    rr_idx_ = 0;
    next_logic_idx_ = 0;
}

std::shared_ptr<GatewayLinkSession> NetworkShard::CreateAcceptSession() {
    if (stopping_) {
        return nullptr;
    }

    auto slot_idx = FindAvailableSlot();
    if (!slot_idx.has_value()) {
        spdlog::warn("create gateway link session failed: no free slot");
        return nullptr;
    }

    auto& slot = slots_[*slot_idx];

    slot.generation++;
    slot.link_id = static_cast<LinkId>(*slot_idx);
    slot.state = SlotState::Accepting;

    auto session = std::make_shared<GatewayLinkSession>(this, ioc_);
    session->BindSlot(*slot_idx, slot.generation);

    slot.session = session;
    return session;
}

void NetworkShard::OnAcceptSuccess(std::size_t slot_id, std::uint64_t generation) {
    if (slot_id >= slots_.size()) {
        return;
    }

    auto& slot = slots_[slot_id];

    if (slot.generation != generation || !slot.session) {
        return;
    }

    if (slot.state != SlotState::Accepting) {
        return;
    }

    slot.state = SlotState::Connected;
    slot.session->Start();

    spdlog::info("gateway link slot {} accept success", slot_id);
}

void NetworkShard::OnAcceptFailed(std::size_t slot_id, std::uint64_t generation) {
    if (slot_id >= slots_.size()) {
        return;
    }

    auto& slot = slots_[slot_id];

    if (slot.generation != generation) {
        return;
    }

    if (slot.session) {
        slot.session->Close();
        slot.session.reset();
    }

    slot.state = SlotState::Closed;

    spdlog::warn("gateway link slot {} accept failed", slot_id);
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

    spdlog::warn("gateway link slot {} closed", slot_id);
}

std::optional<std::size_t> NetworkShard::FindAvailableSlot() const {
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        const auto& slot = slots_[i];

        if (slot.state == SlotState::Empty ||
            slot.state == SlotState::Closed) {
            return i;
        }
    }

    return std::nullopt;
}

std::shared_ptr<GatewayLinkSession> NetworkShard::SelectAvailableSession() {
    if (slots_.empty()) {
        return nullptr;
    }

    const auto n = slots_.size();

    for (std::size_t i = 0; i < n; ++i) {
        const auto idx = rr_idx_++ % n;
        auto& slot = slots_[idx];

        if (slot.state == SlotState::Connected &&
            slot.session &&
            slot.session->IsAvailable()) {
            return slot.session;
        }
    }

    return nullptr;
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
    case MsgId::EnterBattleReq:
        HandleEnterDungeon(link_id, dispatch_msg_id, dispatch_seq, std::move(body));
        break;

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
        HandlePlayerInput(link_id, dispatch_msg_id, dispatch_seq, std::move(body));
        break;

    default:
        spdlog::warn("unknown msg_id {}", static_cast<std::uint16_t>(dispatch_msg_id));
        break;
    }
}

void NetworkShard::HandleEnterDungeon(LinkId link_id,
                                      MsgId msg_id,
                                      SeqId seq,
                                      std::shared_ptr<const RecvNode> body) {
    auto uid = ParseUid(body);
    if (uid == 0) {
        spdlog::warn("enter dungeon failed: invalid uid");
        return;
    }

    auto logic_shard_id = PickLogicShard();

    BindUidToLogicShard(uid, logic_shard_id);

    LogicTask task;
    task.msg_id = msg_id;
    task.uid = uid;
    task.seq = seq;
    task.body = std::move(body);

    server_->PostToLogic(logic_shard_id, std::move(task));
}

void NetworkShard::HandlePlayerInput(LinkId link_id,
                                     MsgId msg_id,
                                     SeqId seq,
                                     std::shared_ptr<const RecvNode> body) {
    auto uid = ParseUid(body);
    if (uid == 0) {
        spdlog::warn("player input failed: invalid uid");
        return;
    }

    auto logic_shard_id = FindLogicShard(uid);
    if (!logic_shard_id.has_value()) {
        spdlog::warn("player input failed: uid {} has no logic shard", uid);
        return;
    }

    LogicTask task;
    task.msg_id = msg_id;
    task.uid = uid;
    task.seq = seq;
    task.body = std::move(body);

    server_->PostToLogic(*logic_shard_id, std::move(task));
}

void NetworkShard::PostTask(NetworkTask task) {
    boost::asio::post(
        ioc_.get_executor(),
        [this, task = std::move(task)]() mutable {
            HandleNetworkTask(std::move(task));
        });
}

void NetworkShard::HandleNetworkTask(NetworkTask task) {
    if (stopping_) {
        return;
    }

    auto session = SelectAvailableSession();
    if (!session) {
        spdlog::error("send to gateway failed: no available gateway link");
        return;
    }

    auto packet = BuildGameToGatewayEnvelope(task);
    if (!packet) {
        spdlog::error("send to gateway failed: build envelope failed");
        return;
    }

    session->PostSend(std::move(packet));
}

ShardId NetworkShard::PickLogicShard() {
    if (!server_) {
        return 0;
    }

    const auto count = server_->LogicShardCount();
    if (count == 0) {
        return 0;
    }

    const auto id = next_logic_idx_++ % count;
    return static_cast<ShardId>(id);
}

std::optional<ShardId> NetworkShard::FindLogicShard(Uid uid) const {
    auto it = uid_to_logic_shard_.find(uid);
    if (it == uid_to_logic_shard_.end()) {
        return std::nullopt;
    }

    return it->second;
}

void NetworkShard::BindUidToLogicShard(Uid uid, ShardId shard_id) {
    uid_to_logic_shard_[uid] = shard_id;
}

void NetworkShard::UnbindUid(Uid uid) {
    uid_to_logic_shard_.erase(uid);
}

Uid NetworkShard::ParseUid(const std::shared_ptr<const RecvNode>& body) {
    // TODO:
    // 从 protobuf GatewayToGameEnvelope 中解析 uid。
    //
    // message GatewayToGameEnvelope {
    //   uint64 uid = 1;
    //   uint32 inner_msg_id = 2;
    //   bytes payload = 3;
    // }
    rts::v1::GateToGameEnvelope envelope;
    if (!ParseGateToGameEnvelope(body, envelope)) {
        return 0;
    }

    return envelope.uid();
}

std::shared_ptr<SendNode> NetworkShard::BuildGameToGatewayEnvelope(const NetworkTask& task) {
    // TODO:
    // 构造 GameToGatewayEnvelope:
    //
    // message GameToGatewayEnvelope {
    //   repeated uint64 target_uids = 1;
    //   uint32 inner_msg_id = 2;
    //   bytes payload = 3;
    // }
    //
    // 如果 task.body 已经是完整可发包，可以第一版直接返回。
    rts::v1::GameToGateEnvelope envelope;
    for (auto uid : task.target_uids) {
        envelope.add_target_uids(uid);
    }
    envelope.set_inner_msg_id(static_cast<std::uint16_t>(task.msg_id));
    envelope.set_server_tick(task.seq);
    envelope.set_payload(ExtractPacketPayload(task.body));

    std::string payload;
    if (!rts::protocol::SerializeProtoToString(envelope, payload)) {
        spdlog::error("serialize GameToGateEnvelope failed");
        return nullptr;
    }

    return std::make_shared<SendNode>(
        payload.empty() ? nullptr : payload.data(),
        static_cast<std::uint32_t>(payload.size()),
        static_cast<std::uint16_t>(MsgId::GameToGateEnvelope));
}
