#include"../include/LogicShard.h"
#include"../include/LogicRouter.h"
#include"../include/MsgNode.h"
#include"../include/Metrics.h"
#include "../../common/ProtoCodec.h"
#include "rts.pb.h"

#include <algorithm>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {

constexpr std::uint32_t kDefaultMaxPlayers = 2;
constexpr std::uint32_t kMaxRoomPlayers = 4;
constexpr std::uint32_t kDefaultTickRate = 20;

// tick / 状态同步参数
constexpr std::chrono::milliseconds kTickInterval {50}; // 20Hz
constexpr float kTickSeconds = 0.05f;
constexpr std::uint64_t kFullSnapshotInterval = 20; // 每 20 个 tick 一次全量, 其余增量
constexpr float kUnitSpeed = 3.0f;  // 单位/秒
constexpr float kUnitHp = 100.0f;
constexpr std::uint32_t kStartUnitsPerPlayer = 6;
constexpr float kFormationColumnSpacing = 2.8f;
constexpr float kFormationRowSpacing = 3.0f;

struct InitialTeamLayout {
    Vec3f barracks;
    Vec3f unit_center;
    Vec3f right;
    Vec3f forward;
};

InitialTeamLayout InitialLayoutForTeam(std::uint32_t team) {
    if (team == 0) {
        return InitialTeamLayout{
            Vec3f{19.3f, 0.0f, 43.71f},
            Vec3f{31.0f, 0.0f, 50.0f},
            Vec3f{1.0f, 0.0f, 0.0f},
            Vec3f{0.0f, 0.0f, 1.0f}};
    }

    if (team == 1) {
        return InitialTeamLayout{
            Vec3f{118.63121f, 0.0f, 133.266647f},
            Vec3f{111.708f, 0.0f, 128.335f},
            Vec3f{-0.5801302f, 0.0f, 0.814503f},
            Vec3f{-0.8144982f, 0.0f, -0.5801538f}};
    }

    const float offset = static_cast<float>(team - 1) * 30.0f;
    return InitialTeamLayout{
        Vec3f{118.63121f + offset, 0.0f, 133.266647f},
        Vec3f{111.708f + offset, 0.0f, 128.335f},
        Vec3f{1.0f, 0.0f, 0.0f},
        Vec3f{0.0f, 0.0f, -1.0f}};
}

Vec3f Offset(const Vec3f& origin, const Vec3f& direction, float distance) {
    return Vec3f{
        origin.x + direction.x * distance,
        origin.y + direction.y * distance,
        origin.z + direction.z * distance};
}

float DirectionYaw(const Vec3f& direction) {
    return std::atan2(direction.x, direction.z);
}

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

void FillUnitSnapshot(rts::v1::UnitStateSnapshot& out, const Unit& u) {
    out.set_id(u.id);
    out.set_team(ToProtoTeam(u.team));
    out.set_unit_type(static_cast<rts::v1::UnitType>(u.unit_type));
    auto* pos = out.mutable_position();
    pos->set_x(u.pos.x);
    pos->set_y(u.pos.y);
    pos->set_z(u.pos.z);
    out.set_yaw(u.yaw);
    out.set_hp(u.hp);
    out.set_state(static_cast<rts::v1::UnitState>(u.state));
    out.set_carried_amount(u.carried_amount);
    out.set_carried_raw(static_cast<rts::v1::ResourceRaw>(u.carried_raw));
}

void FillBuildingSnapshot(rts::v1::BuildingStateSnapshot& out, const Building& b) {
    out.set_id(b.id);
    out.set_team(ToProtoTeam(b.team));
    out.set_building_type(static_cast<rts::v1::BuildingType>(b.building_type));
    auto* pos = out.mutable_position();
    pos->set_x(b.pos.x);
    pos->set_y(b.pos.y);
    pos->set_z(b.pos.z);
    out.set_yaw(b.yaw);
    out.set_hp(b.hp);
    out.set_constructed_percent(b.constructed_percent);
}

void FillFieldSnapshot(rts::v1::ResourceFieldStateSnapshot& out,
                       const ResourceFieldEntity& f) {
    out.set_id(f.id);
    out.set_raw(static_cast<rts::v1::ResourceRaw>(f.raw));
    auto* pos = out.mutable_position();
    pos->set_x(f.pos.x);
    pos->set_y(f.pos.y);
    pos->set_z(f.pos.z);
    out.set_yaw(f.yaw);
    out.set_amount_left(f.amount_left);
}

