#include"../include/LogicShard.h"
#include"../include/LogicRouter.h"
#include"../include/MsgNode.h"
#include "../../common/ProtoCodec.h"
#include "rts.pb.h"

#include <algorithm>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <cstdint>
#include <memory>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

namespace {

constexpr std::uint32_t kDefaultMaxPlayers = 2;
constexpr std::uint32_t kMaxRoomPlayers = 4;
constexpr std::uint32_t kDefaultTickRate = 20;
constexpr std::uint32_t kDefaultSnapshotRate = 2;

std::string_view BodyView(const std::shared_ptr<const RecvNode>& body) {
    if (!body || body->_data == nullptr || body->_total_len == 0) {
        return {};
    }

    return std::string_view(body->_data, body->_total_len);
}

bool ParseEnvelope(const LogicTask& task, rts::v1::GateToGameEnvelope& envelope) {
    if (!rts::protocol::ParseProtoFromBytes(BodyView(task.body), envelope)) {
        spdlog::warn("logic parse GateToGameEnvelope failed, msg={}",
                     static_cast<std::uint16_t>(task.msg_id));
        return false;
    }

    return true;
}

template <typename Message>
bool ParseInnerPayload(const rts::v1::GateToGameEnvelope& envelope, Message& message) {
    return rts::protocol::ParseProtoFromBytes(envelope.payload(), message);
}

rts::v1::Team ToProtoTeam(std::uint32_t team) {
    switch (team) {
    case 0:
        return rts::v1::TEAM_PLAYER;
    case 1:
        return rts::v1::TEAM_ENEMY1;
    case 2:
        return rts::v1::TEAM_ENEMY2;
    case 3:
        return rts::v1::TEAM_ENEMY3;
    default:
        return rts::v1::TEAM_NONE;
    }
}

std::uint32_t NormalizeMaxPlayers(std::uint32_t max_players) {
    if (max_players == 0) {
        return kDefaultMaxPlayers;
    }

    return std::min(max_players, kMaxRoomPlayers);
}

std::uint64_t ResolveRoomId(const rts::v1::GateToGameEnvelope& envelope,
                            const std::unordered_map<Uid, std::uint64_t>& uid_to_room,
                            Uid uid) {
    if (envelope.room_id() != 0) {
        return envelope.room_id();
    }

    auto uid_it = uid_to_room.find(uid);
    if (uid_it != uid_to_room.end()) {
        return uid_it->second;
    }

    return 0;
}

} // namespace


LogicShard::LogicShard(GameServer* server)
    : server_(server),
      b_stop_(false) {}

void LogicShard::start() {
    if (thread_.joinable()) {
        return;
    }

    b_stop_ = false;
    ioc_.restart();

    work_guard_ =
        std::make_unique<boost::asio::executor_work_guard<
            boost::asio::io_context::executor_type>>(ioc_.get_executor());

    thread_ = std::thread([this]() {
        ioc_.run();
    });
}

void LogicShard::stop() {
    b_stop_ = true;

    work_guard_.reset();
    ioc_.stop();

    if (thread_.joinable()) {
        thread_.join();
    }
}

void LogicShard::postTask(LogicTask task) {
    if (b_stop_) {
        return;
    }

    boost::asio::post(ioc_.get_executor(),
        [this, task = std::move(task)]() mutable {
            this->handleTask(std::move(task));
        });
}

void LogicShard::handleTask(LogicTask task) {
    bool ok=LogicRouter::Getinstance()->Dispatch(this,std::move(task));
    if(!ok){
        spdlog::debug("call back undefined msgid");
    }
    spdlog::debug("handle msg success");
}

bool LogicShard::SendToPlayer(MsgId msg_id,
                              Uid uid,
                              const google::protobuf::MessageLite& message,
                              SeqId server_seq) {
    if (!server_) {
        return false;
    }

    std::string payload;
    if (!rts::protocol::SerializeProtoToString(message, payload)) {
        spdlog::error("serialize logic response failed, msg={}",
                      static_cast<std::uint16_t>(msg_id));
        return false;
    }

    auto packet = std::make_shared<SendNode>(
        payload.empty() ? nullptr : payload.data(),
        static_cast<std::uint32_t>(payload.size()),
        static_cast<std::uint16_t>(msg_id));

    NetworkTask network_task;
    network_task.msg_id = msg_id;
    network_task.seq = server_seq;
    network_task.target_uids.push_back(uid);
    network_task.body = std::move(packet);

    server_->PostToNetwork(std::move(network_task));
    return true;
}

