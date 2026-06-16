#include "DungeonRoom.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using Catch::Approx;

namespace {

PendingCommand MakeMove(std::uint64_t owner,
                        std::vector<std::uint64_t> ids,
                        float x, float z) {
    PendingCommand cmd;
    cmd.type = CommandType::Move;
    cmd.owner_uid = owner;
    cmd.unit_ids = std::move(ids);
    cmd.target.x = x;
    cmd.target.z = z;
    return cmd;
}

PendingCommand MakeAttack(std::uint64_t owner,
                          std::vector<std::uint64_t> ids,
                          std::uint64_t target_entity) {
    PendingCommand cmd;
    cmd.type = CommandType::Attack;
    cmd.owner_uid = owner;
    cmd.unit_ids = std::move(ids);
    cmd.target_entity = target_entity;
    return cmd;
}

bool DespawnedContains(const DungeonRoom& room, std::uint64_t id) {
    const auto& d = room.Despawned();
    return std::find(d.begin(), d.end(), id) != d.end();
}

} // namespace

TEST_CASE("AddPlayer assigns sequential teams and respects capacity") {
    DungeonRoom room(1, "r", 2);

    std::uint32_t team = 99;
    REQUIRE(room.AddPlayer(100, &team));
    REQUIRE(team == 0);

    REQUIRE(room.AddPlayer(200, &team));
    REQUIRE(team == 1);

    // room is full now
    REQUIRE_FALSE(room.AddPlayer(300, &team));

    // adding an existing player is idempotent and returns their team
    team = 99;
    REQUIRE(room.AddPlayer(100, &team));
    REQUIRE(team == 0);

    REQUIRE(room.HasPlayer(100));
    REQUIRE_FALSE(room.HasPlayer(300));
}

TEST_CASE("AllReady only true once every player is ready") {
    DungeonRoom room(1, "r", 2);
    room.AddPlayer(100);
    room.AddPlayer(200);

    REQUIRE_FALSE(room.AllReady());
    REQUIRE(room.SetReady(100, true));
    REQUIRE_FALSE(room.AllReady());
    REQUIRE(room.SetReady(200, true));
    REQUIRE(room.AllReady());
}

TEST_CASE("SpawnUnit assigns unique ids and records spawns") {
    DungeonRoom room(1, "r", 2);

    const auto a = room.SpawnUnit(100, 0, 0, Vec3f{1.0f, 0.0f, 2.0f}, 100.0f, 5.0f);
    const auto b = room.SpawnUnit(100, 0, 0, Vec3f{3.0f, 0.0f, 4.0f}, 100.0f, 5.0f);

    REQUIRE(a != b);
    REQUIRE(room.Units().size() == 2);
    REQUIRE(room.Spawned().size() == 2);

    const Unit* unit = room.FindUnit(a);
    REQUIRE(unit != nullptr);
    REQUIRE(unit->owner_uid == 100);
    REQUIRE(unit->pos.x == Approx(1.0f));
    REQUIRE(unit->pos.z == Approx(2.0f));
    REQUIRE_FALSE(unit->has_target);
}

TEST_CASE("Move command only affects units the commander owns") {
    DungeonRoom room(1, "r", 2);
    const auto mine = room.SpawnUnit(100, 0, 0, Vec3f{}, 100.0f, 5.0f);
    const auto other = room.SpawnUnit(200, 1, 0, Vec3f{}, 100.0f, 5.0f);
    room.ClearFrameChanges();

    // player 100 tries to move BOTH units
    room.EnqueueCommand(MakeMove(100, {mine, other}, 10.0f, 0.0f));
    room.ApplyPending();

    REQUIRE(room.FindUnit(mine)->has_target);       // own unit accepted
    REQUIRE_FALSE(room.FindUnit(other)->has_target); // other's unit rejected

    REQUIRE(room.Dirty().count(mine) == 1);
    REQUIRE(room.Dirty().count(other) == 0);
}