void FillDropSnapshot(rts::v1::ResourceDropStateSnapshot& out,
                      const ResourceDropEntity& d) {
    out.set_id(d.id);
    out.set_raw(static_cast<rts::v1::ResourceRaw>(d.raw));
    auto* pos = out.mutable_position();
    pos->set_x(d.pos.x);
    pos->set_y(d.pos.y);
    pos->set_z(d.pos.z);
    out.set_amount(d.amount);
}

void FillPlayerResources(rts::v1::PlayerResources& out,
                         std::uint64_t uid,
                         const PlayerRes& r) {
    out.set_uid(uid);
    out.set_food(r.food);
    out.set_wood(r.wood);
    out.set_gold(r.gold);
}

} // namespace


LogicShard::LogicShard(GameServer* server, ShardId shard_id, std::size_t shard_count)
    : tick_timer_(ioc_),
      b_stop_(false),
      server_(server),
      shard_id_(shard_id),
      shard_count_(shard_count == 0 ? 1 : shard_count) {}

void LogicShard::start() {
    if (thread_.joinable()) {
        return;
    }

    b_stop_ = false;
    ioc_.restart();

    work_guard_ =
        std::make_unique<boost::asio::executor_work_guard<
            boost::asio::io_context::executor_type>>(ioc_.get_executor());

    boost::asio::co_spawn(
        ioc_.get_executor(),
        [this]() -> boost::asio::awaitable<void> {
            co_await TickLoop();
        },
        boost::asio::detached);

    thread_ = std::thread([this]() {
        ioc_.run();
    });
}

void LogicShard::stop() {
    b_stop_ = true;

    boost::system::error_code ec;
    tick_timer_.cancel(ec);

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
    metrics::commands_total.fetch_add(1, std::memory_order_relaxed);
    // 记录这个 uid 最近一次入站的"路由坐标", 回包时按它找对应网络 shard 的链路
    if (task.uid != 0) {
        uid_route_[task.uid] = task.origin;
    }
    bool ok=LogicRouter::Getinstance()->Dispatch(this,std::move(task));
    if(!ok){
        spdlog::debug("call back undefined msgid");
    }
    spdlog::debug("handle msg success");
}

// 固定步长心跳: 绝对时刻调度防累积漂移; 落后过多直接重新对齐(防死亡螺旋)
boost::asio::awaitable<void> LogicShard::TickLoop() {
    boost::system::error_code ec;
    next_tick_deadline_ = std::chrono::steady_clock::now() + kTickInterval;

    while (!b_stop_) {
        tick_timer_.expires_at(next_tick_deadline_);
        ec.clear();
        co_await tick_timer_.async_wait(
            boost::asio::redirect_error(boost::asio::use_awaitable, ec));

        if (ec == boost::asio::error::operation_aborted) {
            co_return;
        }
        if (ec) {
            spdlog::error("logic shard tick timer error: {}", ec.message());
            co_return;
        }

        TickRooms();

        next_tick_deadline_ += kTickInterval;
        const auto now = std::chrono::steady_clock::now();
        if (next_tick_deadline_ < now) {
            // 落后了, 丢掉错过的 tick 重新对齐, 而不是无限追帧
            next_tick_deadline_ = now + kTickInterval;
        }
    }
}

void LogicShard::TickRooms() {
    const auto tick_t0 = std::chrono::steady_clock::now();
    std::uint64_t active_rooms = 0;
    std::uint64_t active_units = 0;

    for (auto& [room_id, room] : rooms_) {
        if (!room.Started() || room.IsGameOver()) {
            continue;
        }

        ++active_rooms;
        active_units += room.Units().size();

        room.ApplyPending();   // 应用本帧攒下的输入

        // 资源不足 / 目标非法 等被拒命令, 回 CommandRejectedNtf
        for (const auto& rej : room.TakeRejected()) {
            SendCommandRejected(rej.uid, room_id, rej.client_seq,
                                static_cast<rts::v1::ErrorCode>(rej.code),
                                "command rejected");
        }

        room.Step(kTickSeconds); // 推进仿真一步: 移动/采集/建造/追击/攻击/死亡

        const auto server_tick = room.NextServerTick();
        if (server_tick % kFullSnapshotInterval == 0) {
            SendFullSnapshot(room);
        } else {
            SendDelta(room); // 含本帧死亡的 EntityDespawnNtf
        }
        room.ClearFrameChanges();

        // 一方团灭则结束
        std::uint32_t winner_team = 0;
        if (room.CheckGameOver(winner_team)) {
            room.SetGameOver(true);
            BroadcastGameOver(room, winner_team);
        }
    }

    const auto tick_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - tick_t0).count();
    metrics::RecordTickUs(static_cast<std::uint32_t>(tick_us));
    metrics::SetShardGauges(shard_id_, active_rooms, active_units);
}