void LogicShard::SendCommandRejected(Uid uid,
                                     std::uint64_t room_id,
                                     SeqId client_seq,
                                     rts::v1::ErrorCode code,
                                     const std::string& reason) {
    rts::v1::CommandRejectedNtf ntf;
    ntf.set_room_id(room_id);
    ntf.set_client_seq(client_seq);
    ntf.set_code(code);
    ntf.set_reason(reason);

    SendToPlayer(MsgId::CommandRejectedNtf, uid, ntf);
}

void LogicShard::BroadcastRoomState(const DungeonRoom& room) {
    rts::v1::RoomStateNtf ntf;
    ntf.set_room_id(room.RoomId());

    for (const auto& player : room.Players()) {
        auto* out = ntf.add_players();
        out->set_uid(player.uid);
        out->set_team(ToProtoTeam(player.team));
        out->set_ready(player.ready);
    }

    for (auto uid : room.PlayerUids()) {
        SendToPlayer(MsgId::RoomStateNtf, uid, ntf);
    }
}

void LogicShard::BroadcastCommandFrame(DungeonRoom& room,
                                       const rts::v1::GateToGameEnvelope& envelope,
                                       MsgId msg_id) {
    const auto server_tick = room.NextServerTick();

    rts::v1::CommandFrameNtf ntf;
    ntf.set_room_id(room.RoomId());
    ntf.set_server_tick(server_tick);

    auto* command = ntf.add_commands();
    command->set_command_id(room.NextCommandId());
    command->set_uid(envelope.uid());
    command->set_client_seq(envelope.client_seq());
    command->set_inner_msg_id(static_cast<std::uint16_t>(msg_id));
    command->set_payload(envelope.payload());

    for (auto uid : room.PlayerUids()) {
        SendToPlayer(MsgId::CommandFrameNtf, uid, ntf, server_tick);
    }
}

void LogicShard::BroadcastGameStart(const DungeonRoom& room) {
    rts::v1::GameStartNtf ntf;
    ntf.set_room_id(room.RoomId());
    ntf.set_server_tick(room.ServerTick());
    ntf.set_tick_rate(kDefaultTickRate);
    ntf.set_snapshot_rate(kDefaultSnapshotRate);
    ntf.set_random_seed(room.RoomId() * 1103515245ULL + 12345ULL);

    for (auto uid : room.PlayerUids()) {
        SendToPlayer(MsgId::GameStartNtf, uid, ntf);
    }
}

void LogicShard::HandleCreateRoom(LogicTask task) {
    rts::v1::CreateRoomRsp rsp;

    rts::v1::GateToGameEnvelope envelope;
    rts::v1::CreateRoomReq req;
    if (!ParseEnvelope(task, envelope) || !ParseInnerPayload(envelope, req)) {
        rsp.set_code(rts::v1::ERROR_INVALID_REQUEST);
        rsp.set_reason("invalid CreateRoomReq");
        SendToPlayer(MsgId::CreateRoomRsp, task.uid, rsp);
        return;
    }

    if (auto uid_it = uid_to_room_.find(task.uid); uid_it != uid_to_room_.end()) {
        rsp.set_code(rts::v1::ERROR_INVALID_REQUEST);
        rsp.set_room_id(uid_it->second);
        rsp.set_reason("player is already in room");
        SendToPlayer(MsgId::CreateRoomRsp, task.uid, rsp);
        return;
    }

    const auto room_id = next_room_id_++;
    auto room_name = req.room_name().empty()
        ? "room-" + std::to_string(room_id)
        : req.room_name();
    auto max_players = NormalizeMaxPlayers(req.max_players());

    auto [iter, inserted] = rooms_.emplace(
        room_id,
        DungeonRoom(room_id, std::move(room_name), max_players));
    auto& room = iter->second;

    std::uint32_t assigned_team = 0;
    room.AddPlayer(task.uid, &assigned_team);
    uid_to_room_[task.uid] = room_id;

    rsp.set_code(rts::v1::ERROR_NONE);
    rsp.set_room_id(room_id);
    SendToPlayer(MsgId::CreateRoomRsp, task.uid, rsp);
    BroadcastRoomState(room);

    spdlog::info("created room {}, owner uid {}, max_players={}",
                 room_id, task.uid, max_players);
}

