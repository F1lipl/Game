#include "../include/GatewayLinkSession.h"
#include "../include/NetworkShard.h"
#include"../include/MsgNode.h"
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <cstring>

namespace {

std::string create_uuid() {
    auto u = boost::uuids::random_generator()();
    return boost::uuids::to_string(u);
}

} // namespace

GatewayLinkSession::GatewayLinkSession(NetworkShard* owner, boost::asio::io_context& ioc)
    : owner_(owner),
      uuid_(create_uuid()),
      socket_(ioc),
      timer_(ioc),
      state_(Session_state::Invalid),
      buffer_(std::make_unique<char[]>(Buffer_size)),
      is_writing_(false) {}

void GatewayLinkSession::BindSlot(std::size_t slot_id, std::uint64_t generation) {
    slot_id_ = slot_id;
    generation_ = generation;
}

void GatewayLinkSession::Start() {
    Set_state(Session_state::Conected);

    auto self = shared_from_this();

    boost::asio::co_spawn(
        socket_.get_executor(),
        [self]() -> boost::asio::awaitable<void> {
            co_await self->start_heartbeat();
        },
        boost::asio::detached);

    boost::asio::co_spawn(
        socket_.get_executor(),
        [self]() -> boost::asio::awaitable<void> {
            co_await self->handle_read();
        },
        boost::asio::detached);
}

void GatewayLinkSession::Close() {
    if (state_ == Session_state::Closing ||
        state_ == Session_state::Closed) {
        return;
    }

    Set_state(Session_state::Closing);
    spdlog::info("gateway link session {} is closing", uuid_);

    boost::system::error_code ec;

    timer_.cancel(ec);
    if (ec) {
        spdlog::debug("gateway link session {} timer cancel error {}", uuid_, ec.message());
    }

    ec.clear();
    socket_.cancel(ec);
    if (ec) {
        spdlog::debug("gateway link session {} socket cancel error {}", uuid_, ec.message());
    }

    ec.clear();
    socket_.close(ec);
    if (ec) {
        spdlog::debug("gateway link session {} socket close error {}", uuid_, ec.message());
    }

    is_writing_ = false;
    ClearSendQueue();

    Set_state(Session_state::Closed);
    NotifyOwnerClosed();

    spdlog::info("gateway link session {} is closed", uuid_);
}

void GatewayLinkSession::PostSend(std::shared_ptr<SendNode> node) {
    if (!node || is_closing() || state_ == Session_state::Closed) {
        return;
    }

    send_que_.push(std::move(node));

    if (is_writing_) {
        return;
    }

    is_writing_ = true;

    auto self = shared_from_this();

    boost::asio::co_spawn(
        socket_.get_executor(),
        [self]() -> boost::asio::awaitable<void> {
            co_await self->start_write_loop();
        },
        boost::asio::detached);
}

boost::asio::awaitable<void> GatewayLinkSession::handle_read() {
    while (!is_closing() && state_ != Session_state::Closed) {
        auto body_len = co_await ReadHead();
        if (body_len == 0 ||
            is_closing() ||
            state_ == Session_state::Closed) {
            co_return;
        }

        auto ok = co_await ReadData(body_len);
        if (!ok) {
            co_return;
        }

        boost::system::error_code ec;
        timer_.cancel(ec);

        // TODO:
        // 从 Recv_node_ / data_node_ 解析 msg_id 和 seq。
        // 然后交给 NetworkShard。
        //
        // owner_->OnPacket(
        //     static_cast<LinkId>(slot_id_),
        //     msg_id,
        //     seq,
        //     data_node_);
    }
}

boost::asio::awaitable<void> GatewayLinkSession::start_heartbeat() {
    boost::system::error_code ec;

    while (!is_closing() && state_ != Session_state::Closed) {
        timer_.expires_after(HEART_TIMEOUT);
        ec.clear();

        co_await timer_.async_wait(
            boost::asio::redirect_error(boost::asio::use_awaitable, ec));

        if (ec == boost::asio::error::operation_aborted) {
            continue;
        }

        if (ec) {
            spdlog::error("gateway link session {} heartbeat error {}", uuid_, ec.message());
            continue;
        }

        spdlog::warn("gateway link session {} heartbeat timeout", uuid_);
        Close();
        co_return;
    }
}

boost::asio::awaitable<std::size_t> GatewayLinkSession::ReadHead() {
    std::memset(buffer_.get(), 0, Buffer_size);

    auto [ec, length] = co_await boost::asio::async_read(
        socket_,
        boost::asio::buffer(buffer_.get(), HEAD_TOTAL_LEN),
        boost::asio::as_tuple(boost::asio::use_awaitable));

    if (ec) {
        spdlog::error("gateway link session {} read head error {}", uuid_, ec.message());
        Close();
        co_return 0;
    }

    if (length != HEAD_TOTAL_LEN) {
        spdlog::error("gateway link session {} head len error {}", uuid_, length);
        Close();
        co_return 0;
    }

    std::uint16_t data_len = 0;
    std::memcpy(&data_len, buffer_.get() + HEAD_ID_LEN, HEAD_DATA_LEN);
    data_len = boost::asio::detail::socket_ops::network_to_host_short(data_len);

    if (data_len == 0 || data_len > Buffer_size) {
        spdlog::error("gateway link session {} invalid body len {}", uuid_, data_len);
        Close();
        co_return 0;
    }

    co_return static_cast<std::size_t>(data_len);
}

boost::asio::awaitable<bool> GatewayLinkSession::ReadData(std::size_t len) {
    std::memset(buffer_.get(), 0, Buffer_size);

    auto [ec, recv_len] = co_await boost::asio::async_read(
        socket_,
        boost::asio::buffer(buffer_.get(), len),
        boost::asio::as_tuple(boost::asio::use_awaitable));

    if (ec) {
        spdlog::error("gateway link session {} read body error {}", uuid_, ec.message());
        Close();
        co_return false;
    }

    if (recv_len != len) {
        spdlog::error("gateway link session {} body len error expect {} got {}",
                      uuid_, len, recv_len);
        Close();
        co_return false;
    }

    // TODO:
    // data_node_ = std::make_shared<RecvNode>(...);
    // std::memcpy(data_node_->_data, buffer_.get(), len);

    co_return true;
}

boost::asio::awaitable<void> GatewayLinkSession::start_write_loop() {
    while (!send_que_.empty() &&
           state_ != Session_state::Closing &&
           state_ != Session_state::Closed) {
        auto node = send_que_.front();
        send_que_.pop();

        // TODO:
        // 按你的 SendNode 结构替换 node->_data / node->_total_len。
        auto [ec, write_len] = co_await boost::asio::async_write(
            socket_,
            boost::asio::buffer(node->_data, node->_total_len),
            boost::asio::as_tuple(boost::asio::use_awaitable));

        if (ec) {
            spdlog::error("gateway link session {} write error {}", uuid_, ec.message());
            is_writing_ = false;
            Close();
            co_return;
        }
    }

    is_writing_ = false;
    co_return;
}

void GatewayLinkSession::NotifyOwnerClosed() {
    if (!owner_) {
        return;
    }

    if (slot_id_ == std::numeric_limits<std::size_t>::max()) {
        return;
    }

    owner_->OnSessionClosed(slot_id_, generation_);
}

void GatewayLinkSession::ClearSendQueue() {
    std::queue<std::shared_ptr<SendNode>> empty;
    send_que_.swap(empty);
}