TEST_CASE("Group move assigns distinct formation slots even when units overlap") {
    DungeonRoom room(1, "r", 2);
    std::vector<std::uint64_t> ids;
    for (int i = 0; i < 6; ++i) {
        ids.push_back(room.SpawnUnit(
            100, 0, 0, Vec3f{0.0f, 0.0f, 0.0f}, 100.0f, 10.0f));
    }

    room.EnqueueCommand(MakeMove(100, ids, 20.0f, 20.0f));
    room.ApplyPending();

    for (std::size_t i = 0; i < ids.size(); ++i) {
        const Unit* lhs = room.FindUnit(ids[i]);
        REQUIRE(lhs != nullptr);
        for (std::size_t j = i + 1; j < ids.size(); ++j) {
            const Unit* rhs = room.FindUnit(ids[j]);
            REQUIRE(rhs != nullptr);
            const float dx = lhs->target.x - rhs->target.x;
            const float dz = lhs->target.z - rhs->target.z;
            REQUIRE(std::sqrt(dx * dx + dz * dz) > 2.0f);
        }
    }
}

TEST_CASE("Step moves a unit toward its target and stops on arrival") {
    DungeonRoom room(1, "r", 2);
    const auto id = room.SpawnUnit(100, 0, 0, Vec3f{0.0f, 0.0f, 0.0f}, 100.0f, 10.0f);

    room.EnqueueCommand(MakeMove(100, {id}, 5.0f, 0.0f));
    room.ApplyPending();
    room.ClearFrameChanges();

    // speed 10, dt 0.1 => 1.0 unit per tick
    room.Step(0.1f);
    REQUIRE(room.FindUnit(id)->pos.x == Approx(1.0f));
    REQUIRE(room.FindUnit(id)->has_target);
    REQUIRE(room.FindUnit(id)->state == 1);     // moving
    REQUIRE(room.Dirty().count(id) == 1);       // moving marks dirty

    // run enough ticks to arrive (5 units / 1.0 per tick)
    for (int i = 0; i < 10; ++i) {
        room.Step(0.1f);
    }

    const Unit* unit = room.FindUnit(id);
    REQUIRE(unit->pos.x == Approx(5.0f));
    REQUIRE_FALSE(unit->has_target);
    REQUIRE(unit->state == 0);                  // idle after arrival
}

TEST_CASE("Idle units are not moved and ClearFrameChanges resets tracking") {
    DungeonRoom room(1, "r", 2);
    const auto id = room.SpawnUnit(100, 0, 0, Vec3f{2.0f, 0.0f, 2.0f}, 100.0f, 10.0f);
    room.ClearFrameChanges();

    room.Step(0.1f); // no target -> no movement, no dirty
    REQUIRE(room.FindUnit(id)->pos.x == Approx(2.0f));
    REQUIRE(room.Dirty().empty());

    // populate then clear
    room.SpawnUnit(100, 0, 0, Vec3f{}, 100.0f, 10.0f);
    REQUIRE_FALSE(room.Spawned().empty());
    room.ClearFrameChanges();
    REQUIRE(room.Spawned().empty());
    REQUIRE(room.Dirty().empty());
    REQUIRE(room.Despawned().empty());
}

TEST_CASE("Stop command clears the target") {
    DungeonRoom room(1, "r", 2);
    const auto id = room.SpawnUnit(100, 0, 0, Vec3f{}, 100.0f, 10.0f);

    room.EnqueueCommand(MakeMove(100, {id}, 9.0f, 0.0f));
    room.ApplyPending();
    REQUIRE(room.FindUnit(id)->has_target);

    PendingCommand stop;
    stop.type = CommandType::Stop;
    stop.owner_uid = 100;
    stop.unit_ids = {id};
    room.EnqueueCommand(stop);
    room.ApplyPending();

    REQUIRE_FALSE(room.FindUnit(id)->has_target);
    REQUIRE(room.FindUnit(id)->state == 0);
}