void LogicShard::SpawnInitialUnits(DungeonRoom& room) {
    for (const auto& player : room.Players()) {
        const auto layout = InitialLayoutForTeam(player.team);
        const float facing_yaw = DirectionYaw(layout.forward);

        // The Unity map already owns the visible barracks. Keep only the
        // authoritative crystal used for health, targeting and victory rules.
        const Vec3f crystal = layout.barracks;

        // 主基地水晶 (胜负核心, 兼作资源站)
        room.SpawnBuilding(player.uid, player.team, kBuildingCrystal,
                           crystal, facing_yaw,
                           /*under_construction=*/false);

        // 起始村民
        for (std::uint32_t i = 0; i < kStartUnitsPerPlayer; ++i) {
            const auto row = static_cast<float>(i / 3);
            const auto column = static_cast<float>(i % 3);
            Vec3f pos = Offset(layout.unit_center,
                               layout.right,
                               (column - 1.0f) * kFormationColumnSpacing);
            pos = Offset(pos,
                         layout.forward,
                         (row - 0.5f) * kFormationRowSpacing);
            room.SpawnUnit(player.uid, player.team, kUnitVillager, pos,
                           kUnitHp, kUnitSpeed, facing_yaw);
        }

        auto spawn_resource = [&](std::uint32_t raw,
                                  float right_distance,
                                  float forward_distance,
                                  std::uint32_t amount) {
            Vec3f pos = Offset(crystal, layout.right, right_distance);
            pos = Offset(pos, layout.forward, forward_distance);
            room.SpawnResourceField(raw, pos, amount);
        };

        // 基地附近资源: 左翼浆果、右翼木材、前方金矿。坐标使用队伍局部坐标,
        // 因此两边天然镜像对称, 并避开兵营和开局六人阵型。
        spawn_resource(kRawBerries, -11.0f, 10.0f, 450);
        spawn_resource(kRawBerries, -14.0f, 13.5f, 450);
        spawn_resource(kRawBerries, -8.0f, 15.5f, 450);

        spawn_resource(kRawWood, 16.0f, 7.0f, 600);
        spawn_resource(kRawWood, 19.0f, 11.0f, 600);
        spawn_resource(kRawWood, 22.0f, 14.5f, 600);

        spawn_resource(kRawGold, -3.5f, 20.0f, 400);
        spawn_resource(kRawGold, 3.5f, 21.5f, 400);
    }
}

void LogicShard::SendFullSnapshot(DungeonRoom& room) {
    metrics::snapshots_total.fetch_add(1, std::memory_order_relaxed);
    rts::v1::SnapshotNtf ntf;
    ntf.set_room_id(room.RoomId());
    ntf.set_server_tick(room.ServerTick());

    for (const auto& [id, unit] : room.Units()) {
        FillUnitSnapshot(*ntf.add_units(), unit);
    }
    for (const auto& [id, building] : room.Buildings()) {
        FillBuildingSnapshot(*ntf.add_buildings(), building);
    }
    for (const auto& [id, field] : room.Fields()) {
        FillFieldSnapshot(*ntf.add_resource_fields(), field);
    }
    for (const auto& [id, drop] : room.Drops()) {
        FillDropSnapshot(*ntf.add_resource_drops(), drop);
    }
    for (const auto& [uid, res] : room.PlayerResources()) {
        FillPlayerResources(*ntf.add_resources(), uid, res);
    }

    SendToPlayers(MsgId::SnapshotNtf, room.PlayerUids(), ntf, room.ServerTick());
}

