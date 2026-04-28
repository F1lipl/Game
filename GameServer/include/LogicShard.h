#include"Const.h"
#include "GameServer.h"
#include "GameServerTypes.h"
#include <atomic>
#include<boost/asio.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <cstdint>
#include <memory>
#include <unordered_map>

//管理房间，

class DungeonRoom;
class LogicShard{

public:
    LogicShard(GameServer*);
    void start();
    void stop();
    void postTask(LogicTask);
private:
    void handleTask(LogicTask);
    std::thread thread_;
    boost::asio::io_context ioc_;
    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>work_guard_;
    std::unordered_map<uint64_t,std::string>uid_to_room_;
    std::unordered_map<std::string,DungeonRoom> Rooms_;
    std::atomic<bool>b_stop_;
    GameServer* server_;
};