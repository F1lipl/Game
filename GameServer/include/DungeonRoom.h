#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

struct RoomPlayerState {
    std::uint64_t uid {};
    std::uint32_t team {};
    bool ready {};
};

// ---- 状态同步: 服务器权威世界状态 ----

struct Vec3f {
    float x {};
    float y {};
    float z {};
};

// ---- 玩法枚举 (数值与 proto rts.proto 保持一致, 但本文件不依赖 proto) ----

// EntityType
inline constexpr std::uint32_t kEntityUnit = 0;
inline constexpr std::uint32_t kEntityBuilding = 1;
inline constexpr std::uint32_t kEntityResourceField = 2;
inline constexpr std::uint32_t kEntityResourceDrop = 3;

// UnitState (rts::v1::UnitState)
inline constexpr std::uint32_t kStateIdle = 0;
inline constexpr std::uint32_t kStateMoving = 1;
inline constexpr std::uint32_t kStateWorking = 2;
inline constexpr std::uint32_t kStateAttacking = 3;
inline constexpr std::uint32_t kStateDead = 4;

// UnitType
inline constexpr std::uint32_t kUnitVillager = 0;

// BuildingType
inline constexpr std::uint32_t kBuildingResourceCamp = 0;
inline constexpr std::uint32_t kBuildingVillagerInn = 1;
inline constexpr std::uint32_t kBuildingBarracks = 2;
inline constexpr std::uint32_t kBuildingCrystal = 3;

// ResourceType (存入玩家账本时用)
inline constexpr std::uint32_t kResFood = 0;
inline constexpr std::uint32_t kResWood = 1;
inline constexpr std::uint32_t kResGold = 2;
inline constexpr std::uint32_t kResNone = 3;

// ResourceRaw (资源场/掉落物的原始类型)
inline constexpr std::uint32_t kRawBerries = 0;
inline constexpr std::uint32_t kRawGold = 1;
inline constexpr std::uint32_t kRawWood = 2;
inline constexpr std::uint32_t kRawFarm = 3;

// 战斗参数(单一兵种, 暂用共享常量)
inline constexpr float kAttackRange = 2.0f;
inline constexpr float kAttackDamage = 20.0f;
inline constexpr float kAttackCooldown = 1.0f; // 秒

// 经济 / 建造 / 生产参数 (可调)
inline constexpr std::uint32_t kVillagerCarryCapacity = 10; // 工人背包上限
inline constexpr float kHarvestRange = 2.0f;        // 采集判定距离
inline constexpr float kHarvestInterval = 1.0f;     // 每多少秒采 1 单位, 对齐 Unity 采集动画节奏
inline constexpr std::uint32_t kHarvestAmount = 1;  // 每次采集量
inline constexpr float kCarryLiftDuration = 2.2f;   // 采满后举起资源的表现停顿
inline constexpr float kDropoffRange = 2.5f;        // 资源站存放判定距离
inline constexpr float kPickupRange = 2.0f;         // 捡拾掉落物判定距离
inline constexpr float kBuildRange = 2.5f;          // 施工判定距离
inline constexpr float kConstructPercentPerSec = 25.0f; // 建造进度速率 (%/s, 单工人)

// 开局每位玩家起始资源
inline constexpr std::uint32_t kStartFood = 200;
inline constexpr std::uint32_t kStartWood = 200;
inline constexpr std::uint32_t kStartGold = 100;

// 建造花费
struct ResCost {
    std::uint32_t food {};
    std::uint32_t wood {};
    std::uint32_t gold {};
};

inline ResCost BuildingCost(std::uint32_t building_type) {
    switch (building_type) {
    case kBuildingResourceCamp: return ResCost{0, 50, 0};
    case kBuildingVillagerInn:  return ResCost{0, 100, 0};
    case kBuildingBarracks:     return ResCost{0, 150, 0};
    case kBuildingCrystal:      return ResCost{0, 0, 0}; // 主基地不可造
    default:                    return ResCost{0, 0, 0};
    }
}

inline float BuildingMaxHp(std::uint32_t building_type) {
    switch (building_type) {
    case kBuildingResourceCamp: return 200.0f;
    case kBuildingVillagerInn:  return 300.0f;
    case kBuildingBarracks:     return 400.0f;
    case kBuildingCrystal:      return 1000.0f;
    default:                    return 200.0f;
    }
}

inline ResCost UnitCost(std::uint32_t unit_type) {
    switch (unit_type) {
    case kUnitVillager: return ResCost{50, 0, 0};
    default:            return ResCost{50, 0, 0};
    }
}

inline float UnitTrainTime(std::uint32_t unit_type) {
    (void)unit_type;
    return 5.0f; // 秒
}

// 哪种建筑能训练哪种单位
inline bool BuildingTrainsUnit(std::uint32_t building_type, std::uint32_t unit_type) {
    if (unit_type == kUnitVillager) {
        return building_type == kBuildingVillagerInn || building_type == kBuildingCrystal;
    }
    return building_type == kBuildingBarracks;
}

// 原始资源 -> 入账资源类型
inline std::uint32_t RawToResourceType(std::uint32_t raw) {
    switch (raw) {
    case kRawBerries: return kResFood;
    case kRawFarm:    return kResFood;
    case kRawWood:    return kResWood;
    case kRawGold:    return kResGold;
    default:          return kResNone;
    }
}

// ---- 实体 ----

enum class WorkerTask : std::uint8_t {
    None,        // 无工作 (可能仍在普通移动 has_target)
    Gathering,   // 去资源场采集
    PreparingReturn, // 采满后原地举起资源, 再回资源站
    Returning,   // 背着资源回资源站
    Pickup,      // 去捡掉落物
    Constructing // 去施工
};

struct Unit {
    std::uint64_t id {};
    std::uint64_t owner_uid {};
    std::uint32_t team {};
    std::uint32_t unit_type {};
    Vec3f pos {};
    Vec3f target {};
    bool has_target {false};
    std::vector<Vec3f> path;        // 方案A: 待走的折线 waypoints (空=直线走 target)
    std::size_t path_index {};      // 当前目标 waypoint 下标
    float yaw {};
    float hp {};
    float speed {};
    std::uint32_t state {}; // 对应 rts::v1::UnitState
    std::uint64_t attack_target {}; // 攻击目标实体 id, 0 表示无
    float attack_cd {};             // 距离下次攻击的剩余冷却(秒)

