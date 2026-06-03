#include "../include/GateProtocol.h"

#include "../include/Csession.h"
#include "../include/MsgNode.h"
#include "../include/WorkShard.h"
#include "../../common/ProtoCodec.h"
#include "rts.pb.h"

#include <cstddef>
#include <functional>
#include <limits>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>

namespace gate::protocol {
namespace {

std::string_view BodyView(const RecvNode& body) {
    if (body._data == nullptr || body._total_len == 0) {
        return {};
    }

    return std::string_view(body._data, body._total_len);
}

bool SerializeMessage(const google::protobuf::MessageLite& message, std::string& out) {
    if (!rts::protocol::SerializeProtoToString(message, out)) {
        spdlog::error("serialize protobuf message failed");
        return false;
    }

    if (out.size() > rts::protocol::kMaxPacketBodyLen) {
        spdlog::error("serialized protobuf message too large: {}", out.size());
        return false;
    }

    return true;
}

} // namespace

std::shared_ptr<SendNode> BuildPacket(std::uint16_t msg_id,
                                      const std::string& payload,
                                      std::uint16_t flags) {
    if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        spdlog::error("packet payload too large: {}", payload.size());
        return nullptr;
    }

    const char* data = payload.empty() ? nullptr : payload.data();
    return std::make_shared<SendNode>(
        data,
        static_cast<std::uint32_t>(payload.size()),
        msg_id,
        flags);
}

std::shared_ptr<SendNode> BuildGateLinkHello(std::uint32_t gate_id,
                                             std::uint32_t link_index) {
    rts::v1::GateLinkHello hello;
    hello.set_gate_id(gate_id);
    hello.set_link_index(link_index);

    std::string payload;
    if (!SerializeMessage(hello, payload)) {
        return nullptr;
    }

    return BuildPacket(
        static_cast<std::uint16_t>(rts::protocol::MsgId::GateLinkHello),
        payload);
}

std::shared_ptr<SendNode> BuildPingReq(std::uint64_t client_time_ms) {
    rts::v1::PingReq ping;
    ping.set_client_time_ms(client_time_ms);

    std::string payload;
    if (!SerializeMessage(ping, payload)) {
        return nullptr;
    }

    return BuildPacket(
        static_cast<std::uint16_t>(rts::protocol::MsgId::PingReq),
        payload);
}

std::shared_ptr<SendNode> BuildGateToGameEnvelope(const Csession& session,
                                                  std::uint16_t inner_msg_id,
                                                  const RecvNode& body) {
    rts::v1::GateToGameEnvelope envelope;
    envelope.set_uid(session.get_uid());
    envelope.set_session_id(session.get_session_id());
    envelope.set_inner_msg_id(inner_msg_id);
    envelope.set_payload(std::string(BodyView(body)));

    std::string payload;
    if (!SerializeMessage(envelope, payload)) {
        return nullptr;
    }

    return BuildPacket(
        static_cast<std::uint16_t>(rts::protocol::MsgId::GateToGameEnvelope),
        payload);
}

std::shared_ptr<SendNode> BuildCommandRejected(std::uint64_t room_id,
                                               std::uint64_t client_seq,
                                               int code,
                                               const std::string& reason) {
    rts::v1::CommandRejectedNtf rejected;
    rejected.set_room_id(room_id);
    rejected.set_client_seq(client_seq);
    rejected.set_code(static_cast<rts::v1::ErrorCode>(code));
    rejected.set_reason(reason);

    std::string payload;
    if (!SerializeMessage(rejected, payload)) {
        return nullptr;
    }

    return BuildPacket(
        static_cast<std::uint16_t>(rts::protocol::MsgId::CommandRejectedNtf),
        payload);
}

bool ForwardGameToClients(WorkShard& shard, const RecvNode& body) {
    rts::v1::GameToGateEnvelope envelope;
    if (!rts::protocol::ParseProtoFromBytes(BodyView(body), envelope)) {
        spdlog::warn("parse GameToGateEnvelope failed");
        return false;
    }

    auto packet = BuildPacket(
        static_cast<std::uint16_t>(envelope.inner_msg_id()),
        envelope.payload());
    if (!packet) {
        return false;
    }

    if (envelope.target_uids_size() == 0) {
        auto sent = shard.Broadcast(packet);
        spdlog::debug("broadcast backend msg {} to {} clients",
                      envelope.inner_msg_id(), sent);
        return sent > 0;
    }

    std::size_t sent = 0;
    for (auto uid : envelope.target_uids()) {
        if (shard.SendToUid(uid, packet)) {
            ++sent;
        } else {
            spdlog::warn("target uid {} not found for backend msg {}",
                         uid, envelope.inner_msg_id());
        }
    }

    return sent > 0;
}

std::uint64_t MakeLoginUid(const Csession& session,
                           const std::string& account,
                           const std::string& token) {
    const auto key = account.empty()
        ? session.get_uuid()
        : account + ":" + token;

    auto uid = static_cast<std::uint64_t>(std::hash<std::string>{}(key));
    if (uid == 0) {
        uid = 1;
    }

    return uid;
}

} // namespace gate::protocol