TEST_CASE("Attack in range deals damage and respects cooldown") {
    DungeonRoom room(1, "r", 2);
    const auto a = room.SpawnUnit(100, 0, 0, Vec3f{0.0f, 0.0f, 0.0f}, 100.0f, 10.0f);
    const auto b = room.SpawnUnit(200, 1, 0, Vec3f{1.0f, 0.0f, 0.0f}, 100.0f, 10.0f);

    room.EnqueueCommand(MakeAttack(100, {a}, b));
    room.ApplyPending();

    // in range (dist 1 < 2): first tick lands a hit
    room.Step(0.1f);
    REQUIRE(room.FindUnit(b)->hp == Approx(100.0f - kAttackDamage));
    REQUIRE(room.FindUnit(a)->state == 3); // attacking

    // cooldown not yet elapsed -> no further damage
    room.Step(0.1f);
    REQUIRE(room.FindUnit(b)->hp == Approx(100.0f - kAttackDamage));
}

TEST_CASE("Out-of-range attacker chases the target") {
    DungeonRoom room(1, "r", 2);
    const auto a = room.SpawnUnit(100, 0, 0, Vec3f{0.0f, 0.0f, 0.0f}, 100.0f, 10.0f);
    const auto b = room.SpawnUnit(200, 1, 0, Vec3f{10.0f, 0.0f, 0.0f}, 100.0f, 10.0f);

    room.EnqueueCommand(MakeAttack(100, {a}, b));
    room.ApplyPending();

    room.Step(0.1f); // speed 10, dt 0.1 -> moves 1.0 toward target, no damage
    REQUIRE(room.FindUnit(a)->pos.x == Approx(1.0f));
    REQUIRE(room.FindUnit(b)->hp == Approx(100.0f));
    REQUIRE(room.FindUnit(a)->state == 3);
}

TEST_CASE("A unit dies and is despawned when hp is depleted") {
    DungeonRoom room(1, "r", 2);
    const auto a = room.SpawnUnit(100, 0, 0, Vec3f{0.0f, 0.0f, 0.0f}, 100.0f, 10.0f);
    const auto b = room.SpawnUnit(200, 1, 0, Vec3f{1.0f, 0.0f, 0.0f}, 100.0f, 10.0f);

    room.EnqueueCommand(MakeAttack(100, {a}, b));
    room.ApplyPending();

    // dt 1.0 fully consumes the 1.0s cooldown each tick -> 20 dmg/tick, 5 ticks to kill
    for (int i = 0; i < 5; ++i) {
        room.Step(1.0f);
    }

    REQUIRE(room.FindUnit(b) == nullptr);
    REQUIRE(DespawnedContains(room, b));
    REQUIRE(room.Units().size() == 1);
}

TEST_CASE("Game over fires when one team is eliminated") {
    DungeonRoom room(1, "r", 2);
    const auto a = room.SpawnUnit(100, 0, 0, Vec3f{0.0f, 0.0f, 0.0f}, 100.0f, 10.0f);
    const auto b = room.SpawnUnit(200, 1, 0, Vec3f{1.0f, 0.0f, 0.0f}, 100.0f, 10.0f);
    room.BeginBattle();

    std::uint32_t winner = 99;
    REQUIRE_FALSE(room.CheckGameOver(winner)); // both teams alive

    room.EnqueueCommand(MakeAttack(100, {a}, b));
    room.ApplyPending();
    for (int i = 0; i < 5; ++i) {
        room.Step(1.0f);
    }

    REQUIRE(room.CheckGameOver(winner));
    REQUIRE(winner == 0); // team 0 (unit a) survives
}

TEST_CASE("Single-team room never declares game over") {
    DungeonRoom room(1, "r", 2);
    room.SpawnUnit(100, 0, 0, Vec3f{}, 100.0f, 10.0f);
    room.SpawnUnit(100, 0, 0, Vec3f{}, 100.0f, 10.0f);
    room.BeginBattle();

    std::uint32_t winner = 99;
    REQUIRE_FALSE(room.CheckGameOver(winner));
}