    // 工人经济 / 建造
    WorkerTask task {WorkerTask::None};
    std::uint64_t gather_field_id {}; // 当前采集的资源场
    std::uint64_t dropoff_id {};      // 当前返还的资源站
    std::uint64_t pickup_drop_id {};  // 当前捡拾的掉落物
    std::uint64_t build_target_id {}; // 当前施工的建筑
    Vec3f interaction_offset {};      // 多单位共享目标周围的工作槽位
    std::uint32_t carried_amount {};  // 背包数量
    std::uint32_t carried_raw {};     // 背包资源原始类型
    float work_cd {};                 // 采集计时累加器
};

struct Building {
    std::uint64_t id {};
    std::uint64_t owner_uid {};
    std::uint32_t team {};
    std::uint32_t building_type {};
    Vec3f pos {};
    float yaw {};
    float hp {};
    float max_hp {};
    float constructed_percent {}; // 0..100
    bool under_construction {false};

    // 训练队列 (剩余时间秒)
    struct TrainItem {
        std::uint32_t unit_type {};
        float remaining {};
    };
    std::vector<TrainItem> train_queue;
};

struct ResourceFieldEntity {
    std::uint64_t id {};
    std::uint32_t raw {};
    Vec3f pos {};
    float yaw {};
    std::uint32_t amount_left {};
};

struct ResourceDropEntity {
    std::uint64_t id {};
    std::uint32_t raw {};
    Vec3f pos {};
    std::uint32_t amount {};
};

struct PlayerRes {
    std::uint32_t food {};
    std::uint32_t wood {};
    std::uint32_t gold {};

    bool CanAfford(const ResCost& c) const {
        return food >= c.food && wood >= c.wood && gold >= c.gold;
    }
    void Pay(const ResCost& c) {
        food -= c.food;
        wood -= c.wood;
        gold -= c.gold;
    }
    void Add(std::uint32_t res_type, std::uint32_t amount) {
        switch (res_type) {
        case kResFood: food += amount; break;
        case kResWood: wood += amount; break;
        case kResGold: gold += amount; break;
        default: break;
        }
    }
};

enum class CommandType : std::uint8_t {
    Move,
    Stop,
    Attack,
    Harvest,
    Store,
    Pickup,
    Build,
    Construct,
    Train,
};

// 客户端意图先入队, 到 tick 边界才统一应用 (proto-free, 便于单测)
struct PendingCommand {
    CommandType type {CommandType::Move};
    std::uint64_t owner_uid {};
    std::uint64_t client_seq {};
    std::vector<std::uint64_t> unit_ids;
    Vec3f target {};
    std::vector<Vec3f> path;        // 方案A: MoveCmd 的 waypoints
    std::uint64_t target_entity {}; // 攻击/采集/存放/捡拾/施工 的目标实体 id
    std::uint32_t aux_type {};      // building_type (Build) 或 unit_type (Train)
    float yaw {};                   // Build 朝向
};

// 命令被拒 (资源不足 / 目标非法), 供 LogicShard 回 CommandRejectedNtf
struct RejectedCommand {
    std::uint64_t uid {};
    std::uint64_t client_seq {};
    int code {}; // 对应 rts::v1::ErrorCode
};

// ErrorCode 子集 (与 proto 一致, 避免依赖 proto 头)
inline constexpr int kErrNone = 0;
inline constexpr int kErrInvalidRequest = 1;
inline constexpr int kErrInvalidEntity = 6;
inline constexpr int kErrNotEnoughResources = 7;
inline constexpr int kErrInvalidBuildPosition = 8;

class DungeonRoom {
public:
    DungeonRoom() = default;

    DungeonRoom(std::uint64_t room_id,
                std::string room_name,
                std::uint32_t max_players)
        : room_id_(room_id),
          room_name_(std::move(room_name)),
          max_players_(max_players == 0 ? 2 : max_players) {}

    std::uint64_t RoomId() const {
        return room_id_;
    }

    const std::string& RoomName() const {
        return room_name_;
    }

    std::uint32_t MaxPlayers() const {
        return max_players_;
    }

    bool HasPlayer(std::uint64_t uid) const {
        return players_.find(uid) != players_.end();
    }

    bool IsFull() const {
        return players_.size() >= max_players_;
    }

    bool AddPlayer(std::uint64_t uid, std::uint32_t* assigned_team = nullptr) {
        auto it = players_.find(uid);
        if (it != players_.end()) {
            if (assigned_team) {
                *assigned_team = it->second.team;
            }
            return true;
        }

        if (IsFull()) {
            return false;
        }

        RoomPlayerState state;
        state.uid = uid;
        state.team = static_cast<std::uint32_t>(players_.size());
        state.ready = false;
        players_.emplace(uid, state);
        player_res_.emplace(uid, PlayerRes{});

        if (assigned_team) {
            *assigned_team = state.team;
        }

        return true;
    }

    bool SetReady(std::uint64_t uid, bool ready) {
        auto it = players_.find(uid);
        if (it == players_.end()) {
            return false;
        }

        it->second.ready = ready;
        return true;
    }

    bool RemovePlayer(std::uint64_t uid) {
        player_res_.erase(uid);
        return players_.erase(uid) > 0;
    }

    bool Empty() const {
        return players_.empty();
    }

    bool AllReady() const {
        if (players_.empty()) {
            return false;
        }

        for (const auto& [uid, player] : players_) {
            if (!player.ready) {
                return false;
            }
        }

        return true;
    }

    bool Started() const {
        return started_;
    }

    void SetStarted(bool started) {
        started_ = started;
    }

    std::uint64_t ServerTick() const {
        return server_tick_;
    }

    std::uint64_t NextServerTick() {
        return ++server_tick_;
    }

    std::uint64_t NextCommandId() {
        return ++next_command_id_;
    }

