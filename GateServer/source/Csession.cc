#include "../include/Csession.h"
#include <boost/asio.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/impl/write.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/registered_buffer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/detail/error_code.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <cstddef>
#include <cstring>
#include <memory>
#include <spdlog/spdlog.h>
#include <string>
#include <sys/types.h>
#include"../include/MsgNode.h"
#include "../include/Router.h"
std::string create_uuid() {
    boost::uuids::uuid u = boost::uuids::random_generator()();
    return boost::uuids::to_string(u);
}

Csession::Csession(WorkShard* shard, boost::asio::io_context& ioc)
    : shard_(shard),
      socket_(ioc),
      uuid_(create_uuid()),
      timer_(ioc),
      state_(Session_state::Invalid),
      buffer_(new char[Buffer_size]),
      is_writing_(false),
      Recv_node_(std::make_shared<RecvNode>(HEAD_TOTAL_LEN, 0)),
      data_node_(std::make_shared<RecvNode>(Buffer_size, 0)) ,
      uid_(0)
      {}

Csession::~Csession() {
    delete[] buffer_;
}

void Csession::start() {
    Set_state(Session_state::Conected);

    boost::system::error_code nodelay_ec;
    socket_.set_option(boost::asio::ip::tcp::no_delay(true), nodelay_ec);

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

boost::asio::awaitable<void> Csession::handle_read() {
    while (state_ != Session_state::Closing && state_ != Session_state::Closed) {
        std::size_t len = co_await ReadHead();
        if (state_ == Session_state::Closing || state_ == Session_state::Closed) {
            co_return;
        }

        bool ok = co_await ReadData(len);
        if (!ok) {
            co_return;
        }

        // 收到完整包后，重置心跳超时
        boost::system::error_code ec;
        timer_.cancel(ec);

        // TODO: 送进逻辑层
        // 例如:
        // shard_->GetRouter().HandleMsg(shared_from_this(), Recv_node_, data_node_);
        ClientIngressRouter::MsgContext ctx;
        ctx.shard = shard_;
        ctx.session = shared_from_this();
        ctx.body = data_node_;
        ClientIngressRouter::HandleMsg(Recv_node_->MsgId(), ctx);
    }
}

boost::asio::awaitable<void> Csession::start_heartbeat() {
    boost::system::error_code ec;

    while (state_ != Session_state::Closing && state_ != Session_state::Closed) {
        timer_.expires_after(HEART_TIMEOUT);
        ec.clear();

        co_await timer_.async_wait(
            boost::asio::redirect_error(boost::asio::use_awaitable, ec));

        if (ec == boost::asio::error::operation_aborted) {
            // 收到包后会 cancel timer，用来重置超时，继续下一轮等待
            continue;
        }

        if (ec) {
            spdlog::error("session {} heartbeat timer error {}", uuid_, ec.message());
            continue;
        }

        // 真正超时
        spdlog::warn("session {} heartbeat timeout", uuid_);
        close();
        co_return;
    }
}

boost::asio::awaitable<std::size_t> Csession::ReadHead() {
    auto [ec, length] = co_await boost::asio::async_read(
        socket_,
        boost::asio::buffer(buffer_, HEAD_TOTAL_LEN),
        boost::asio::as_tuple(boost::asio::use_awaitable));

    if (ec) {
        spdlog::error("session {} read head error {}", uuid_, ec.message());
        close();
        co_return 0;
    }

    if (length != HEAD_TOTAL_LEN) {
        spdlog::error("session {} recv head len error, got {}", uuid_, length);
        close();
        co_return 0;
    }
    std::uint16_t magic = 0;
    std::memcpy(&magic, buffer_ + HEAD_MAGIC_OFFSET, HEAD_MAGIC_LEN);
    magic = boost::asio::detail::socket_ops::network_to_host_short(magic);
    if (magic != rts::protocol::kPacketMagic) {
        spdlog::error("session {} invalid packet magic {}", uuid_, magic);
        close();
        co_return 0;
    }

    std::uint16_t msg_id = 0;
    std::memcpy(&msg_id, buffer_ + HEAD_ID_OFFSET, HEAD_ID_LEN);
    msg_id = boost::asio::detail::socket_ops::network_to_host_short(msg_id);

    std::uint16_t flags = 0;
    std::memcpy(&flags, buffer_ + HEAD_FLAGS_OFFSET, HEAD_FLAGS_LEN);
    flags = boost::asio::detail::socket_ops::network_to_host_short(flags);

    std::uint32_t data_len = 0;
    std::memcpy(&data_len, buffer_ + HEAD_DATA_OFFSET, HEAD_DATA_LEN);
    data_len = boost::asio::detail::socket_ops::network_to_host_long(data_len);

    // 这里你原来的 msg_id != HEAD_ID_LEN 明显写错了
    // 如果你后面有消息枚举范围，这里再换成更严格的校验
    if (msg_id <= 0) {
        spdlog::error("session {} invalid msg id {}", uuid_, msg_id);
        close();
        co_return 0;
    }

    if (data_len > Buffer_size) {
        spdlog::error("session {} invalid data len {}", uuid_, data_len);
        close();
        co_return 0;
    }

    Recv_node_ = std::make_shared<RecvNode>(HEAD_TOTAL_LEN, msg_id, flags);
    std::memcpy(Recv_node_->_data, buffer_, HEAD_TOTAL_LEN);

    spdlog::info("session {} recv package, msg id {}, flags {}, data len {}",
                 uuid_, msg_id, flags, data_len);

    co_return static_cast<std::size_t>(data_len);
}

boost::asio::awaitable<bool> Csession::ReadData(std::size_t len) {
    data_node_ = std::make_shared<RecvNode>(
        len,
        Recv_node_ ? Recv_node_->MsgId() : 0,
        Recv_node_ ? Recv_node_->Flags() : rts::protocol::kPacketFlagNone);

    if (len == 0) {
        co_return true;
    }

    auto [ec, recv_data_len] = co_await boost::asio::async_read(
        socket_,
        boost::asio::buffer(buffer_, len),
        boost::asio::as_tuple(boost::asio::use_awaitable));

    if (ec) {
        spdlog::error("session {} read data error {}", uuid_, ec.message());
        close();
        co_return false;
    }

    if (recv_data_len != len) {
        spdlog::error("session {} recv data len error, expect {}, got {}",
                      uuid_, len, recv_data_len);
        close();
        co_return false;
    }
    std::memcpy(data_node_->_data, buffer_, len);

    co_return true;
}

void Csession::SendData(std::shared_ptr<SendNode> node){
    if (!node ||
        state_ == Session_state::Closing ||
        state_ == Session_state::Closed) {
        return;
    }

    if (send_que_.size() >= rts::protocol::kMaxSendQueueDepth) {
        send_que_.pop();
        if ((++send_dropped_ & 0xFF) == 1) {
            spdlog::warn("session {} send queue full, dropping oldest (dropped {})",
                         uuid_, send_dropped_);
        }
    }
    send_que_.push(std::move(node));
    if(is_writing_)return;
    is_writing_ = true;

    auto self=shared_from_this();
    boost::asio::co_spawn(
        socket_.get_executor(),
        [self]() -> boost::asio::awaitable<void> {
            co_await self->start_write_loop();
        },
        boost::asio::detached);
    return;
}

boost::asio::awaitable<void>Csession::start_write_loop(){
    while(state_!=Session_state::Closed&&state_!=Session_state::Closing&&!send_que_.empty()){
        auto node=std::move(send_que_.front());
        send_que_.pop();
        auto [ec,len]= co_await boost::asio::async_write(socket_,boost::asio::buffer(node->_data,node->_total_len),boost::asio::as_tuple(boost::asio::use_awaitable));
        if(ec){
            is_writing_=false;
            spdlog::error("session {} write error {}",uuid_,ec.message());
            close();
            co_return;
        }

    }
    is_writing_=false;
    co_return;
}