void LogicShard::HandleJoinRoom(LogicTask task) {
    rts::v1::JoinRoomRsp rsp;

    rts::v1::GateToGameEnvelope envelope;
    rts::v1::JoinRoomReq req;
    if (!ParseEnvelope(task, envelope) || !ParseInnerPayload(envelope, req)) {
        rsp.set_code(rts::v1::ERROR_INVALID_REQUEST);
        rsp.set_reason("invalid JoinRoomReq");
        SendToPlayer(MsgId::JoinRoomRsp, task.uid, rsp);
        return;
    }

    if (auto uid_it = uid_to_room_.find(task.uid);
        uid_it != uid_to_room_.end() && uid_it->second != req.room_id()) {
        rsp.set_code(rts::v1::ERROR_INVALID_REQUEST);
        rsp.set_room_id(uid_it->second);
        rsp.set_reason("player is already in another room");
        SendToPlayer(MsgId::JoinRoomRsp, task.uid, rsp);
        return;
    }

    auto room_it = rooms_.find(req.room_id());
    if (room_it == rooms_.end()) {
        rsp.set_code(rts::v1::ERROR_ROOM_NOT_FOUND);
        rsp.set_room_id(req.room_id());
        rsp.set_reason("room not found");
        SendToPlayer(MsgId::JoinRoomRsp, task.uid, rsp);
        return;
    }

    auto& room = room_it->second;
    std::uint32_t assigned_team = 0;
    if (!room.AddPlayer(task.uid, &assigned_team)) {
        rsp.set_code(rts::v1::ERROR_INVALID_REQUEST);
        rsp.set_room_id(req.room_id());
        rsp.set_reason("room is full");
        SendToPlayer(MsgId::JoinRoomRsp, task.uid, rsp);
        return;
    }

    uid_to_room_[task.uid] = req.room_id();

    rsp.set_code(rts::v1::ERROR_NONE);
    rsp.set_room_id(req.room_id());
    rsp.set_assigned_team(ToProtoTeam(assigned_team));
    SendToPlayer(MsgId::JoinRoomRsp, task.uid, rsp);
    BroadcastRoomState(room);

    spdlog::info("uid {} joined room {}", task.uid, req.room_id());
}

void LogicShard::HandleLeaveRoom(LogicTask task) {
    rts::v1::GateToGameEnvelope envelope;
    rts::v1::LeaveRoomReq req;
    if (!ParseEnvelope(task, envelope) || !ParseInnerPayload(envelope, req)) {
        spdlog::warn("invalid LeaveRoomReq from uid {}", task.uid);
        return;
    }

    auto room_id = req.room_id();
    if (room_id == 0) {
        room_id = ResolveRoomId(envelope, uid_to_room_, task.uid);
    }

    auto room_it = rooms_.find(room_id);
    if (room_it == rooms_.end()) {
        spdlog::warn("leave failed: room {} not found, uid {}", room_id, task.uid);
        return;
    }

    auto& room = room_it->second;
    if (!room.RemovePlayer(task.uid)) {
        spdlog::warn("leave failed: uid {} not in room {}", task.uid, room_id);
        return;
    }

    uid_to_room_.erase(task.uid);

    if (room.Empty()) {
        rooms_.erase(room_it);
        spdlog::info("room {} removed after uid {} left", room_id, task.uid);
        return;
    }

    BroadcastRoomState(room);
    spdlog::info("uid {} left room {}", task.uid, room_id);
}