    std::vector<RoomPlayerState> Players() const {
        std::vector<RoomPlayerState> result;
        result.reserve(players_.size());
        for (const auto& [uid, player] : players_) {
            result.push_back(player);
        }
        return result;
    }

    std::vector<std::uint64_t> PlayerUids() const {
        std::vector<std::uint64_t> result;
        result.reserve(players_.size());
        for (const auto& [uid, player] : players_) {
            result.push_back(uid);
        }
        return result;
    }

    // ---- world state / simulation ----

    std::uint64_t SpawnUnit(std::uint64_t owner_uid,
                            std::uint32_t team,
                            std::uint32_t unit_type,
                            const Vec3f& pos,
                            float hp,
                            float speed,
                            float yaw = 0.0f) {
        const auto id = next_entity_id_++;
        Unit u;
        u.id = id;
        u.owner_uid = owner_uid;
        u.team = team;
        u.unit_type = unit_type;
        u.pos = pos;
        u.target = pos;
        u.has_target = false;
        u.yaw = yaw;
        u.hp = hp;
        u.speed = speed;
        u.state = kStateIdle;
        units_.emplace(id, u);
        spawned_.push_back(id);
        return id;
    }

    std::uint64_t SpawnBuilding(std::uint64_t owner_uid,
                                std::uint32_t team,
                                std::uint32_t building_type,
                                const Vec3f& pos,
                                float yaw,
                                bool under_construction) {
        const auto id = next_entity_id_++;
        Building b;
        b.id = id;
        b.owner_uid = owner_uid;
        b.team = team;
        b.building_type = building_type;
        b.pos = pos;
        b.yaw = yaw;
        b.max_hp = BuildingMaxHp(building_type);
        b.under_construction = under_construction;
        if (under_construction) {
            b.constructed_percent = 0.0f;
            b.hp = std::max(1.0f, b.max_hp * 0.05f); // 地基有一点血
        } else {
            b.constructed_percent = 100.0f;
            b.hp = b.max_hp;
        }
        buildings_.emplace(id, std::move(b));
        building_spawned_.push_back(id);
        return id;
    }

    std::uint64_t SpawnResourceField(std::uint32_t raw,
                                     const Vec3f& pos,
                                     std::uint32_t amount) {
        const auto id = next_entity_id_++;
        ResourceFieldEntity f;
        f.id = id;
        f.raw = raw;
        f.pos = pos;
        f.amount_left = amount;
        fields_.emplace(id, f);
        field_spawned_.push_back(id);
        return id;
    }

    std::uint64_t SpawnResourceDrop(std::uint32_t raw,
                                    const Vec3f& pos,
                                    std::uint32_t amount) {
        const auto id = next_entity_id_++;
        ResourceDropEntity d;
        d.id = id;
        d.raw = raw;
        d.pos = pos;
        d.amount = amount;
        drops_.emplace(id, d);
        drop_spawned_.push_back(id);
        return id;
    }

    void EnqueueCommand(PendingCommand cmd) {
        pending_.push_back(std::move(cmd));
    }

    // tick 边界: 统一应用本帧攒下的输入
    void ApplyPending() {
        for (auto& cmd : pending_) {
            switch (cmd.type) {
            case CommandType::Move:
                ApplyMove(cmd.owner_uid, cmd.unit_ids, cmd.target, cmd.path);
                break;
            case CommandType::Stop:
                ApplyStop(cmd.owner_uid, cmd.unit_ids);
                break;
            case CommandType::Attack:
                ApplyAttack(cmd.owner_uid, cmd.unit_ids, cmd.target_entity);
                break;
            case CommandType::Harvest:
                ApplyHarvest(cmd.owner_uid, cmd.unit_ids, cmd.target_entity);
                break;
            case CommandType::Store:
                ApplyStore(cmd.owner_uid, cmd.unit_ids, cmd.target_entity);
                break;
            case CommandType::Pickup:
                ApplyPickup(cmd.owner_uid, cmd.unit_ids, cmd.target_entity);
                break;
            case CommandType::Build:
                ApplyBuild(cmd);
                break;
            case CommandType::Construct:
                ApplyConstruct(cmd.owner_uid, cmd.unit_ids, cmd.target_entity);
                break;
            case CommandType::Train:
                ApplyTrain(cmd);
                break;
            }
        }
        pending_.clear();
    }

    // 推进一个固定步长: 移动 / 采集 / 建造 / 追击 / 攻击, 最后清理死亡
    void Step(float dt) {
        for (auto& [id, u] : units_) {
            if (u.hp <= 0.0f) {
                continue; // 本帧已被打死, 等待清理
            }

            if (u.attack_cd > 0.0f) {
                u.attack_cd -= dt;
                if (u.attack_cd < 0.0f) {
                    u.attack_cd = 0.0f;
                }
            }

            if (u.attack_target != 0) {
                StepCombat(u, dt);
            } else {
                switch (u.task) {
                case WorkerTask::Gathering:   StepGather(u, dt); break;
                case WorkerTask::PreparingReturn: StepPreparingReturn(u, dt); break;
                case WorkerTask::Returning:   StepReturn(u, dt); break;
                case WorkerTask::Pickup:      StepPickup(u, dt); break;
                case WorkerTask::Constructing:StepConstruct(u, dt); break;
                case WorkerTask::None:
                    if (u.has_target) {
                        StepMove(u, dt);
                    }
                    break;
                }
            }
        }

        StepBuildings(dt);
        RemoveDead();
    }

    // 战斗开始时记录初始阵营数, 用于判定团灭
    void BeginBattle() {
        std::unordered_set<std::uint32_t> teams;
        for (const auto& [id, u] : units_) {
            teams.insert(u.team);
        }
        initial_team_count_ = static_cast<std::uint32_t>(teams.size());

        std::unordered_set<std::uint32_t> crystal_teams;
        for (const auto& [id, b] : buildings_) {
            if (b.building_type == kBuildingCrystal) {
                crystal_teams.insert(b.team);
            }
        }
        initial_crystal_team_count_ = static_cast<std::uint32_t>(crystal_teams.size());

        // 发开局起始资源
        for (auto& [uid, res] : player_res_) {
            res.food = kStartFood;
            res.wood = kStartWood;
            res.gold = kStartGold;
            resource_dirty_.insert(uid);
        }

        battle_begun_ = true;
    }