void LogicShard::SendDelta(DungeonRoom& room) {
    metrics::deltas_total.fetch_add(1, std::memory_order_relaxed);
    const auto tick = room.ServerTick();
    const auto player_uids = room.PlayerUids();

    // 发一条 EntitySpawnNtf 的小工具
    auto send_spawn = [&](rts::v1::EntityType type, std::string state_bytes) {
        rts::v1::EntitySpawnNtf spawn;
        spawn.set_room_id(room.RoomId());
        spawn.set_server_tick(tick);
        spawn.set_entity_type(type);
        spawn.set_entity_state(std::move(state_bytes));
        SendToPlayers(MsgId::EntitySpawnNtf, player_uids, spawn, tick);
    };

    auto send_despawn = [&](rts::v1::EntityType type, std::uint64_t id) {
        rts::v1::EntityDespawnNtf despawn;
        despawn.set_room_id(room.RoomId());
        despawn.set_server_tick(tick);
        auto* ref = despawn.mutable_entity();
        ref->set_type(type);
        ref->set_id(id);
        SendToPlayers(MsgId::EntityDespawnNtf, player_uids, despawn, tick);
    };

    // 1. 新生实体 (各类型, 携带完整初始状态)
    for (auto id : room.Spawned()) {
        const Unit* unit = room.FindUnit(id);
        if (!unit) continue;
        rts::v1::UnitStateSnapshot state;
        FillUnitSnapshot(state, *unit);
        std::string bytes;
        state.SerializeToString(&bytes);
        send_spawn(rts::v1::ENTITY_UNIT, std::move(bytes));
    }
    for (auto id : room.BuildingsSpawned()) {
        const Building* b = room.FindBuilding(id);
        if (!b) continue;
        rts::v1::BuildingStateSnapshot state;
        FillBuildingSnapshot(state, *b);
        std::string bytes;
        state.SerializeToString(&bytes);
        send_spawn(rts::v1::ENTITY_BUILDING, std::move(bytes));
    }
    for (auto id : room.FieldsSpawned()) {
        const ResourceFieldEntity* f = room.FindField(id);
        if (!f) continue;
        rts::v1::ResourceFieldStateSnapshot state;
        FillFieldSnapshot(state, *f);
        std::string bytes;
        state.SerializeToString(&bytes);
        send_spawn(rts::v1::ENTITY_RESOURCE_FIELD, std::move(bytes));
    }
    for (auto id : room.DropsSpawned()) {
        const ResourceDropEntity* d = room.FindDrop(id);
        if (!d) continue;
        rts::v1::ResourceDropStateSnapshot state;
        FillDropSnapshot(state, *d);
        std::string bytes;
        state.SerializeToString(&bytes);
        send_spawn(rts::v1::ENTITY_RESOURCE_DROP, std::move(bytes));
    }

    // 2. 销毁实体
    for (auto id : room.Despawned()) {
        send_despawn(rts::v1::ENTITY_UNIT, id);
    }
    for (auto id : room.BuildingsDespawned()) {
        send_despawn(rts::v1::ENTITY_BUILDING, id);
    }
    for (auto id : room.FieldsDespawned()) {
        send_despawn(rts::v1::ENTITY_RESOURCE_FIELD, id);
    }
    for (auto id : room.DropsDespawned()) {
        send_despawn(rts::v1::ENTITY_RESOURCE_DROP, id);
    }

    // 3. 状态变化(排除本帧新生的, 它们已在 spawn 里带过全量)
    std::unordered_set<std::uint64_t> unit_spawned(room.Spawned().begin(),
                                                   room.Spawned().end());
    std::unordered_set<std::uint64_t> building_spawned(room.BuildingsSpawned().begin(),
                                                       room.BuildingsSpawned().end());
    rts::v1::WorldDeltaNtf delta;
    delta.set_room_id(room.RoomId());
    delta.set_server_tick(tick);

    bool has_changes = false;
    for (auto id : room.Dirty()) {
        if (unit_spawned.count(id) != 0) continue;
        const Unit* unit = room.FindUnit(id);
        if (!unit) continue;
        FillUnitSnapshot(*delta.add_units(), *unit);
        has_changes = true;
    }
    for (auto id : room.BuildingsDirty()) {
        if (building_spawned.count(id) != 0) continue;
        const Building* b = room.FindBuilding(id);
        if (!b) continue;
        FillBuildingSnapshot(*delta.add_buildings(), *b);
        has_changes = true;
    }
    for (auto uid : room.ResourceDirty()) {
        const PlayerRes* r = room.FindPlayerRes(uid);
        if (!r) continue;
        FillPlayerResources(*delta.add_resources(), uid, *r);
        has_changes = true;
    }

    if (has_changes) {
        SendToPlayers(MsgId::WorldDeltaNtf, player_uids, delta, tick);
    }
}

