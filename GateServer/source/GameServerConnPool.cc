#include "../include/GameServerConnPool.h"

#include "../include/ClientSession.h"
#include "../include/Const.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <spdlog/spdlog.h>

GameServerConnPool::GameServerConnPool(Cserver* server, boost::asio::io_context& ioc)
    : server_(server),
      ioc_(ioc),
      conn_cnt_(GAMESERVER_CONN_CNT),
      rr_idx_(0),
      timer_(ioc),
      initialized_(false) {}

void GameServerConnPool::Init() {
    if (initialized_) {
        return;
    }

    sessions_.resize(conn_cnt_);

    for (std::size_t i = 0; i < conn_cnt_; ++i) {
        auto conn = CreateConn();
        sessions_[i] = conn;

        if (!conn) {
            spdlog::warn("GameServerConnPool init: slot {} create failed", i);
        }
    }

    initialized_ = true;

    boost::asio::co_spawn(
        ioc_.get_executor(),
        detection(),
        boost::asio::detached);
}

void GameServerConnPool::Stop() {
    boost::system::error_code ec;
    timer_.cancel(ec);

    sessions_.clear();
    rr_idx_ = 0;
    initialized_ = false;
}

GameServerConnPool::ConnPtr GameServerConnPool::CreateConn() {
    auto conn = std::make_shared<ClientSession>(ioc_, server_);
    conn->start();

    if (!IsConnAvailable(conn)) {
        return nullptr;
    }

    return conn;
}

bool GameServerConnPool::IsConnAvailable(const ConnPtr& conn) const {
    if (!conn) {
        return false;
    }

    auto state = conn->get_state();
    return state == ClientSession_state::Connected ||
           state == ClientSession_state::Busy;
}

GameServerConnPool::ConnPtr GameServerConnPool::SelectConnUnsafe() {
    if (sessions_.empty()) {
        return nullptr;
    }

    for (std::size_t n = 0; n < conn_cnt_; ++n) {
        std::size_t idx = rr_idx_ % conn_cnt_;
        ++rr_idx_;

        auto& conn = sessions_[idx];
        if (IsConnAvailable(conn)) {
            return conn;
        }
    }

    return nullptr;
}

boost::asio::awaitable<GameServerConnPool::ConnPtr>
GameServerConnPool::GetAvailableConn() {
    co_await boost::asio::dispatch(ioc_.get_executor(), boost::asio::use_awaitable);
    co_return SelectConnUnsafe();
}

boost::asio::awaitable<bool>
GameServerConnPool::PostMessage(std::shared_ptr<SendNode> node) {
    co_await boost::asio::dispatch(ioc_.get_executor(), boost::asio::use_awaitable);

    auto conn = SelectConnUnsafe();
    if (!conn) {
        spdlog::error("GameServerConnPool::PostMessage failed: no available conn");
        co_return false;
    }

    // 这里假设 ClientSession 提供这个接口：
    // boost::asio::awaitable<void> PostSend(std::shared_ptr<SendNode> node);
    co_await conn->PostSend(std::move(node));
    co_return true;
}

boost::asio::awaitable<void> GameServerConnPool::detection() {
    boost::system::error_code ec;

    while (true) {
        timer_.expires_after(LINK_DETECTION_TIME);
        ec.clear();

        co_await timer_.async_wait(
            boost::asio::redirect_error(boost::asio::use_awaitable, ec));

        if (ec) {
            if (ec == boost::asio::error::operation_aborted) {
                co_return;
            }

            spdlog::error("GameServerConnPool detection timer error: {}", ec.message());
            continue;
        }

        for (std::size_t i = 0; i < conn_cnt_; ++i) {
            if (IsConnAvailable(sessions_[i])) {
                continue;
            }

            auto new_conn = CreateConn();
            if (!new_conn) {
                spdlog::warn("GameServerConnPool rebuild slot {} failed", i);
                continue;
            }

            sessions_[i] = new_conn;
            spdlog::info("GameServerConnPool rebuild slot {} success", i);
        }
    }
}