    bool IsGameOver() const { return game_over_; }
    void SetGameOver(bool over) { game_over_ = over; }

    // 胜负: 有水晶则按"水晶团灭"判, 否则按"单位团灭"判 (兼容旧测试)
    bool CheckGameOver(std::uint32_t& winner_team) const {
        if (!battle_begun_) {
            return false;
        }

        if (initial_crystal_team_count_ >= 2) {
            std::unordered_set<std::uint32_t> alive;
            for (const auto& [id, b] : buildings_) {
                if (b.building_type == kBuildingCrystal && b.hp > 0.0f) {
                    alive.insert(b.team);
                }
            }
            if (alive.size() <= 1) {
                winner_team = alive.empty() ? 0xFFFFFFFFu : *alive.begin();
                return true;
            }
            return false;
        }

        if (initial_team_count_ < 2) {
            return false;
        }

        std::unordered_set<std::uint32_t> living;
        for (const auto& [id, u] : units_) {
            living.insert(u.team);
        }

        if (living.size() <= 1) {
            winner_team = living.empty()
                ? 0xFFFFFFFFu            // 同归于尽 -> 无胜者
                : *living.begin();
            return true;
        }

        return false;
    }

    // ---- 只读访问 (LogicShard 下发快照/增量时用) ----
    const std::unordered_map<std::uint64_t, Unit>& Units() const { return units_; }
    const std::unordered_set<std::uint64_t>& Dirty() const { return dirty_; }
    const std::vector<std::uint64_t>& Spawned() const { return spawned_; }
    const std::vector<std::uint64_t>& Despawned() const { return despawned_; }

    const std::unordered_map<std::uint64_t, Building>& Buildings() const { return buildings_; }
    const std::unordered_set<std::uint64_t>& BuildingsDirty() const { return building_dirty_; }
    const std::vector<std::uint64_t>& BuildingsSpawned() const { return building_spawned_; }
    const std::vector<std::uint64_t>& BuildingsDespawned() const { return building_despawned_; }

    const std::unordered_map<std::uint64_t, ResourceFieldEntity>& Fields() const { return fields_; }
    const std::vector<std::uint64_t>& FieldsSpawned() const { return field_spawned_; }
    const std::vector<std::uint64_t>& FieldsDespawned() const { return field_despawned_; }

    const std::unordered_map<std::uint64_t, ResourceDropEntity>& Drops() const { return drops_; }
    const std::vector<std::uint64_t>& DropsSpawned() const { return drop_spawned_; }
    const std::vector<std::uint64_t>& DropsDespawned() const { return drop_despawned_; }

    const std::unordered_map<std::uint64_t, PlayerRes>& PlayerResources() const { return player_res_; }
    const std::unordered_set<std::uint64_t>& ResourceDirty() const { return resource_dirty_; }

    const Unit* FindUnit(std::uint64_t id) const {
        auto it = units_.find(id);
        return it == units_.end() ? nullptr : &it->second;
    }

    const Building* FindBuilding(std::uint64_t id) const {
        auto it = buildings_.find(id);
        return it == buildings_.end() ? nullptr : &it->second;
    }

    const ResourceFieldEntity* FindField(std::uint64_t id) const {
        auto it = fields_.find(id);
        return it == fields_.end() ? nullptr : &it->second;
    }

    const ResourceDropEntity* FindDrop(std::uint64_t id) const {
        auto it = drops_.find(id);
        return it == drops_.end() ? nullptr : &it->second;
    }

    const PlayerRes* FindPlayerRes(std::uint64_t uid) const {
        auto it = player_res_.find(uid);
        return it == player_res_.end() ? nullptr : &it->second;
    }

    // LogicShard 在 ApplyPending 后取走被拒命令, 回 CommandRejectedNtf
    std::vector<RejectedCommand> TakeRejected() {
        std::vector<RejectedCommand> out;
        out.swap(rejected_);
        return out;
    }

    void ClearFrameChanges() {
        dirty_.clear();
        spawned_.clear();
        despawned_.clear();
        building_dirty_.clear();
        building_spawned_.clear();
        building_despawned_.clear();
        field_dirty_.clear();
        field_spawned_.clear();
        field_despawned_.clear();
        drop_spawned_.clear();
        drop_despawned_.clear();
        resource_dirty_.clear();
    }

private:
    // ---- 命令应用 (tick 边界) ----

    void ClearWorkerTask(Unit& u) {
        u.task = WorkerTask::None;
        u.gather_field_id = 0;
        u.dropoff_id = 0;
        u.pickup_drop_id = 0;
        u.build_target_id = 0;
        u.interaction_offset = Vec3f{};
        u.work_cd = 0.0f;
        u.path.clear();
        u.path_index = 0;
    }

    std::vector<Unit*> CollectOwnedUnits(
        std::uint64_t owner_uid,
        const std::vector<std::uint64_t>& unit_ids) {
        std::vector<Unit*> selected;
        selected.reserve(unit_ids.size());
        for (auto id : unit_ids) {
            auto it = units_.find(id);
            if (it != units_.end() && it->second.owner_uid == owner_uid) {
                selected.push_back(&it->second);
            }
        }
        std::sort(selected.begin(), selected.end(),
                  [](const Unit* lhs, const Unit* rhs) { return lhs->id < rhs->id; });
        return selected;
    }

    static Vec3f FormationSlot(std::size_t index,
                               std::size_t count,
                               float forward_x,
                               float forward_z) {
        if (count <= 1) {
            return Vec3f{};
        }

        const std::size_t columns = static_cast<std::size_t>(
            std::ceil(std::sqrt(static_cast<float>(count))));
        const std::size_t rows = (count + columns - 1) / columns;
        const std::size_t row = index / columns;
        const std::size_t column = index % columns;
        const std::size_t row_count = std::min(columns, count - row * columns);

        const float lateral =
            (static_cast<float>(column) - (static_cast<float>(row_count) - 1.0f) * 0.5f) * 2.8f;
        const float longitudinal =
            ((static_cast<float>(rows) - 1.0f) * 0.5f - static_cast<float>(row)) * 3.0f;
        const float right_x = forward_z;
        const float right_z = -forward_x;
        return Vec3f{
            right_x * lateral + forward_x * longitudinal,
            0.0f,
            right_z * lateral + forward_z * longitudinal};
    }