bool LogicShard::SendToPlayer(MsgId msg_id,
                              Uid uid,
                              const google::protobuf::MessageLite& message,
                              SeqId server_seq) {
    const std::vector<Uid> one{uid};
    SendToPlayers(msg_id, one, message, server_seq);
    return true;
}

void LogicShard::SendToPlayers(MsgId msg_id,
                               const std::vector<Uid>& uids,
                               const google::protobuf::MessageLite& message,
                               SeqId server_seq) {
    if (!server_ || uids.empty()) {
        return;
    }

    // 序列化一次, 复用给所有目标
    std::string payload;
    if (!rts::protocol::SerializeProtoToString(message, payload)) {
        spdlog::error("serialize logic response failed, msg={}",
                      static_cast<std::uint16_t>(msg_id));
        return;
    }

    auto packet = std::make_shared<SendNode>(
        payload.empty() ? nullptr : payload.data(),
        static_cast<std::uint32_t>(payload.size()),
        static_cast<std::uint16_t>(msg_id));

    // 按 网络shard -> 链路 分组(同链路上的多个 uid 合并成一个 envelope)
    std::unordered_map<NetworkShardId,
                       std::unordered_map<LinkId, NetworkSendTarget>> grouped;
    for (auto uid : uids) {
        auto it = uid_route_.find(uid);
        if (it == uid_route_.end()) {
            continue; // 还没见过这个 uid 的入站, 无法路由
        }
        const auto& route = it->second;
        auto& target = grouped[route.net_shard][route.link_id];
        target.link_id = route.link_id;
        target.generation = route.generation;
        target.uids.push_back(uid);
    }

    for (auto& [net_shard, links] : grouped) {
        NetworkTask task;
        task.msg_id = msg_id;
        task.seq = server_seq;
        task.body = packet; // 多个 shard 共享同一份只读 SendNode
        task.targets.reserve(links.size());
        for (auto& [link_id, target] : links) {
            task.targets.push_back(std::move(target));
        }
        server_->PostToNetwork(net_shard, std::move(task));
    }
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

void LogicShard::BroadcastGameStart(const DungeonRoom& room) {
    rts::v1::GameStartNtf ntf;
    ntf.set_room_id(room.RoomId());
    ntf.set_server_tick(room.ServerTick());
    ntf.set_tick_rate(kDefaultTickRate);
    ntf.set_snapshot_rate(static_cast<std::uint32_t>(kFullSnapshotInterval));
    ntf.set_random_seed(room.RoomId() * 1103515245ULL + 12345ULL);

    for (auto uid : room.PlayerUids()) {
        SendToPlayer(MsgId::GameStartNtf, uid, ntf);
    }
}

void LogicShard::BroadcastGameOver(const DungeonRoom& room, std::uint32_t winner_team) {
    rts::v1::GameOverNtf ntf;
    ntf.set_room_id(room.RoomId());
    ntf.set_server_tick(room.ServerTick());
    ntf.set_winner(ToProtoTeam(winner_team));

    SendToPlayers(MsgId::GameOverNtf, room.PlayerUids(), ntf, room.ServerTick());
    spdlog::info("room {} game over, winner team {}", room.RoomId(), winner_team);
}

void LogicShard::LeaveRoomInternal(Uid uid) {
    auto it = uid_to_room_.find(uid);
    if (it == uid_to_room_.end()) {
        return; // 不在本分片任何房间
    }
    const auto room_id = it->second;
    uid_to_room_.erase(it);

    auto room_it = rooms_.find(room_id);
    if (room_it == rooms_.end()) {
        return;
    }
    auto& room = room_it->second;
    room.RemovePlayer(uid);
    if (room.Empty()) {
        rooms_.erase(room_it);
        spdlog::info("room {} removed (uid {} left/disconnected)", room_id, uid);
    } else {
        BroadcastRoomState(room);
        spdlog::info("uid {} left room {}", uid, room_id);
    }
}

void LogicShard::HandleClientDisconnected(LogicTask task) {
    rts::v1::ClientDisconnectedNtf ntf;
    if (!rts::protocol::ParseProtoFromBytes(BodyView(task.body), ntf)) {
        return;
    }
    const auto uid = ntf.uid();
    if (uid == 0) {
        return;
    }
    uid_route_.erase(uid);
    LeaveRoomInternal(uid); // 该玩家不在本分片则是 no-op
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

    // 已在某房间(常见于上次掉线/退出残留): 先隐式离开旧房再建新房
    LeaveRoomInternal(task.uid);

    // room_id 落在本 shard 的同余类: room_id % shard_count == shard_id_
    // 这样任何带 room_id 的消息都能路由回房间所在的 shard
    const auto room_id =
        static_cast<std::uint64_t>(shard_id_) + shard_count_ * next_room_id_;
    ++next_room_id_;
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

    // 已在别的房间(常见于上次掉线/退出残留): 先隐式离开
    if (auto uid_it = uid_to_room_.find(task.uid);
        uid_it != uid_to_room_.end() && uid_it->second != req.room_id()) {
        LeaveRoomInternal(task.uid);
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
    // 必须房间满员 + 全员准备才开局 (避免单人 ready 就开)
    if (room.IsFull() && room.AllReady() && !room.Started()) {
        room.SetStarted(true);
        BroadcastGameStart(room);

        // 服务器权威地生成初始单位, 并下发一帧全量做 baseline
        SpawnInitialUnits(room);
        room.BeginBattle();
        SendFullSnapshot(room);
        room.ClearFrameChanges();
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

    // 收集命令: 解析意图后入队, 到 tick 边界统一应用 (不立即改世界)
    switch (task.msg_id) {
    case MsgId::MoveCmd: {
        rts::v1::MoveCmd cmd;
        if (!ParseInnerPayload(envelope, cmd)) {
            SendCommandRejected(task.uid, room_id, envelope.client_seq(),
                                rts::v1::ERROR_INVALID_REQUEST, "invalid MoveCmd");
            return;
        }

        PendingCommand pending;
        pending.type = CommandType::Move;
        pending.owner_uid = task.uid;
        pending.unit_ids.assign(cmd.actor_unit_ids().begin(),
                                cmd.actor_unit_ids().end());
        if (cmd.has_target_position()) {
            pending.target.x = cmd.target_position().x();
            pending.target.y = cmd.target_position().y();
            pending.target.z = cmd.target_position().z();
        }
        // 方案A: 客户端 NavMesh 折线路径
        for (const auto& wp : cmd.path()) {
            Vec3f v;
            v.x = wp.x();
            v.y = wp.y();
            v.z = wp.z();
            pending.path.push_back(v);
        }
        if (!pending.path.empty()) {
            pending.target = pending.path.back(); // 兜底终点
        }
        room.EnqueueCommand(std::move(pending));
        break;
    }
    case MsgId::StopCmd: {
        rts::v1::StopCmd cmd;
        if (!ParseInnerPayload(envelope, cmd)) {
            SendCommandRejected(task.uid, room_id, envelope.client_seq(),
                                rts::v1::ERROR_INVALID_REQUEST, "invalid StopCmd");
            return;
        }

        PendingCommand pending;
        pending.type = CommandType::Stop;
        pending.owner_uid = task.uid;
        pending.unit_ids.assign(cmd.actor_unit_ids().begin(),
                                cmd.actor_unit_ids().end());
        room.EnqueueCommand(std::move(pending));
        break;
    }
    case MsgId::AttackCmd: {
        rts::v1::AttackCmd cmd;
        if (!ParseInnerPayload(envelope, cmd)) {
            SendCommandRejected(task.uid, room_id, envelope.client_seq(),
                                rts::v1::ERROR_INVALID_REQUEST, "invalid AttackCmd");
            return;
        }

        PendingCommand pending;
        pending.type = CommandType::Attack;
        pending.owner_uid = task.uid;
        pending.unit_ids.assign(cmd.actor_unit_ids().begin(),
                                cmd.actor_unit_ids().end());
        if (cmd.has_target()) {
            pending.target_entity = cmd.target().id();
        }
        room.EnqueueCommand(std::move(pending));
        break;
    }
    case MsgId::HarvestCmd: {
        rts::v1::HarvestCmd cmd;
        if (!ParseInnerPayload(envelope, cmd)) {
            SendCommandRejected(task.uid, room_id, envelope.client_seq(),
                                rts::v1::ERROR_INVALID_REQUEST, "invalid HarvestCmd");
            return;
        }
        PendingCommand pending;
        pending.type = CommandType::Harvest;
        pending.owner_uid = task.uid;
        pending.client_seq = envelope.client_seq();
        pending.unit_ids.assign(cmd.actor_unit_ids().begin(),
                                cmd.actor_unit_ids().end());
        pending.target_entity = cmd.resource_field_id();
        room.EnqueueCommand(std::move(pending));
        break;
    }
    case MsgId::StoreResourceCmd: {
        rts::v1::StoreResourceCmd cmd;
        if (!ParseInnerPayload(envelope, cmd)) {
            SendCommandRejected(task.uid, room_id, envelope.client_seq(),
                                rts::v1::ERROR_INVALID_REQUEST, "invalid StoreResourceCmd");
            return;
        }
        PendingCommand pending;
        pending.type = CommandType::Store;
        pending.owner_uid = task.uid;
        pending.client_seq = envelope.client_seq();
        pending.unit_ids.assign(cmd.actor_unit_ids().begin(),
                                cmd.actor_unit_ids().end());
        pending.target_entity = cmd.resource_camp_id();
        room.EnqueueCommand(std::move(pending));
        break;
    }
    case MsgId::PickupResourceCmd: {
        rts::v1::PickupResourceCmd cmd;
        if (!ParseInnerPayload(envelope, cmd)) {
            SendCommandRejected(task.uid, room_id, envelope.client_seq(),
                                rts::v1::ERROR_INVALID_REQUEST, "invalid PickupResourceCmd");
            return;
        }
        PendingCommand pending;
        pending.type = CommandType::Pickup;
        pending.owner_uid = task.uid;
        pending.client_seq = envelope.client_seq();
        pending.unit_ids.assign(cmd.actor_unit_ids().begin(),
                                cmd.actor_unit_ids().end());
        pending.target_entity = cmd.resource_drop_id();
        room.EnqueueCommand(std::move(pending));
        break;
    }
    case MsgId::BuildCmd: {
        rts::v1::BuildCmd cmd;
        if (!ParseInnerPayload(envelope, cmd)) {
            SendCommandRejected(task.uid, room_id, envelope.client_seq(),
                                rts::v1::ERROR_INVALID_REQUEST, "invalid BuildCmd");
            return;
        }
        PendingCommand pending;
        pending.type = CommandType::Build;
        pending.owner_uid = task.uid;
        pending.client_seq = envelope.client_seq();
        pending.unit_ids.assign(cmd.builder_unit_ids().begin(),
                                cmd.builder_unit_ids().end());
        pending.aux_type = static_cast<std::uint32_t>(cmd.building_type());
        if (cmd.has_position()) {
            pending.target.x = cmd.position().x();
            pending.target.y = cmd.position().y();
            pending.target.z = cmd.position().z();
        }
        pending.yaw = cmd.yaw();
        room.EnqueueCommand(std::move(pending));
        break;
    }
    case MsgId::ConstructCmd: {
        rts::v1::ConstructCmd cmd;
        if (!ParseInnerPayload(envelope, cmd)) {
            SendCommandRejected(task.uid, room_id, envelope.client_seq(),
                                rts::v1::ERROR_INVALID_REQUEST, "invalid ConstructCmd");
            return;
        }
        PendingCommand pending;
        pending.type = CommandType::Construct;
        pending.owner_uid = task.uid;
        pending.client_seq = envelope.client_seq();
        pending.unit_ids.assign(cmd.builder_unit_ids().begin(),
                                cmd.builder_unit_ids().end());
        pending.target_entity = cmd.under_construction_building_id();
        room.EnqueueCommand(std::move(pending));
        break;
    }
    case MsgId::TrainUnitCmd: {
        rts::v1::TrainUnitCmd cmd;
        if (!ParseInnerPayload(envelope, cmd)) {
            SendCommandRejected(task.uid, room_id, envelope.client_seq(),
                                rts::v1::ERROR_INVALID_REQUEST, "invalid TrainUnitCmd");
            return;
        }
        PendingCommand pending;
        pending.type = CommandType::Train;
        pending.owner_uid = task.uid;
        pending.client_seq = envelope.client_seq();
        pending.target_entity = cmd.producer_building_id();
        pending.aux_type = static_cast<std::uint32_t>(cmd.unit_type());
        room.EnqueueCommand(std::move(pending));
        break;
    }
    default:
        spdlog::debug("command msg {} not simulated yet",
                      static_cast<std::uint16_t>(task.msg_id));
        break;
    }
}