TEST_CASE("Destroying the enemy crystal ends the game") {
    DungeonRoom room(1, "r", 2);
    // 双方各一座水晶 (1000 血), team 0 派一名攻击者去拆 team 1 的水晶
    room.SpawnBuilding(100, 0, kBuildingCrystal, Vec3f{0.0f, 0.0f, 0.0f}, 0.0f, false);
    const auto enemy_crystal =
        room.SpawnBuilding(200, 1, kBuildingCrystal, Vec3f{1.0f, 0.0f, 0.0f}, 0.0f, false);
    const auto soldier = room.SpawnUnit(100, 0, 0, Vec3f{0.0f, 0.0f, 0.0f}, 100.0f, 10.0f);
    room.BeginBattle();

    std::uint32_t winner = 99;
    REQUIRE_FALSE(room.CheckGameOver(winner)); // 两座水晶都在 -> 未分胜负

    room.EnqueueCommand(MakeAttack(100, {soldier}, enemy_crystal));
    room.ApplyPending();

    // 1000 血 / 20 每秒 -> ~50 个 1 秒 tick 拆完 (多给几次确保打掉)
    for (int i = 0; i < 60; ++i) {
        room.Step(1.0f);
    }

    REQUIRE(room.FindBuilding(enemy_crystal) == nullptr); // 水晶已被摧毁并清理
    REQUIRE(room.CheckGameOver(winner));
    REQUIRE(winner == 0); // team 0 的水晶仍在 -> team 0 获胜
}

namespace {

PendingCommand MakeHarvest(std::uint64_t owner,
                           std::vector<std::uint64_t> ids,
                           std::uint64_t field_id) {
    PendingCommand cmd;
    cmd.type = CommandType::Harvest;
    cmd.owner_uid = owner;
    cmd.unit_ids = std::move(ids);
    cmd.target_entity = field_id;
    return cmd;
}

PendingCommand MakeTrain(std::uint64_t owner,
                         std::uint64_t building_id,
                         std::uint32_t unit_type) {
    PendingCommand cmd;
    cmd.type = CommandType::Train;
    cmd.owner_uid = owner;
    cmd.target_entity = building_id;
    cmd.aux_type = unit_type;
    return cmd;
}

} // namespace

TEST_CASE("Move with a waypoint path follows the polyline, not a straight line") {
    DungeonRoom room(1, "r", 2);
    const auto id = room.SpawnUnit(100, 0, 0, Vec3f{0.0f, 0.0f, 0.0f}, 100.0f, 10.0f);

    PendingCommand cmd;
    cmd.type = CommandType::Move;
    cmd.owner_uid = 100;
    cmd.unit_ids = {id};
    cmd.path = { Vec3f{0.0f, 0.0f, 5.0f}, Vec3f{5.0f, 0.0f, 5.0f} }; // +Z 然后 +X
    cmd.target = Vec3f{5.0f, 0.0f, 5.0f};
    room.EnqueueCommand(cmd);
    room.ApplyPending();

    // speed 10, dt 0.1 => 1.0/tick; 第一段长 5 => 5 个 tick 到第一个拐点
    for (int i = 0; i < 5; ++i) room.Step(0.1f);
    const Unit* mid = room.FindUnit(id);
    REQUIRE(mid->pos.x == Approx(0.0f).margin(0.05)); // 走了 +Z 而非斜穿
    REQUIRE(mid->pos.z == Approx(5.0f).margin(0.05));

    for (int i = 0; i < 15; ++i) room.Step(0.1f);
    const Unit* end = room.FindUnit(id);
    REQUIRE(end->pos.x == Approx(5.0f));
    REQUIRE(end->pos.z == Approx(5.0f));
    REQUIRE_FALSE(end->has_target);
    REQUIRE(end->state == 0); // 到达后 idle
}

