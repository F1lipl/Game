#include"../include/ClientSession.h"
#include<boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/detail/error_code.hpp>
#include <chrono>
#include <spdlog/spdlog.h>
#include "../../common/ProtoCodec.h"
#include"../include/GateProtocol.h"
#include"../include/MsgNode.h"
#include "../include/Router.h"
#include "rts.pb.h"

#include <string_view>

namespace {

constexpr std::chrono::seconds kBackendHeartbeatInterval{5};
constexpr std::chrono::seconds kBackendHeartbeatTimeout{15};

std::uint64_t NowUnixMs() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

std::string_view BodyView(const RecvNode& body) {
    if (body._data == nullptr || body._total_len == 0) {
        return {};
    }

    return std::string_view(body._data, body._total_len);
}

} // namespace

ClientSession::ClientSession(boost::asio::io_context &ioc,
                             WorkShard* shard,
                             std::uint32_t link_index):
socket_(ioc),
buffer_(new char[Buffer_size]),
state_(ClientSession_state::Connecting),
shard_(shard),
timer_(ioc),
is_writing_(false),
last_pong_time_(std::chrono::steady_clock::now()),
link_index_(link_index),
head_(std::make_shared<RecvNode>(HEAD_TOTAL_LEN, 0)),
body_(std::make_shared<RecvNode>(Buffer_size, 0)) {}

ClientSession::~ClientSession() {
    delete[] buffer_;
}


void ClientSession::start(){
    auto self = shared_from_this();
    boost::asio::co_spawn(
        socket_.get_executor(),
        [self]() -> boost::asio::awaitable<void> {
            co_await self->handleconnect();
        },
        boost::asio::detached);
}

boost::asio::awaitable<void> ClientSession::handleconnect() {
    IniConfig ini;
    std::string err;

    if (!ini.Load("config/config.ini", &err) &&
        !ini.Load("../config/config.ini", &err) &&
        !ini.Load("/home/cmr/workspace/project/Gamer/config/config.ini", &err)) {
        spdlog::error("client session load config.ini err {}", err);
        state_ = ClientSession_state::Error;
        close();
        co_return;
    }

    try {
        int port_value = ini.Require<int>("GameServer.port");
        gate_id_ = ini.Get<std::uint32_t>("GateServer.gate_id", 1);
        std::string ip = ini.Get<std::string>(
            "GameServer.ip",
            ini.Get<std::string>("GameServer.host", "127.0.0.1"));

        if (port_value < 0 || port_value > 65535) {
            spdlog::error("invalid port: {}", port_value);
            state_ = ClientSession_state::Error;
            close();
            co_return;
        }

        auto addr = boost::asio::ip::make_address(ip);
        boost::asio::ip::tcp::endpoint ep(
            addr,
            static_cast<unsigned short>(port_value)
        );

        auto self = shared_from_this();
        timer_.expires_after(std::chrono::seconds(5));
        timer_.async_wait([self, ip, port_value](const boost::system::error_code& ec) {
            if (ec || self->state_ != ClientSession_state::Connecting) {
                return;
            }

            spdlog::warn("connect to {}:{} timeout", ip, port_value);
            self->close();
        });

        boost::system::error_code connect_ec;
        co_await socket_.async_connect(
            ep,
            boost::asio::redirect_error(boost::asio::use_awaitable, connect_ec));

        boost::system::error_code cancel_ec;
        timer_.cancel(cancel_ec);

        if (connect_ec) {
            if (state_ != ClientSession_state::closing &&
                state_ != ClientSession_state::closed) {
                spdlog::warn("connect to {}:{} failed: {}", ip, port_value, connect_ec.message());
                state_ = ClientSession_state::Error;
                close();
            }
            co_return;
        }

        if (state_ == ClientSession_state::closing ||
            state_ == ClientSession_state::closed) {
            co_return;
        }

        state_ = ClientSession_state::Connected;
        last_recv_time_ = std::chrono::steady_clock::now();
        last_pong_time_ = last_recv_time_;
        SendGateLinkHello();
        SendPing();

        boost::asio::co_spawn(
            socket_.get_executor(),
            [self]() -> boost::asio::awaitable<void> {
                co_await self->HandleRead();
            },
            boost::asio::detached);
        boost::asio::co_spawn(
            socket_.get_executor(),
            [self]() -> boost::asio::awaitable<void> {
                co_await self->keep_alive();
            },
            boost::asio::detached);
        spdlog::debug("connect to {}:{} success", ip, port_value);
    } catch (const std::exception& e) {
        spdlog::error("handleconnect exception: {}", e.what());
        if (state_ != ClientSession_state::closing &&
            state_ != ClientSession_state::closed) {
            state_ = ClientSession_state::Error;
            close();
        }
    }
}

void ClientSession::SendGateLinkHello() {
    auto packet = gate::protocol::BuildGateLinkHello(gate_id_, link_index_);
    if (!packet) {
        spdlog::warn("build GateLinkHello failed, gate_id={}, link_index={}",
                     gate_id_, link_index_);
        return;
    }

    SendData(std::move(packet));
}

