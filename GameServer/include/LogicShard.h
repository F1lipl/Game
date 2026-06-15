#pragma once
#include"Const.h"
#include "DungeonRoom.h"
#include "GameServer.h"
#include "GameServerTypes.h"
#include <atomic>
#include<boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

namespace google::protobuf {
class MessageLite;
}

namespace rts::v1 {
class GateToGameEnvelope;
enum ErrorCode : int;
}

//管理房间，

class LogicShard{

public:
    LogicShard(GameServer*, ShardId shard_id, std::size_t shard_count);
    void start();
    void stop();
    void postTask(LogicTask);

    void HandleCreateRoom(LogicTask task);
    void HandleJoinRoom(LogicTask task);
    void HandleLeaveRoom(LogicTask task);
    void HandlePlayerReady(LogicTask task);
    void HandleEnterBattle(LogicTask task);
    void HandlePlayerCommand(LogicTask task);
    void HandleClientDisconnected(LogicTask task);

private:
    void handleTask(LogicTask);
    // 把 uid 从其所在房间移除; 房间空了就删除 (掉线/隐式离开共用)
    void LeaveRoomInternal(Uid uid);

    // ---- tick / state-sync ----
    boost::asio::awaitable<void> TickLoop();
    void TickRooms();
    void SpawnInitialUnits(DungeonRoom& room);
    void SendFullSnapshot(DungeonRoom& room);
    void SendDelta(DungeonRoom& room);

    bool SendToPlayer(MsgId msg_id,
                      Uid uid,
                      const google::protobuf::MessageLite& message,
                      SeqId server_seq = 0);
    // 序列化一次, 复用给房间内所有玩家 (一个 NetworkTask 带多个 target_uids)
    void SendToPlayers(MsgId msg_id,
                       const std::vector<Uid>& uids,
                       const google::protobuf::MessageLite& message,
                       SeqId server_seq = 0);
    void SendCommandRejected(Uid uid,
                             std::uint64_t room_id,
                             SeqId client_seq,
                             rts::v1::ErrorCode code,
                             const std::string& reason);
    void BroadcastRoomState(const DungeonRoom& room);
    void BroadcastGameStart(const DungeonRoom& room);
    void BroadcastGameOver(const DungeonRoom& room, std::uint32_t winner_team);

    std::thread thread_;
    boost::asio::io_context ioc_;
    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>work_guard_;
    boost::asio::steady_timer tick_timer_;
    std::chrono::steady_clock::time_point next_tick_deadline_;
    std::unordered_map<Uid, std::uint64_t> uid_to_room_;
    std::unordered_map<Uid, GatewayRoute> uid_route_;
    std::unordered_map<std::uint64_t, DungeonRoom> rooms_;
    std::uint64_t next_room_id_ {1};
    std::atomic<bool>b_stop_;
    GameServer* server_;
    ShardId shard_id_ {};
    std::size_t shard_count_ {1};
};
