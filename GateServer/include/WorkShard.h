#include"Const.h"
#include <algorithm>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/detail/service_registry.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <climits>
#include <memory>
#include <ranges>
#include <thread>
#include <unordered_map>
#include "Csession.h"
#include "GameServerConnPool.h"
#include "MsgNode.h"

//一线程，一iocontext，一连接池，一用户管理
class WorkShard{
public:
    WorkShard();
    ~WorkShard();

    using work=boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
    boost::asio::io_context& get_io_context();
    void start();
    void stop();
    void add_user_session(std::string,std::shared_ptr<Csession>);
    void PostMessage(std::shared_ptr<RecvNode>);
    void delete_user_session(std::string name);





private:
    boost::asio::io_context ioc_;
    std::unique_ptr<work>worker_;
    std::unique_ptr<GameServerConnPool>ConnPool_;
    std::unordered_map<std::string,std::shared_ptr<Csession>>user_session_mgr;
    thread thread_;  
    Cserver* server;
};