void ClientSession::SendPing() {
    auto packet = gate::protocol::BuildPingReq(NowUnixMs());
    if (!packet) {
        spdlog::warn("build backend PingReq failed");
        return;
    }

    SendData(std::move(packet));
}

void ClientSession::MarkPongReceived() {
    last_pong_time_ = std::chrono::steady_clock::now();
    last_recv_time_ = last_pong_time_;
}


//每十五秒苏醒一次，向服务器发送一次ping包
// boost::asio::awaitable<void> ClientSession::keep_alive(){
//     uint8_t state=co_await Get_state();
//     if(state!=ClientSession_state::Connected)co_return;
//     // sendfile(ping)
//     //发送一个ping包，十五秒以后苏醒，检测一下是否接收到数据包了，如果接受数据包的时间点，之间超过十五秒直接关闭连接
//     timer_.expires_after(KEEP_ALIVE_TIME);

//     co_await timer_.async_wait(boost::asio::use_awaitable);
//     auto time_stamp=co_await get_last_recv_time();
//     auto now=std::chrono::steady_clock::now();
//     if(now-time_stamp<=std::chrono::seconds(15)){
//         co_await keep_alive();
//     }
//     else{
//         co_await Set_State(ClientSession_state::Timeout);
//         co_return;
//     }

    
// // }递归可能导致栈溢出
// boost::asio::awaitable<void> ClientSession::keep_alive(){
//     while(true){
//        co_await boost::asio::dispatch(strand_,boost::asio::use_awaitable);
//        if(state_!=ClientSession_state::Connected&&state_!=ClientSession_state::Busy)co_return;

//        timer_.expires_after(KEEP_ALIVE_TIME);
//         // sendfile
//        boost::system::error_code ec; 
//        co_await timer_.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
//        if(ec){
//             if(ec==boost::asio::error::operation_aborted){
//                 spdlog::info("timer canceled, keep-alive exit");
//                 // co_await close();
//                 co_return; 
//             }  
//             else{
//                 co_await Set_State(ClientSession_state::Error);
//                 co_await close();
//                 co_return;
//             }
//        }
//        if(state_==ClientSession_state::Busy){
//             timer_.expires_after(std::chrono::seconds(5));
//             co_await timer_.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable,ec));
//             if(ec){
//                 if(ec==boost::asio::error::operation_aborted)co_return;
//                 co_await Set_State(ClientSession_state::Error);
//                 co_await close();
//                 co_return;
//             }
//             continue;
//        }
//        if(state_==ClientSession_state::Connected){
//             auto now=std::chrono::steady_clock::now();
//             if(now-last_recv_time_>std::chrono::seconds(15)){
//                 co_await Set_State(ClientSession_state::Timeout);
//                 co_await close();
//                 co_return;
//         }    
//        }
      
//     }
// }

boost::asio::awaitable<void> ClientSession::keep_alive() {
    boost::system::error_code ec;
    while (true) {
        co_await boost::asio::dispatch(socket_.get_executor(), boost::asio::use_awaitable);

        if (state_ != ClientSession_state::Connected &&
            state_ != ClientSession_state::Busy) {
            co_return;
        }

        timer_.expires_after(kBackendHeartbeatInterval);
        ec.clear();
        co_await timer_.async_wait(
            boost::asio::redirect_error(boost::asio::use_awaitable, ec));

        if (ec == boost::asio::error::operation_aborted) {
            co_return;
        }
        if (ec) {
            state_=ClientSession_state::Error;
            close();
            co_return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_pong_time_ > kBackendHeartbeatTimeout) {
            spdlog::warn("backend heartbeat timeout, gate_id={}, link_index={}",
                         gate_id_, link_index_);
            state_=ClientSession_state::Timeout;
            close();
            co_return;
        }

        SendPing();
    }
}


std::chrono::steady_clock::time_point  ClientSession::get_last_recv_time(){
    return last_recv_time_;
}
void ClientSession::set_time_stamp(std::chrono::steady_clock::time_point time){
    last_recv_time_=time;
    return;
}



boost::asio::awaitable<void> ClientSession::HandleRead(){
    while(state_!=ClientSession_state::closed&&state_!=ClientSession_state::closing){
        size_t len=co_await Readhead();
         if (state_ == ClientSession_state::closing || state_ == ClientSession_state::closed) {
            co_return;
        }

        bool ok = co_await ReadData(len);
        if (!ok) {
            co_return;
        }
        last_recv_time_=std::chrono::steady_clock::now();

        if (head_ &&
            head_->MsgId() == static_cast<std::uint16_t>(rts::protocol::MsgId::PongRsp)) {
            rts::v1::PongRsp pong;
            if (!rts::protocol::ParseProtoFromBytes(BodyView(*body_), pong)) {
                spdlog::warn("parse backend PongRsp failed");
                continue;
            }

            MarkPongReceived();
            spdlog::debug("backend pong received, gate_id={}, link_index={}, client_time_ms={}",
                          gate_id_, link_index_, pong.client_time_ms());
            continue;
        }

        BackendIngressRouter::BackendMsgContext ctx;
        ctx.shard = shard_;
        ctx.backend_session = shared_from_this();
        ctx.body = body_;
        BackendIngressRouter::HandleMsg(head_->MsgId(), ctx);
    }


    //to do 送进逻辑层，找对应的session回包

}