    static Vec3f InteractionSlot(std::size_t index, std::size_t count) {
        if (count == 0) {
            return Vec3f{};
        }
        constexpr float kPi = 3.14159265358979323846f;
        constexpr float kInteractionRadius = 1.8f;
        const float angle = 2.0f * kPi * static_cast<float>(index) /
                            static_cast<float>(count);
        return Vec3f{
            std::cos(angle) * kInteractionRadius,
            0.0f,
            std::sin(angle) * kInteractionRadius};
    }

    // 反作弊兜底: 只能指挥属于自己的单位
    void ApplyMove(std::uint64_t owner_uid,
                   const std::vector<std::uint64_t>& unit_ids,
                   const Vec3f& target,
                   const std::vector<Vec3f>& path = {}) {
        auto selected = CollectOwnedUnits(owner_uid, unit_ids);
        if (selected.empty()) {
            return;
        }

        Vec3f center {};
        for (const Unit* unit : selected) {
            center.x += unit->pos.x;
            center.y += unit->pos.y;
            center.z += unit->pos.z;
        }
        const float inverse_count = 1.0f / static_cast<float>(selected.size());
        center.x *= inverse_count;
        center.y *= inverse_count;
        center.z *= inverse_count;

        float forward_x = target.x - center.x;
        float forward_z = target.z - center.z;
        const float direction_length = std::sqrt(forward_x * forward_x + forward_z * forward_z);
        if (direction_length > 1e-4f) {
            forward_x /= direction_length;
            forward_z /= direction_length;
        } else {
            forward_x = 0.0f;
            forward_z = 1.0f;
        }

        for (std::size_t index = 0; index < selected.size(); ++index) {
            auto& u = *selected[index];
            const Vec3f formation_offset =
                FormationSlot(index, selected.size(), forward_x, forward_z);
            ClearWorkerTask(u);
            u.target = Vec3f{
                target.x + formation_offset.x,
                target.y,
                target.z + formation_offset.z};
            u.path = path;       // 方案A: 沿 waypoints 走; 空则直线走 target
            for (auto& waypoint : u.path) {
                waypoint.x += formation_offset.x;
                waypoint.z += formation_offset.z;
            }
            u.path_index = 0;
            u.has_target = true;
            u.attack_target = 0; // 移动取消攻击
            u.state = kStateMoving;
            dirty_.insert(u.id);
        }
    }

    void ApplyStop(std::uint64_t owner_uid,
                   const std::vector<std::uint64_t>& unit_ids) {
        for (auto id : unit_ids) {
            auto it = units_.find(id);
            if (it == units_.end() || it->second.owner_uid != owner_uid) {
                continue;
            }
            auto& u = it->second;
            ClearWorkerTask(u);
            u.has_target = false;
            u.attack_target = 0;
            u.state = kStateIdle;
            dirty_.insert(id);
        }
    }

    void ApplyAttack(std::uint64_t owner_uid,
                     const std::vector<std::uint64_t>& unit_ids,
                     std::uint64_t target_entity) {
        for (auto id : unit_ids) {
            auto it = units_.find(id);
            if (it == units_.end() || it->second.owner_uid != owner_uid) {
                continue;
            }
            if (id == target_entity) {
                continue; // 不能攻击自己
            }
            auto& u = it->second;
            ClearWorkerTask(u);
            u.attack_target = target_entity;
            u.has_target = false;
            u.state = kStateAttacking;
            dirty_.insert(id);
        }
    }

    void ApplyHarvest(std::uint64_t owner_uid,
                      const std::vector<std::uint64_t>& unit_ids,
                      std::uint64_t field_id) {
        auto field_it = fields_.find(field_id);
        if (field_it == fields_.end()) {
            return;
        }
        auto selected = CollectOwnedUnits(owner_uid, unit_ids);
        for (std::size_t index = 0; index < selected.size(); ++index) {
            auto& u = *selected[index];
            ClearWorkerTask(u);
            u.interaction_offset = InteractionSlot(index, selected.size());
            u.attack_target = 0;
            u.has_target = false;
            u.task = WorkerTask::Gathering;
            u.gather_field_id = field_id;
            u.target = field_it->second.pos;
            u.state = kStateMoving;
            dirty_.insert(u.id);
        }
    }

    void ApplyStore(std::uint64_t owner_uid,
                    const std::vector<std::uint64_t>& unit_ids,
                    std::uint64_t camp_id) {
        auto selected = CollectOwnedUnits(owner_uid, unit_ids);
        for (std::size_t index = 0; index < selected.size(); ++index) {
            auto& u = *selected[index];
            ClearWorkerTask(u);
            u.interaction_offset = InteractionSlot(index, selected.size());
            u.attack_target = 0;
            u.has_target = false;
            u.task = WorkerTask::Returning;
            u.dropoff_id = camp_id; // 0 表示自动找最近资源站
            u.state = kStateMoving;
            dirty_.insert(u.id);
        }
    }

    void ApplyPickup(std::uint64_t owner_uid,
                     const std::vector<std::uint64_t>& unit_ids,
                     std::uint64_t drop_id) {
        if (drops_.find(drop_id) == drops_.end()) {
            return;
        }
        auto selected = CollectOwnedUnits(owner_uid, unit_ids);
        for (std::size_t index = 0; index < selected.size(); ++index) {
            auto& u = *selected[index];
            ClearWorkerTask(u);
            u.interaction_offset = InteractionSlot(index, selected.size());
            u.attack_target = 0;
            u.has_target = false;
            u.task = WorkerTask::Pickup;
            u.pickup_drop_id = drop_id;
            u.state = kStateMoving;
            dirty_.insert(u.id);
        }
    }

