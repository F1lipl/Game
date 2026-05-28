#pragma once
#include"Const.h"
#include "DungeonRoom.h"
#include "GameServer.h"
#include "GameServerTypes.h"
#include <atomic>
#include<boost/asio.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
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
    LogicShard(GameServer*);
    void start();
    void stop();
    void postTask(LogicTask);

    void HandleCreateRoom(LogicTask task);
    void HandleJoinRoom(LogicTask task);
    void HandleLeaveRoom(LogicTask task);
    void HandlePlayerReady(LogicTask task);
    void HandleEnterBattle(LogicTask task);
    void HandlePlayerCommand(LogicTask task);

private:
    void handleTask(LogicTask);
    bool SendToPlayer(MsgId msg_id,
                      Uid uid,
                      const google::protobuf::MessageLite& message,
                      SeqId server_seq = 0);
    void SendCommandRejected(Uid uid,
                             std::uint64_t room_id,
                             SeqId client_seq,
                             rts::v1::ErrorCode code,
                             const std::string& reason);
    void BroadcastRoomState(const DungeonRoom& room);
    void BroadcastGameStart(const DungeonRoom& room);
    void BroadcastCommandFrame(DungeonRoom& room,
                               const rts::v1::GateToGameEnvelope& envelope,
                               MsgId msg_id);

    std::thread thread_;
    boost::asio::io_context ioc_;
    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>work_guard_;
    std::unordered_map<Uid, std::uint64_t> uid_to_room_;
    std::unordered_map<std::uint64_t, DungeonRoom> rooms_;
    std::uint64_t next_room_id_ {1};
    std::atomic<bool>b_stop_;
    GameServer* server_;
};
