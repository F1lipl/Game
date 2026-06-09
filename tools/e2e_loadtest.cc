// End-to-end load generator: behaves as REAL clients against the GateServer.
// Each client = one TCP connection to the Gate (:8888):
//   LoginReq -> CreateRoomReq -> PlayerReadyReq -> stream MoveCmd
// Client sends RAW inner messages (the Gate injects uid + wraps to the
// GameServer) and receives RAW inner messages (the Gate unwraps). This
// exercises the full Gate + Game path. Async: a few io threads drive many
// thousands of client connections.
//
// Usage: e2e_loadtest <host> <port> <clients> <duration_sec> <move_ms> <io_threads>

#include "common/Protocol.h"
#include "rts.pb.h"

#include <boost/asio.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using boost::asio::ip::tcp;
using Clock = std::chrono::steady_clock;
namespace proto = rts::protocol;

namespace {

std::atomic<std::uint64_t> g_connected{0};
std::atomic<std::uint64_t> g_recv{0};
std::atomic<std::uint64_t> g_sent{0};
std::atomic<bool> g_running{true};

std::mutex g_login_mtx;
std::vector<double> g_login_us; // LoginReq->LoginRsp RTT (end-to-end through Gate)

std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count());
}

std::string Frame(std::uint16_t msg_id, const std::string& body) {
    std::string out;
    out.push_back(static_cast<char>((proto::kPacketMagic >> 8) & 0xFF));
    out.push_back(static_cast<char>(proto::kPacketMagic & 0xFF));
    out.push_back(static_cast<char>((msg_id >> 8) & 0xFF));
    out.push_back(static_cast<char>(msg_id & 0xFF));
    out.push_back(0); out.push_back(0); // flags
    std::uint32_t n = static_cast<std::uint32_t>(body.size());
    out.push_back(static_cast<char>((n >> 24) & 0xFF));
    out.push_back(static_cast<char>((n >> 16) & 0xFF));
    out.push_back(static_cast<char>((n >> 8) & 0xFF));
    out.push_back(static_cast<char>(n & 0xFF));
    out += body;
    return out;
}

struct Client : std::enable_shared_from_this<Client> {
    Client(boost::asio::io_context& ioc, std::uint64_t idx)
        : sock(ioc), timer(ioc), index(idx) {}

    tcp::socket sock;
    boost::asio::steady_timer timer;
    std::uint64_t index;
    std::uint64_t room_id{0};
    std::vector<std::uint64_t> units;
    int phase{0}; // 0=login,1=room,2=playing
    std::uint64_t login_send_ns{0};
    std::uint64_t first_tick{0}, last_tick{0};
    Clock::time_point first_t{}, last_t{};

    char hdr[proto::kPacketHeaderLen];
    std::string body;

    std::deque<std::string> outq;
    bool writing{false};
    int move_interval_ms{100};

    void Send(std::uint16_t msg_id, const std::string& inner) {
        outq.push_back(Frame(msg_id, inner));
        if (!writing) DoWrite();
    }
    void DoWrite() {
        writing = true;
        auto self = shared_from_this();
        boost::asio::async_write(sock, boost::asio::buffer(outq.front()),
            [self](const boost::system::error_code& ec, std::size_t) {
                if (ec) return;
                g_sent.fetch_add(1, std::memory_order_relaxed);
                self->outq.pop_front();
                if (!self->outq.empty()) self->DoWrite();
                else self->writing = false;
            });
    }

    void Start() {
        login_send_ns = NowNs();
        rts::v1::LoginReq req;
        req.set_account("p" + std::to_string(index));
        req.set_token("t");
        std::string b; req.SerializeToString(&b);
        Send(static_cast<std::uint16_t>(proto::MsgId::LoginReq), b);
        ReadHeader();
    }

    void ReadHeader() {
        auto self = shared_from_this();
        boost::asio::async_read(sock, boost::asio::buffer(hdr, proto::kPacketHeaderLen),
            [self](const boost::system::error_code& ec, std::size_t) {
                if (ec) return;
                const auto* h = reinterpret_cast<unsigned char*>(self->hdr);
                std::uint16_t msg_id = (h[2] << 8) | h[3];
                std::uint32_t len = (h[6] << 24) | (h[7] << 16) | (h[8] << 8) | h[9];
                self->body.resize(len);
                self->ReadBody(msg_id, len);
            });
    }
    void ReadBody(std::uint16_t msg_id, std::uint32_t len) {
        auto self = shared_from_this();
        auto cont = [self, msg_id](const boost::system::error_code& ec, std::size_t) {
            if (ec) return;
            g_recv.fetch_add(1, std::memory_order_relaxed);
            self->OnMessage(msg_id);
            self->ReadHeader();
        };
        if (len == 0) cont({}, 0);
        else boost::asio::async_read(sock, boost::asio::buffer(self->body.data(), len), cont);
    }