    void ApplyBuild(const PendingCommand& cmd) {
        auto res_it = player_res_.find(cmd.owner_uid);
        if (res_it == player_res_.end()) {
            Reject(cmd, kErrInvalidRequest);
            return;
        }
        const std::uint32_t building_type = cmd.aux_type;
        if (building_type == kBuildingCrystal) {
            Reject(cmd, kErrInvalidRequest); // 主基地不可造
            return;
        }
        const ResCost cost = BuildingCost(building_type);
        if (!res_it->second.CanAfford(cost)) {
            Reject(cmd, kErrNotEnoughResources);
            return;
        }

        std::uint32_t team = TeamOf(cmd.owner_uid);
        res_it->second.Pay(cost);
        resource_dirty_.insert(cmd.owner_uid);

        const auto building_id = SpawnBuilding(cmd.owner_uid, team, building_type,
                                               cmd.target, cmd.yaw, /*under_construction=*/true);

        // 让本命令里的工人去施工
        AssignConstructors(cmd.owner_uid, cmd.unit_ids, building_id);
    }

    void ApplyConstruct(std::uint64_t owner_uid,
                        const std::vector<std::uint64_t>& unit_ids,
                        std::uint64_t building_id) {
        auto b_it = buildings_.find(building_id);
        if (b_it == buildings_.end() || !b_it->second.under_construction) {
            return;
        }
        AssignConstructors(owner_uid, unit_ids, building_id);
    }

    void AssignConstructors(std::uint64_t owner_uid,
                            const std::vector<std::uint64_t>& unit_ids,
                            std::uint64_t building_id) {
        for (auto id : unit_ids) {
            auto it = units_.find(id);
            if (it == units_.end() || it->second.owner_uid != owner_uid) {
                continue;
            }
            auto& u = it->second;
            ClearWorkerTask(u);
            u.attack_target = 0;
            u.has_target = false;
            u.task = WorkerTask::Constructing;
            u.build_target_id = building_id;
            u.state = kStateMoving;
            dirty_.insert(id);
        }
    }

    void ApplyTrain(const PendingCommand& cmd) {
        auto b_it = buildings_.find(cmd.target_entity);
        if (b_it == buildings_.end()) {
            Reject(cmd, kErrInvalidEntity);
            return;
        }
        Building& b = b_it->second;
        if (b.owner_uid != cmd.owner_uid || b.under_construction) {
            Reject(cmd, kErrInvalidEntity);
            return;
        }
        const std::uint32_t unit_type = cmd.aux_type;
        if (!BuildingTrainsUnit(b.building_type, unit_type)) {
            Reject(cmd, kErrInvalidRequest);
            return;
        }

        auto res_it = player_res_.find(cmd.owner_uid);
        if (res_it == player_res_.end()) {
            Reject(cmd, kErrInvalidRequest);
            return;
        }
        const ResCost cost = UnitCost(unit_type);
        if (!res_it->second.CanAfford(cost)) {
            Reject(cmd, kErrNotEnoughResources);
            return;
        }

        res_it->second.Pay(cost);
        resource_dirty_.insert(cmd.owner_uid);

        Building::TrainItem item;
        item.unit_type = unit_type;
        item.remaining = UnitTrainTime(unit_type);
        b.train_queue.push_back(item);
        building_dirty_.insert(b.id);
    }

    void Reject(const PendingCommand& cmd, int code) {
        rejected_.push_back(RejectedCommand{cmd.owner_uid, cmd.client_seq, code});
    }

    std::uint32_t TeamOf(std::uint64_t uid) const {
        auto it = players_.find(uid);
        return it == players_.end() ? 0u : it->second.team;
    }

    // ---- 仿真步进 ----

    // 朝目标点直线移动一步, 返回是否已到达 (dist<=arrive_dist)
    bool MoveToward(Unit& u, const Vec3f& dest, float arrive_dist, float dt) {
        const float dx = dest.x - u.pos.x;
        const float dz = dest.z - u.pos.z;
        const float dist = std::sqrt(dx * dx + dz * dz);
        if (dist <= arrive_dist) {
            return true;
        }
        const float step = u.speed * dt;
        if (step > 0.0f && dist > 1e-4f) {
            u.pos.x += dx / dist * step;
            u.pos.z += dz / dist * step;
            u.yaw = std::atan2(dx, dz);
        }
        return false;
    }

    void FaceToward(Unit& u, const Vec3f& target) {
        const float dx = target.x - u.pos.x;
        const float dz = target.z - u.pos.z;
        if ((dx * dx + dz * dz) > 1e-4f) {
            u.yaw = std::atan2(dx, dz);
        }
    }

    // 方案A: 有 path 则沿 waypoints 依次走 (一个 tick 内可跨多个近点), 否则直线走 target
    void StepMove(Unit& u, float dt) {
        float remaining = u.speed * dt;

        while (remaining > 1e-5f) {
            const bool on_path = !u.path.empty() && u.path_index < u.path.size();
            const Vec3f dest = on_path ? u.path[u.path_index] : u.target;

            const float dx = dest.x - u.pos.x;
            const float dz = dest.z - u.pos.z;
            const float dist = std::sqrt(dx * dx + dz * dz);

            if (dist <= remaining || dist < 1e-4f) {
                // 到达当前目标点
                u.pos = dest;
                remaining -= (dist > 0.0f ? dist : 0.0f);
                if (on_path) {
                    ++u.path_index;
                    if (u.path_index >= u.path.size()) {
                        u.path.clear();
                        u.path_index = 0;
                        u.has_target = false;
                        u.state = kStateIdle;
                        break;
                    }
                    continue; // 还有下一个 waypoint, 用剩余步长继续走
                }
                u.has_target = false;
                u.state = kStateIdle;
                break;
            }

            // 朝 dest 走一步
            u.pos.x += dx / dist * remaining;
            u.pos.z += dz / dist * remaining;
            u.yaw = std::atan2(dx, dz);
            u.state = kStateMoving;
            break;
        }

        dirty_.insert(u.id);
    }