boost::asio::awaitable<size_t>ClientSession::Readhead(){
   std::memset(buffer_, 0, Buffer_size);
    auto [ec, length] = co_await boost::asio::async_read(
        socket_,
        boost::asio::buffer(buffer_, HEAD_TOTAL_LEN),
        boost::asio::as_tuple(boost::asio::use_awaitable));

    if (ec) {
        spdlog::error("session read head error {}" , ec.message());
        close();
        co_return 0;
    }

    if (length != HEAD_TOTAL_LEN) {
        spdlog::error("session recv head len error, got {}", length);
        close();
        co_return 0;
    }
    std::uint16_t magic = 0;
    std::memcpy(&magic, buffer_ + HEAD_MAGIC_OFFSET, HEAD_MAGIC_LEN);
    magic = boost::asio::detail::socket_ops::network_to_host_short(magic);
    if (magic != rts::protocol::kPacketMagic) {
        spdlog::error("session invalid packet magic {}", magic);
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
        spdlog::error("session invalid msg id {}", msg_id);
        close();
        co_return 0;
    }

    if (data_len > Buffer_size) {
        spdlog::error("session invalid data len {}", data_len);
        close();
        co_return 0;
    }

    head_ = std::make_shared<RecvNode>(HEAD_TOTAL_LEN, msg_id, flags);
    std::memcpy(head_->_data, buffer_, HEAD_TOTAL_LEN);

    spdlog::debug("backend session recv package, msg id {}, flags {}, data len {}",
                  msg_id, flags, data_len);

    co_return static_cast<std::size_t>(data_len);
}

boost ::asio::awaitable<bool> ClientSession::ReadData(size_t len){
    body_ = std::make_shared<RecvNode>(
        len,
        head_ ? head_->MsgId() : 0,
        head_ ? head_->Flags() : rts::protocol::kPacketFlagNone);

    if (len == 0) {
        co_return true;
    }

    std::memset(buffer_, 0, Buffer_size);
    auto [ec, recv_data_len] = co_await boost::asio::async_read(
        socket_,
        boost::asio::buffer(buffer_, len),
        boost::asio::as_tuple(boost::asio::use_awaitable));

    if (ec) {
        spdlog::error("session read data error {}",ec.message());
        close();
        co_return false;
    }

    if (recv_data_len != len) {
        spdlog::error("session recv data len error, expect {}, got {}", len, recv_data_len);
        close();
        co_return false;
    }
    std::memcpy(body_->_data, buffer_, len);
    co_return true;
}
void ClientSession::close(){
    if(state_==ClientSession_state::closed||state_==ClientSession_state::closing)return;
    boost::system::error_code ec;
    state_=ClientSession_state::closing;
    spdlog::info("clientsession is closing");
    timer_.cancel(ec);
    if(ec){
        spdlog::error("clientsession timer cancel error,error is {}",ec.message());
    }
    ec.clear();
    socket_.cancel(ec);
    if(ec){
        spdlog::error("clientsession cancel error error is {}",ec.message());
    }
    ec.clear();
    socket_.close(ec);
    if (ec) {
        spdlog::error("ClientSession close socket failed: {}", ec.message());
    }
    state_ = ClientSession_state::closed;
    spdlog::info("ClientSession closed successfully");
    return;
}


// 需要同步的机制，最好放在同一个函数里这样能保证串行执行
void ClientSession::SendData(std::shared_ptr<SendNode> node) {
    auto ex = socket_.get_executor();
    // co_await boost::asio::dispatch(ex, boost::asio::use_awaitable);

    send_que_.push(std::move(node));

    if (is_writing_) {
        return;
    }

    is_writing_ = true;

    auto self = shared_from_this();
    boost::asio::co_spawn(
        ex,
        [self]() -> boost::asio::awaitable<void> {
            co_await self->start_write_loop();
        },
        boost::asio::detached);

    return;;
}

boost::asio::awaitable<void> ClientSession::start_write_loop() {
    while (!send_que_.empty() &&
           (state_ == ClientSession_state::Busy ||
            state_ == ClientSession_state::Connected)) {
        state_ = ClientSession_state::Busy;

        auto node = send_que_.front();
        send_que_.pop();

        auto [ec, write_len] =
            co_await boost::asio::async_write(
                socket_,
                boost::asio::buffer(node->_data, node->_total_len),
                boost::asio::as_tuple(boost::asio::use_awaitable));

        if (ec) {
            is_writing_ = false;
            state_ = ClientSession_state::Error;
            spdlog::error("write data is error {}", ec.message());
            close();
            co_return;
        }
    }

    is_writing_ = false;

    if (state_ == ClientSession_state::Busy) {
        state_ = ClientSession_state::Connected;
    }

    co_return;
}
