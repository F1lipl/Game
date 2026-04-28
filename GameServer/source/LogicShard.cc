#include"../include/LogicShard.h"
#include"../include/LogicRouter.h"
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <memory>
#include <thread>


LogicShard::LogicShard(GameServer* server)
    : server_(server),
      b_stop_(false) {}

void LogicShard::start() {
    if (thread_.joinable()) {
        return;
    }

    b_stop_ = false;
    ioc_.restart();

    work_guard_ =
        std::make_unique<boost::asio::executor_work_guard<
            boost::asio::io_context::executor_type>>(ioc_.get_executor());

    thread_ = std::thread([this]() {
        ioc_.run();
    });
}

void LogicShard::stop() {
    b_stop_ = true;

    work_guard_.reset();
    ioc_.stop();

    if (thread_.joinable()) {
        thread_.join();
    }
}

void LogicShard::postTask(LogicTask task) {
    if (b_stop_) {
        return;
    }

    boost::asio::post(ioc_.get_executor(),
        [this, task = std::move(task)]() mutable {
            this->handleTask(std::move(task));
        });
}

void LogicShard::handleTask(LogicTask task) {
    MsgId id=task.msg_id;
    LogicRouter::Getinstance()->handle_task(id)(this,std::move(task));
}