    void OnMessage(std::uint16_t msg_id) {
        if (msg_id == static_cast<std::uint16_t>(proto::MsgId::LoginRsp)) {
            const double us = static_cast<double>(NowNs() - login_send_ns) / 1000.0;
            { std::lock_guard<std::mutex> lk(g_login_mtx); g_login_us.push_back(us); }
            rts::v1::CreateRoomReq req; req.set_max_players(1);
            std::string b; req.SerializeToString(&b);
            Send(static_cast<std::uint16_t>(proto::MsgId::CreateRoomReq), b);
            phase = 1;
        } else if (msg_id == static_cast<std::uint16_t>(proto::MsgId::CreateRoomRsp)) {
            rts::v1::CreateRoomRsp rsp;
            if (!rsp.ParseFromString(body) || rsp.room_id() == 0) return;
            room_id = rsp.room_id();
            rts::v1::PlayerReadyReq req; req.set_room_id(room_id); req.set_ready(true);
            std::string b; req.SerializeToString(&b);
            Send(static_cast<std::uint16_t>(proto::MsgId::PlayerReadyReq), b);
        } else if (msg_id == static_cast<std::uint16_t>(proto::MsgId::SnapshotNtf)) {
            rts::v1::SnapshotNtf snap;
            if (!snap.ParseFromString(body)) return;
            units.clear();
            for (const auto& u : snap.units()) units.push_back(u.id());
            const auto now = Clock::now();
            if (first_tick == 0) { first_tick = snap.server_tick(); first_t = now; }
            last_tick = snap.server_tick(); last_t = now;
            if (phase != 2 && !units.empty()) { phase = 2; ArmMove(); }
        } else if (msg_id == static_cast<std::uint16_t>(proto::MsgId::WorldDeltaNtf)) {
            rts::v1::WorldDeltaNtf d;
            if (!d.ParseFromString(body)) return;
            const auto now = Clock::now();
            if (first_tick == 0) { first_tick = d.server_tick(); first_t = now; }
            last_tick = d.server_tick(); last_t = now;
        }
    }

    void ArmMove() {
        if (!g_running.load(std::memory_order_relaxed)) return;
        timer.expires_after(std::chrono::milliseconds(move_interval_ms));
        auto self = shared_from_this();
        timer.async_wait([self](const boost::system::error_code& ec) {
            if (ec || !g_running.load(std::memory_order_relaxed)) return;
            if (self->phase == 2 && !self->units.empty()) {
                rts::v1::MoveCmd cmd;
                cmd.set_room_id(self->room_id);
                for (auto id : self->units) cmd.add_actor_unit_ids(id);
                auto* t = cmd.mutable_target_position();
                t->set_x(static_cast<float>((self->index + self->last_tick) % 100) - 50.0f);
                t->set_z(0.0f);
                std::string b; cmd.SerializeToString(&b);
                self->Send(static_cast<std::uint16_t>(proto::MsgId::MoveCmd), b);
            }
            self->ArmMove();
        });
    }
};

} // namespace

int main(int argc, char** argv) {
    std::string host = argc > 1 ? argv[1] : "127.0.0.1";
    std::string port = argc > 2 ? argv[2] : "8888";
    std::size_t clients = argc > 3 ? std::stoul(argv[3]) : 500;
    int duration = argc > 4 ? std::stoi(argv[4]) : 10;
    int move_ms = argc > 5 ? std::stoi(argv[5]) : 100;
    int io_threads = argc > 6 ? std::stoi(argv[6]) : 2;

    std::cout << "e2e_loadtest -> Gate " << host << ":" << port
              << " clients=" << clients << " duration=" << duration << "s\n";

    boost::asio::io_context ioc;
    auto guard = boost::asio::make_work_guard(ioc);

    std::vector<std::shared_ptr<Client>> all;
    all.reserve(clients);
    tcp::resolver res(ioc);
    auto eps = res.resolve(host, port);

    for (std::size_t i = 0; i < clients; ++i) {
        auto c = std::make_shared<Client>(ioc, i + 1);
        c->move_interval_ms = move_ms;
        boost::asio::async_connect(c->sock, eps,
            [c](const boost::system::error_code& ec, const tcp::endpoint&) {
                if (ec) return;
                boost::system::error_code ig;
                c->sock.set_option(tcp::no_delay(true), ig);
                g_connected.fetch_add(1, std::memory_order_relaxed);
                c->Start();
            });
        all.push_back(std::move(c));
    }

    std::vector<std::thread> pool;
    for (int i = 0; i < io_threads; ++i) pool.emplace_back([&ioc]() { ioc.run(); });

    const auto t0 = Clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(duration));
    g_running.store(false);
    const double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();

    // effective tick + playing
    double sum_hz = 0.0; std::size_t playing = 0;
    for (auto& c : all) {
        if (c->phase == 2 && c->last_tick > c->first_tick) {
            double secs = std::chrono::duration<double>(c->last_t - c->first_t).count();
            if (secs > 0.5) { sum_hz += static_cast<double>(c->last_tick - c->first_tick) / secs; ++playing; }
        }
    }
    double avg_login = 0.0, p95_login = 0.0; std::size_t ln = 0;
    {
        std::lock_guard<std::mutex> lk(g_login_mtx);
        ln = g_login_us.size();
        if (ln) {
            std::sort(g_login_us.begin(), g_login_us.end());
            double s = 0; for (double v : g_login_us) s += v;
            avg_login = (s / ln) / 1000.0;
            p95_login = g_login_us[static_cast<std::size_t>(0.95 * (ln - 1))] / 1000.0;
        }
    }

    guard.reset();
    ioc.stop();
    for (auto& t : pool) t.join();

    std::cout << "----- results -----\n";
    std::cout << "connected          : " << g_connected.load() << " / " << clients << "\n";
    std::cout << "playing            : " << playing << "\n";
    std::cout << "login RTT avg/p95  : " << avg_login << " / " << p95_login << " ms (end-to-end thru Gate)\n";
    std::cout << "msgs sent/recv     : " << g_sent.load() / elapsed << " / " << g_recv.load() / elapsed << " per s\n";
    std::cout << "effective tick rate: " << (playing ? sum_hz / playing : 0.0) << " Hz\n";
    return 0;
}
