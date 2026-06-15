#include "../include/LogicRouter.h"

#include "../include/LogicShard.h"

#include <utility>

void LogicRouter::Init() {
    if (initialized_) {
        return;
    }

    Register(MsgId::CreateRoomReq, [](LogicShard* shard, LogicTask task) {
        if (shard) {
            shard->HandleCreateRoom(std::move(task));
        }
    });

    Register(MsgId::JoinRoomReq, [](LogicShard* shard, LogicTask task) {
        if (shard) {
            shard->HandleJoinRoom(std::move(task));
        }
    });

    Register(MsgId::LeaveRoomReq, [](LogicShard* shard, LogicTask task) {
        if (shard) {
            shard->HandleLeaveRoom(std::move(task));
        }
    });

    Register(MsgId::PlayerReadyReq, [](LogicShard* shard, LogicTask task) {
        if (shard) {
            shard->HandlePlayerReady(std::move(task));
        }
    });

    Register(MsgId::EnterBattleReq, [](LogicShard* shard, LogicTask task) {
        if (shard) {
            shard->HandleEnterBattle(std::move(task));
        }
    });

    Register(MsgId::ClientDisconnectedNtf, [](LogicShard* shard, LogicTask task) {
        if (shard) {
            shard->HandleClientDisconnected(std::move(task));
        }
    });

    auto register_command = [this](MsgId msg_id) {
        Register(msg_id, [](LogicShard* shard, LogicTask task) {
            if (shard) {
                shard->HandlePlayerCommand(std::move(task));
            }
        });
    };

    register_command(MsgId::MoveCmd);
    register_command(MsgId::AttackCmd);
    register_command(MsgId::SkillCmd);
    register_command(MsgId::HarvestCmd);
    register_command(MsgId::StoreResourceCmd);
    register_command(MsgId::PickupResourceCmd);
    register_command(MsgId::BuildCmd);
    register_command(MsgId::ConstructCmd);
    register_command(MsgId::TrainUnitCmd);
    register_command(MsgId::StopCmd);

    initialized_ = true;
}