void LogicShard::HandlePlayerReady(LogicTask task) {
    rts::v1::GateToGameEnvelope envelope;
    rts::v1::PlayerReadyReq req;
    if (!ParseEnvelope(task, envelope) || !ParseInnerPayload(envelope, req)) {
        spdlog::warn("invalid PlayerReadyReq from uid {}", task.uid);
        return;
    }

    auto room_id = req.room_id();
    if (room_id == 0) {
        room_id = ResolveRoomId(envelope, uid_to_room_, task.uid);
    }

    auto room_it = rooms_.find(room_id);
    if (room_it == rooms_.end()) {
        spdlog::warn("ready failed: room {} not found, uid {}", room_id, task.uid);
        return;
    }

    auto& room = room_it->second;
    if (!room.SetReady(task.uid, req.ready())) {
        spdlog::warn("ready failed: uid {} not in room {}", task.uid, room_id);
        return;
    }

    BroadcastRoomState(room);
    if (room.AllReady() && !room.Started()) {
        room.SetStarted(true);
        BroadcastGameStart(room);
    }
}

void LogicShard::HandleEnterBattle(LogicTask task) {
    rts::v1::EnterBattleRsp rsp;

    rts::v1::GateToGameEnvelope envelope;
    rts::v1::EnterBattleReq req;
    if (!ParseEnvelope(task, envelope) || !ParseInnerPayload(envelope, req)) {
        rsp.set_code(rts::v1::ERROR_INVALID_REQUEST);
        rsp.set_reason("invalid EnterBattleReq");
        SendToPlayer(MsgId::EnterBattleRsp, task.uid, rsp);
        return;
    }

    auto room_id = req.room_id();
    if (room_id == 0) {
        room_id = ResolveRoomId(envelope, uid_to_room_, task.uid);
    }

    auto room_it = rooms_.find(room_id);
    if (room_it == rooms_.end()) {
        rsp.set_code(rts::v1::ERROR_ROOM_NOT_FOUND);
        rsp.set_room_id(room_id);
        rsp.set_reason("room not found");
        SendToPlayer(MsgId::EnterBattleRsp, task.uid, rsp);
        return;
    }

    if (!room_it->second.HasPlayer(task.uid)) {
        rsp.set_code(rts::v1::ERROR_NOT_IN_ROOM);
        rsp.set_room_id(room_id);
        rsp.set_reason("player is not in room");
        SendToPlayer(MsgId::EnterBattleRsp, task.uid, rsp);
        return;
    }

    rsp.set_code(rts::v1::ERROR_NONE);
    rsp.set_room_id(room_id);
    rsp.set_server_tick(room_it->second.ServerTick());
    SendToPlayer(MsgId::EnterBattleRsp, task.uid, rsp);
}

void LogicShard::HandlePlayerCommand(LogicTask task) {
    rts::v1::GateToGameEnvelope envelope;
    if (!ParseEnvelope(task, envelope)) {
        SendCommandRejected(task.uid,
                            0,
                            task.seq,
                            rts::v1::ERROR_INVALID_REQUEST,
                            "invalid GateToGameEnvelope");
        return;
    }

    const auto room_id = ResolveRoomId(envelope, uid_to_room_, task.uid);
    if (room_id == 0) {
        SendCommandRejected(task.uid,
                            0,
                            envelope.client_seq(),
                            rts::v1::ERROR_NOT_IN_ROOM,
                            "player is not in room");
        return;
    }

    auto room_it = rooms_.find(room_id);
    if (room_it == rooms_.end()) {
        SendCommandRejected(task.uid,
                            room_id,
                            envelope.client_seq(),
                            rts::v1::ERROR_ROOM_NOT_FOUND,
                            "room not found");
        return;
    }

    auto& room = room_it->second;
    if (!room.HasPlayer(task.uid)) {
        SendCommandRejected(task.uid,
                            room_id,
                            envelope.client_seq(),
                            rts::v1::ERROR_NOT_IN_ROOM,
                            "player is not in room");
        return;
    }

    if (envelope.payload().empty()) {
        SendCommandRejected(task.uid,
                            room_id,
                            envelope.client_seq(),
                            rts::v1::ERROR_INVALID_REQUEST,
                            "empty command payload");
        return;
    }

    BroadcastCommandFrame(room, envelope, task.msg_id);
}
