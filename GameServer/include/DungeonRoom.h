#pragma once

#include <cmath>
#include <cstdint>
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

// 战斗参数(单一兵种, 暂用共享常量)
inline constexpr float kAttackRange = 2.0f;
inline constexpr float kAttackDamage = 20.0f;
inline constexpr float kAttackCooldown = 1.0f; // 秒

struct Unit {
    std::uint64_t id {};
    std::uint64_t owner_uid {};
    std::uint32_t team {};
    std::uint32_t unit_type {};
    Vec3f pos {};
    Vec3f target {};
    bool has_target {false};
    float yaw {};
    float hp {};
    float speed {};
    std::uint32_t state {}; // 对应 rts::v1::UnitState
    std::uint64_t attack_target {}; // 攻击目标实体 id, 0 表示无
    float attack_cd {};             // 距离下次攻击的剩余冷却(秒)
};

enum class CommandType : std::uint8_t {
    Move,
    Stop,
    Attack,
};

// 客户端意图先入队, 到 tick 边界才统一应用 (proto-free, 便于单测)
struct PendingCommand {
    CommandType type {CommandType::Move};
    std::uint64_t owner_uid {};
    std::vector<std::uint64_t> unit_ids;
    Vec3f target {};
    std::uint64_t target_entity {}; // 攻击目标实体 id
};

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
                            float speed) {
        const auto id = next_entity_id_++;
        Unit u;
        u.id = id;
        u.owner_uid = owner_uid;
        u.team = team;
        u.unit_type = unit_type;
        u.pos = pos;
        u.target = pos;
        u.has_target = false;
        u.yaw = 0.0f;
        u.hp = hp;
        u.speed = speed;
        u.state = 0;
        units_.emplace(id, u);
        spawned_.push_back(id);
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
                ApplyMove(cmd.owner_uid, cmd.unit_ids, cmd.target);
                break;
            case CommandType::Stop:
                ApplyStop(cmd.owner_uid, cmd.unit_ids);
                break;
            case CommandType::Attack:
                ApplyAttack(cmd.owner_uid, cmd.unit_ids, cmd.target_entity);
                break;
            }
        }
        pending_.clear();
    }

    // 推进一个固定步长: 移动 / 追击 / 攻击, 最后清理死亡单位
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
            } else if (u.has_target) {
                StepMove(u, dt);
            }
        }

        RemoveDead();
    }

    // 战斗开始时记录初始阵营数, 用于判定团灭
    void BeginBattle() {
        std::unordered_set<std::uint32_t> teams;
        for (const auto& [id, u] : units_) {
            teams.insert(u.team);
        }
        initial_team_count_ = static_cast<std::uint32_t>(teams.size());
        battle_begun_ = true;
    }

    bool IsGameOver() const { return game_over_; }
    void SetGameOver(bool over) { game_over_ = over; }

    // 还剩 <=1 个阵营有存活单位时结束; 单阵营房间(测试)不触发
    bool CheckGameOver(std::uint32_t& winner_team) const {
        if (!battle_begun_ || initial_team_count_ < 2) {
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

    const std::unordered_map<std::uint64_t, Unit>& Units() const { return units_; }
    const std::unordered_set<std::uint64_t>& Dirty() const { return dirty_; }
    const std::vector<std::uint64_t>& Spawned() const { return spawned_; }
    const std::vector<std::uint64_t>& Despawned() const { return despawned_; }

    const Unit* FindUnit(std::uint64_t id) const {
        auto it = units_.find(id);
        return it == units_.end() ? nullptr : &it->second;
    }

    void ClearFrameChanges() {
        dirty_.clear();
        spawned_.clear();
        despawned_.clear();
    }

private:
    // 反作弊兜底: 只能指挥属于自己的单位
    void ApplyMove(std::uint64_t owner_uid,
                   const std::vector<std::uint64_t>& unit_ids,
                   const Vec3f& target) {
        for (auto id : unit_ids) {
            auto it = units_.find(id);
            if (it == units_.end() || it->second.owner_uid != owner_uid) {
                continue;
            }
            it->second.target = target;
            it->second.has_target = true;
            it->second.attack_target = 0; // 移动取消攻击
            it->second.state = 1;
            dirty_.insert(id);
        }
    }

    void ApplyStop(std::uint64_t owner_uid,
                   const std::vector<std::uint64_t>& unit_ids) {
        for (auto id : unit_ids) {
            auto it = units_.find(id);
            if (it == units_.end() || it->second.owner_uid != owner_uid) {
                continue;
            }
            it->second.has_target = false;
            it->second.attack_target = 0;
            it->second.state = 0;
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
            it->second.attack_target = target_entity;
            it->second.has_target = false;
            it->second.state = 3; // attacking
            dirty_.insert(id);
        }
    }

    void StepMove(Unit& u, float dt) {
        const float dx = u.target.x - u.pos.x;
        const float dz = u.target.z - u.pos.z;
        const float dist = std::sqrt(dx * dx + dz * dz);
        const float step = u.speed * dt;

        if (dist <= step || dist < 1e-4f) {
            u.pos = u.target;
            u.has_target = false;
            u.state = 0; // idle
        } else {
            u.pos.x += dx / dist * step;
            u.pos.z += dz / dist * step;
            u.yaw = std::atan2(dx, dz);
            u.state = 1; // moving
        }

        dirty_.insert(u.id);
    }

    void StepCombat(Unit& u, float dt) {
        auto it = units_.find(u.attack_target);
        if (it == units_.end() || it->second.hp <= 0.0f) {
            // 目标已不存在/已死, 停手
            u.attack_target = 0;
            u.state = 0;
            dirty_.insert(u.id);
            return;
        }

        Unit& target = it->second;
        const float dx = target.pos.x - u.pos.x;
        const float dz = target.pos.z - u.pos.z;
        const float dist = std::sqrt(dx * dx + dz * dz);

        u.state = 3; // attacking

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

    void RemoveDead() {
        for (auto it = units_.begin(); it != units_.end();) {
            if (it->second.hp <= 0.0f) {
                despawned_.push_back(it->first);
                it = units_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::uint64_t room_id_ {};
    std::string room_name_;
    std::uint32_t max_players_ {2};
    bool started_ {false};
    std::uint64_t server_tick_ {};
    std::uint64_t next_command_id_ {};
    std::unordered_map<std::uint64_t, RoomPlayerState> players_;

    // world state
    std::uint64_t next_entity_id_ {1};
    std::unordered_map<std::uint64_t, Unit> units_;
    std::unordered_set<std::uint64_t> dirty_;
    std::vector<std::uint64_t> spawned_;
    std::vector<std::uint64_t> despawned_;
    std::vector<PendingCommand> pending_;

    // 战斗 / 胜负
    std::uint32_t initial_team_count_ {};
    bool battle_begun_ {false};
    bool game_over_ {false};
};
