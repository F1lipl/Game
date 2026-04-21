#pragma once
#include "WorkShard.h"
#include"Const.h"
#include<boost/asio.hpp>
class ClientSession;
class WorkShard;
class SendNode;
class GameServerConnPool {
public:
    using ConnPtr = std::shared_ptr<ClientSession>;

    GameServerConnPool(WorkShard* shard, boost::asio::io_context& ioc);

    // 约定：初始化阶段在池所属线程调用
    void Init();
    void Stop();

    // fire-and-forget：只保证消息成功提交到某个连接的发送队列
    bool PostMessage(std::shared_ptr<SendNode> node);

    // 如果以后你要做 request-response，可以先拿到一个可用连接
    ConnPtr GetAvailableConn();

private:
    ConnPtr CreateConn();
    bool IsConnAvailable(const ConnPtr& conn) const;
    ConnPtr SelectConnUnsafe();

    boost::asio::awaitable<void> detection();

private:
    WorkShard* shard_;
    boost::asio::io_context& ioc_;

    std::vector<ConnPtr> sessions_;
    std::size_t conn_cnt_;
    std::size_t rr_idx_;

    boost::asio::steady_timer timer_;
    bool initialized_;
};