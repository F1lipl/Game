#pragma once

#include "Const.h"
#include "GameServerTypes.h"

#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/system/error_code.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <queue>
#include <string>

#include <spdlog/spdlog.h>

class NetworkShard;
class RecvNode;
class SendNode;
class MsgNode;

class GatewayLinkSession : public std::enable_shared_from_this<GatewayLinkSession> {
public:
    GatewayLinkSession(NetworkShard* owner, boost::asio::io_context& ioc);

    void BindSlot(std::size_t slot_id, std::uint64_t generation);

    boost::asio::ip::tcp::socket& get_socket() {
        return socket_;
    }

    std::size_t SlotId() const {
        return slot_id_;
    }

    std::uint64_t Generation() const {
        return generation_;
    }

    const std::string& get_uuid() const {
        return uuid_;
    }

    void Set_state(std::uint8_t state) {
        state_ = state;
    }

    bool is_closing() const {
        return state_ == Session_state::Closing;
    }

    bool is_connected() const {
        return state_ == Session_state::Conected;
    }

    bool IsAvailable() const {
        return state_ == Session_state::Conected;
    }

    void Start();
    void Close();

    void PostSend(std::shared_ptr<SendNode> node);

private:
    boost::asio::awaitable<void> handle_read();
    boost::asio::awaitable<void> start_heartbeat();
    boost::asio::awaitable<std::size_t> ReadHead();
    boost::asio::awaitable<bool> ReadData(std::size_t len);
    boost::asio::awaitable<void> start_write_loop();

    void NotifyOwnerClosed();
    void ClearSendQueue();

private:
    NetworkShard* owner_ {};
    std::size_t slot_id_ {std::numeric_limits<std::size_t>::max()};
    std::uint64_t generation_ {};

    std::string uuid_;
    boost::asio::ip::tcp::socket socket_;
    boost::asio::steady_timer timer_;

    std::uint8_t state_ {Session_state::Invalid};

    std::unique_ptr<char[]> buffer_;
    std::shared_ptr<MsgNode> Recv_node_;
    std::shared_ptr<RecvNode> data_node_;

    std::queue<std::shared_ptr<SendNode>> send_que_;
    bool is_writing_ {false};
};
