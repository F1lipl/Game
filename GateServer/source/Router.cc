#include "../include/Router.h"

#include "../include/Csession.h"
#include "../include/GateProtocol.h"
#include "../include/MsgNode.h"
#include "../include/WorkShard.h"
#include "../../common/ProtoCodec.h"
#include "rts.pb.h"

#include <cstdint>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>
#include <utility>

std::unordered_map<std::uint16_t, ClientIngressRouter::Handler>
    ClientIngressRouter::callbacks_;

std::unordered_map<std::uint16_t, BackendIngressRouter::Handler>
    BackendIngressRouter::callbacks_;

namespace {

std::string_view BodyView(const RecvNode& body) {
    if (body._data == nullptr || body._total_len == 0) {
        return {};
    }

    return std::string_view(body._data, body._total_len);
}

void SendToClient(const ClientIngressRouter::MsgContext& ctx,
                  std::shared_ptr<SendNode> packet) {
    if (!ctx.session || !packet) {
        return;
    }

    ctx.session->SendData(std::move(packet));
}

void HandleLoginReq(const ClientIngressRouter::MsgContext& ctx) {
    if (!ctx.session || !ctx.body) {
        return;
    }

    rts::v1::LoginReq req;
    rts::v1::LoginRsp rsp;
    if (!rts::protocol::ParseProtoFromBytes(BodyView(*ctx.body), req)) {
        rsp.set_code(rts::v1::ERROR_INVALID_REQUEST);
        rsp.set_reason("invalid LoginReq protobuf body");
        std::string payload;
        if (rsp.SerializeToString(&payload)) {
            SendToClient(ctx, gate::protocol::BuildPacket(
                static_cast<std::uint16_t>(rts::protocol::MsgId::LoginRsp),
                payload));
        }
        return;
    }

    const auto uid = gate::protocol::MakeLoginUid(
        *ctx.session,
        req.account(),
        req.token());
    ctx.session->BindUid(uid);

    rsp.set_code(rts::v1::ERROR_NONE);
    rsp.set_uid(uid);

    std::string payload;
    if (!rsp.SerializeToString(&payload)) {
        spdlog::error("serialize LoginRsp failed");
        return;
    }

    SendToClient(ctx, gate::protocol::BuildPacket(
        static_cast<std::uint16_t>(rts::protocol::MsgId::LoginRsp),
        payload));
}

void ForwardClientMsgToGame(std::uint16_t msgid,
                            const ClientIngressRouter::MsgContext& ctx) {
    if (!ctx.shard || !ctx.session || !ctx.body) {
        return;
    }

    if (ctx.session->get_uid() == 0) {
        auto rejected = gate::protocol::BuildCommandRejected(
            0,
            0,
            static_cast<int>(rts::v1::ERROR_NOT_LOGGED_IN),
            "client is not logged in");
        SendToClient(ctx, std::move(rejected));
        return;
    }

    auto packet = gate::protocol::BuildGateToGameEnvelope(
        *ctx.session,
        msgid,
        *ctx.body);
    if (!packet) {
        return;
    }

    ctx.shard->PostMessage(std::move(packet));
}

void RegisterForward(std::uint16_t msgid) {
    ClientIngressRouter::RegisterCallback(
        msgid,
        [msgid](const ClientIngressRouter::MsgContext& ctx) {
            ForwardClientMsgToGame(msgid, ctx);
        });
}

} // namespace

void ClientIngressRouter::Init() {
    RegisterCallback(static_cast<std::uint16_t>(rts::protocol::MsgId::LoginReq),
        [](const MsgContext& ctx) {
        HandleLoginReq(ctx);
    });

    RegisterForward(static_cast<std::uint16_t>(rts::protocol::MsgId::CreateRoomReq));
    RegisterForward(static_cast<std::uint16_t>(rts::protocol::MsgId::JoinRoomReq));
    RegisterForward(static_cast<std::uint16_t>(rts::protocol::MsgId::LeaveRoomReq));
    RegisterForward(static_cast<std::uint16_t>(rts::protocol::MsgId::PlayerReadyReq));
    RegisterForward(static_cast<std::uint16_t>(rts::protocol::MsgId::EnterBattleReq));
    RegisterForward(static_cast<std::uint16_t>(rts::protocol::MsgId::MoveCmd));
    RegisterForward(static_cast<std::uint16_t>(rts::protocol::MsgId::AttackCmd));
    RegisterForward(static_cast<std::uint16_t>(rts::protocol::MsgId::SkillCmd));
    RegisterForward(static_cast<std::uint16_t>(rts::protocol::MsgId::HarvestCmd));
    RegisterForward(static_cast<std::uint16_t>(rts::protocol::MsgId::StoreResourceCmd));
    RegisterForward(static_cast<std::uint16_t>(rts::protocol::MsgId::PickupResourceCmd));
    RegisterForward(static_cast<std::uint16_t>(rts::protocol::MsgId::BuildCmd));
    RegisterForward(static_cast<std::uint16_t>(rts::protocol::MsgId::ConstructCmd));
    RegisterForward(static_cast<std::uint16_t>(rts::protocol::MsgId::TrainUnitCmd));
    RegisterForward(static_cast<std::uint16_t>(rts::protocol::MsgId::StopCmd));
}

void ClientIngressRouter::HandleMsg(std::uint16_t msgid, const MsgContext& context) {
    auto it = callbacks_.find(msgid);
    if (it == callbacks_.end()) {
        spdlog::warn("ClientIngressRouter unknown msgid {}", msgid);
        return;
    }
    it->second(context);
}

void ClientIngressRouter::RegisterCallback(std::uint16_t msgid, Handler handler) {
    callbacks_[msgid] = std::move(handler);
}

void BackendIngressRouter::Init() {
    RegisterCallback(static_cast<std::uint16_t>(rts::protocol::MsgId::GameToGateEnvelope),
        [](const BackendMsgContext& ctx) {
        if (!ctx.shard || !ctx.body) {
            return;
        }

        gate::protocol::ForwardGameToClients(*ctx.shard, *ctx.body);
    });
}

void BackendIngressRouter::HandleMsg(std::uint16_t msgid, const BackendMsgContext& context) {
    auto it = callbacks_.find(msgid);
    if (it == callbacks_.end()) {
        spdlog::warn("BackendIngressRouter unknown msgid {}", msgid);
        return;
    }
    it->second(context);
}

void BackendIngressRouter::RegisterCallback(std::uint16_t msgid, Handler handler) {
    callbacks_[msgid] = std::move(handler);
}