TEST_CASE("Worker harvests a field and deposits resources at a dropoff") {
    DungeonRoom room(1, "r", 2);
    room.AddPlayer(100);
    room.SpawnBuilding(100, 0, kBuildingCrystal, Vec3f{0.0f, 0.0f, 0.0f}, 0.0f, false);
    const auto field = room.SpawnResourceField(kRawWood, Vec3f{5.0f, 0.0f, 0.0f}, 100);
    const auto worker = room.SpawnUnit(100, 0, kUnitVillager, Vec3f{0.0f, 0.0f, 0.0f}, 100.0f, 10.0f);
    room.BeginBattle();

    const std::uint32_t wood_before = room.FindPlayerRes(100)->wood;

    room.EnqueueCommand(MakeHarvest(100, {worker}, field));
    room.ApplyPending();

    for (int i = 0; i < 200; ++i) {
        room.Step(0.1f);
    }

    REQUIRE(room.FindPlayerRes(100)->wood > wood_before);
    REQUIRE(room.FindField(field)->amount_left < 100u);
}

TEST_CASE("Build deducts cost, creates a site, and construction completes it") {
    DungeonRoom room(1, "r", 2);
    room.AddPlayer(100);
    room.BeginBattle();
    const auto worker = room.SpawnUnit(100, 0, kUnitVillager, Vec3f{0.0f, 0.0f, 0.0f}, 100.0f, 10.0f);

    const std::uint32_t wood_before = room.FindPlayerRes(100)->wood;

    PendingCommand build;
    build.type = CommandType::Build;
    build.owner_uid = 100;
    build.unit_ids = {worker};
    build.aux_type = kBuildingResourceCamp;
    build.target = Vec3f{2.0f, 0.0f, 0.0f};
    room.EnqueueCommand(build);
    room.ApplyPending();

    REQUIRE(room.FindPlayerRes(100)->wood == wood_before - BuildingCost(kBuildingResourceCamp).wood);
    REQUIRE(room.Buildings().size() == 1);

    for (int i = 0; i < 200; ++i) {
        room.Step(0.1f);
    }

    const Building* built = nullptr;
    for (const auto& [id, b] : room.Buildings()) {
        built = &b;
    }
    REQUIRE(built != nullptr);
    REQUIRE_FALSE(built->under_construction);
    REQUIRE(built->constructed_percent == Approx(100.0f));
}

TEST_CASE("Training queues a unit and spawns it after the timer") {
    DungeonRoom room(1, "r", 2);
    room.AddPlayer(100);
    const auto inn = room.SpawnBuilding(100, 0, kBuildingVillagerInn, Vec3f{}, 0.0f, false);
    room.BeginBattle();

    const std::size_t units_before = room.Units().size();

    room.EnqueueCommand(MakeTrain(100, inn, kUnitVillager));
    room.ApplyPending();
    REQUIRE(room.FindPlayerRes(100)->food == kStartFood - UnitCost(kUnitVillager).food);

    for (int i = 0; i < 60; ++i) { // 5s train time / 0.1 = 50 ticks
        room.Step(0.1f);
    }

    REQUIRE(room.Units().size() == units_before + 1);
}

TEST_CASE("Commands are rejected when the player cannot afford them") {
    DungeonRoom room(1, "r", 2);
    room.AddPlayer(100);
    const auto inn = room.SpawnBuilding(100, 0, kBuildingVillagerInn, Vec3f{}, 0.0f, false);
    room.BeginBattle(); // food = kStartFood (200), villager costs 50 => 4 affordable

    for (int i = 0; i < 4; ++i) {
        room.EnqueueCommand(MakeTrain(100, inn, kUnitVillager));
    }
    room.ApplyPending();
    REQUIRE(room.FindPlayerRes(100)->food == 0u);
    REQUIRE(room.TakeRejected().empty());

    // 5th training has no food left
    room.EnqueueCommand(MakeTrain(100, inn, kUnitVillager));
    room.ApplyPending();

    auto rejected = room.TakeRejected();
    REQUIRE(rejected.size() == 1);
    REQUIRE(rejected[0].code == kErrNotEnoughResources);
}