    void StepCombat(Unit& u, float dt) {
        auto it = units_.find(u.attack_target);
        if (it == units_.end() || it->second.hp <= 0.0f) {
            // 目标已不存在/已死, 停手
            u.attack_target = 0;
            u.state = kStateIdle;
            dirty_.insert(u.id);
            return;
        }

        Unit& target = it->second;
        const float dx = target.pos.x - u.pos.x;
        const float dz = target.pos.z - u.pos.z;
        const float dist = std::sqrt(dx * dx + dz * dz);

        u.state = kStateAttacking; // attacking

        if (dist > kAttackRange) {
            // 不在射程内 -> 追击
            const float step = u.speed * dt;
            if (step > 0.0f && dist > 1e-4f) {
                u.pos.x += dx / dist * step;
                u.pos.z += dz / dist * step;
                u.yaw = std::atan2(dx, dz);
            }
            dirty_.insert(u.id);
            return;
        }

        // 射程内, 冷却好了就打一下
        if (u.attack_cd <= 0.0f) {
            target.hp -= kAttackDamage;
            if (target.hp < 0.0f) {
                target.hp = 0.0f;
            }
            u.attack_cd = kAttackCooldown;
            dirty_.insert(target.id);
        }
        dirty_.insert(u.id);
    }

    void StepGather(Unit& u, float dt) {
        // 背满了 -> 回去存
        if (u.carried_amount >= kVillagerCarryCapacity) {
            const ResourceFieldEntity* field = FindField(u.gather_field_id);
            SwitchToPreparingReturn(u, field != nullptr ? field->pos : u.target);
            return;
        }

        auto it = fields_.find(u.gather_field_id);
        if (it == fields_.end() || it->second.amount_left == 0) {
            // 资源场没了: 有货先回存, 否则发呆
            if (u.carried_amount > 0) {
                SwitchToPreparingReturn(u, u.target);
            } else {
                ClearWorkerTask(u);
                u.state = kStateIdle;
                dirty_.insert(u.id);
            }
            return;
        }

        ResourceFieldEntity& field = it->second;
        const Vec3f field_position = field.pos;
        u.target = field_position;
        const Vec3f work_position {
            field.pos.x + u.interaction_offset.x,
            field.pos.y,
            field.pos.z + u.interaction_offset.z};
        if (!MoveToward(u, work_position, 0.15f, dt)) {
            u.state = kStateMoving;
            dirty_.insert(u.id);
            return;
        }

        // 到达, 开采
        FaceToward(u, field_position);
        u.state = kStateWorking;
        u.carried_raw = field.raw;
        u.work_cd += dt;
        while (u.work_cd >= kHarvestInterval && u.carried_amount < kVillagerCarryCapacity
               && field.amount_left > 0) {
            const std::uint32_t take = std::min<std::uint32_t>(
                kHarvestAmount,
                std::min<std::uint32_t>(field.amount_left,
                                        kVillagerCarryCapacity - u.carried_amount));
            u.carried_amount += take;
            field.amount_left -= take;
            u.work_cd -= kHarvestInterval;
        }
        field_dirty_.insert(field.id);

        if (field.amount_left == 0) {
            // 采空 -> 资源场消失
            field_despawned_.push_back(field.id);
            fields_.erase(it);
            if (u.carried_amount > 0) {
                SwitchToPreparingReturn(u, field_position);
                return;
            }
        }

        if (u.carried_amount >= kVillagerCarryCapacity) {
            SwitchToPreparingReturn(u, field_position);
            return;
        }
        dirty_.insert(u.id);
    }

    void SwitchToPreparingReturn(Unit& u, const Vec3f& face_target) {
        u.task = WorkerTask::PreparingReturn;
        u.target = face_target;
        u.has_target = false;
        u.work_cd = kCarryLiftDuration;
        u.state = kStateWorking;
        FaceToward(u, face_target);
        dirty_.insert(u.id);
    }

    void StepPreparingReturn(Unit& u, float dt) {
        FaceToward(u, u.target);
        u.state = kStateWorking;
        u.work_cd -= dt;
        if (u.work_cd > 0.0f) {
            dirty_.insert(u.id);
            return;
        }
        SwitchToReturn(u);
    }

    void SwitchToReturn(Unit& u) {
        u.task = WorkerTask::Returning;
        u.work_cd = 0.0f;
        u.state = kStateMoving;
        dirty_.insert(u.id);
    }

    void StepReturn(Unit& u, float dt) {
        if (u.carried_amount == 0) {
            // 没货了, 尝试回到原资源场继续采
            if (u.gather_field_id != 0 && fields_.find(u.gather_field_id) != fields_.end()) {
                u.task = WorkerTask::Gathering;
                u.state = kStateMoving;
            } else {
                ClearWorkerTask(u);
                u.state = kStateIdle;
            }
            dirty_.insert(u.id);
            return;
        }

        const std::uint32_t res_type = RawToResourceType(u.carried_raw);
        const Building* camp = nullptr;
        if (u.dropoff_id != 0) {
            camp = FindBuilding(u.dropoff_id);
            if (camp == nullptr || camp->under_construction || camp->team != u.team) {
                camp = nullptr;
            }
        }
        if (camp == nullptr) {
            camp = FindNearestDropoff(u.team, u.pos);
            if (camp != nullptr) {
                u.dropoff_id = camp->id;
            }
        }

        if (camp == nullptr) {
            // 没有可用资源站, 发呆 (保留背包)
            u.state = kStateIdle;
            dirty_.insert(u.id);
            return;
        }

        const Vec3f dropoff_position {
            camp->pos.x + u.interaction_offset.x,
            camp->pos.y,
            camp->pos.z + u.interaction_offset.z};
        if (!MoveToward(u, dropoff_position, 0.15f, dt)) {
            u.state = kStateMoving;
            dirty_.insert(u.id);
            return;
        }

        // 到站, 入账
        auto res_it = player_res_.find(u.owner_uid);
        if (res_it != player_res_.end()) {
            res_it->second.Add(res_type, u.carried_amount);
            resource_dirty_.insert(u.owner_uid);
        }
        u.carried_amount = 0;

        // 回去继续采
        if (u.gather_field_id != 0 && fields_.find(u.gather_field_id) != fields_.end()) {
            u.task = WorkerTask::Gathering;
            u.state = kStateMoving;
        } else {
            ClearWorkerTask(u);
            u.state = kStateIdle;
        }
        dirty_.insert(u.id);
    }

