#include "DungeonRoom.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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