    void StepPickup(Unit& u, float dt) {
        auto it = drops_.find(u.pickup_drop_id);
        if (it == drops_.end()) {
            // 掉落物没了
            if (u.carried_amount > 0) {
                SwitchToReturn(u);
            } else {
                ClearWorkerTask(u);
                u.state = kStateIdle;
                dirty_.insert(u.id);
            }
            return;
        }

        ResourceDropEntity& drop = it->second;
        const Vec3f pickup_position {
            drop.pos.x + u.interaction_offset.x,
            drop.pos.y,
            drop.pos.z + u.interaction_offset.z};
        if (!MoveToward(u, pickup_position, 0.15f, dt)) {
            u.state = kStateMoving;
            dirty_.insert(u.id);
            return;
        }

        // 捡起 (不超过背包上限)
        u.carried_raw = drop.raw;
        const std::uint32_t space = kVillagerCarryCapacity - u.carried_amount;
        const std::uint32_t take = std::min<std::uint32_t>(space, drop.amount);
        u.carried_amount += take;
        drop.amount -= take;

        if (drop.amount == 0) {
            drop_despawned_.push_back(drop.id);
            drops_.erase(it);
        }

        SwitchToReturn(u); // 捡完自动去存
        dirty_.insert(u.id);
    }

    void StepConstruct(Unit& u, float dt) {
        auto it = buildings_.find(u.build_target_id);
        if (it == buildings_.end() || !it->second.under_construction) {
            ClearWorkerTask(u);
            u.state = kStateIdle;
            dirty_.insert(u.id);
            return;
        }

        Building& b = it->second;
        if (!MoveToward(u, b.pos, kBuildRange, dt)) {
            u.state = kStateMoving;
            dirty_.insert(u.id);
            return;
        }

        // 施工
        u.state = kStateWorking;
        b.constructed_percent += kConstructPercentPerSec * dt;
        if (b.constructed_percent >= 100.0f) {
            b.constructed_percent = 100.0f;
            b.under_construction = false;
            b.hp = b.max_hp;
            // 完工, 工人空闲
            ClearWorkerTask(u);
            u.state = kStateIdle;
        } else {
            b.hp = std::max(b.hp, b.max_hp * b.constructed_percent / 100.0f);
        }
        building_dirty_.insert(b.id);
        dirty_.insert(u.id);
    }

    void StepBuildings(float dt) {
        for (auto& [id, b] : buildings_) {
            if (b.train_queue.empty() || b.under_construction) {
                continue;
            }
            auto& item = b.train_queue.front();
            item.remaining -= dt;
            if (item.remaining <= 0.0f) {
                // 训练完成, 在建筑旁出兵
                Vec3f spawn_pos = b.pos;
                spawn_pos.x += 2.0f;
                SpawnUnit(b.owner_uid, b.team, item.unit_type, spawn_pos,
                          100.0f, 3.0f);
                b.train_queue.erase(b.train_queue.begin());
            }
            building_dirty_.insert(b.id);
        }
    }

    void RemoveDead() {
        for (auto it = units_.begin(); it != units_.end();) {
            if (it->second.hp <= 0.0f) {
                despawned_.push_back(it->first);
                it = units_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = buildings_.begin(); it != buildings_.end();) {
            if (it->second.hp <= 0.0f) {
                building_despawned_.push_back(it->first);
                it = buildings_.erase(it);
            } else {
                ++it;
            }
        }
    }

    const Building* FindNearestDropoff(std::uint32_t team, const Vec3f& from) const {
        const Building* best = nullptr;
        float best_dist = std::numeric_limits<float>::max();
        for (const auto& [id, b] : buildings_) {
            if (b.team != team || b.under_construction) {
                continue;
            }
            if (b.building_type != kBuildingResourceCamp &&
                b.building_type != kBuildingCrystal) {
                continue;
            }
            const float dx = b.pos.x - from.x;
            const float dz = b.pos.z - from.z;
            const float d = dx * dx + dz * dz;
            if (d < best_dist) {
                best_dist = d;
                best = &b;
            }
        }
        return best;
    }

    std::uint64_t room_id_ {};
    std::string room_name_;
    std::uint32_t max_players_ {2};
    bool started_ {false};
    std::uint64_t server_tick_ {};
    std::uint64_t next_command_id_ {};
    std::unordered_map<std::uint64_t, RoomPlayerState> players_;
    std::unordered_map<std::uint64_t, PlayerRes> player_res_;

    // world state
    std::uint64_t next_entity_id_ {1};
    std::unordered_map<std::uint64_t, Unit> units_;
    std::unordered_map<std::uint64_t, Building> buildings_;
    std::unordered_map<std::uint64_t, ResourceFieldEntity> fields_;
    std::unordered_map<std::uint64_t, ResourceDropEntity> drops_;

    // 帧变化集合
    std::unordered_set<std::uint64_t> dirty_;
    std::vector<std::uint64_t> spawned_;
    std::vector<std::uint64_t> despawned_;
    std::unordered_set<std::uint64_t> building_dirty_;
    std::vector<std::uint64_t> building_spawned_;
    std::vector<std::uint64_t> building_despawned_;
    std::unordered_set<std::uint64_t> field_dirty_;
    std::vector<std::uint64_t> field_spawned_;
    std::vector<std::uint64_t> field_despawned_;
    std::vector<std::uint64_t> drop_spawned_;
    std::vector<std::uint64_t> drop_despawned_;
    std::unordered_set<std::uint64_t> resource_dirty_;

    std::vector<PendingCommand> pending_;
    std::vector<RejectedCommand> rejected_;

    // 战斗 / 胜负
    std::uint32_t initial_team_count_ {};
    std::uint32_t initial_crystal_team_count_ {};
    bool battle_begun_ {false};
    bool game_over_ {false};
};
