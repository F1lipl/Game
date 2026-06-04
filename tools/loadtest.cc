// Load generator for the RTS GameServer.
//
// Connects directly to the GameServer gateway port (default 50051) and
// pretends to be a GateServer link. Because the GateToGameEnvelope carries the
// uid, a handful of TCP connections can drive thousands of virtual single-player
// rooms: each virtual player creates a room, readies up (which starts the tick
// and spawns units), then streams MoveCmd so units keep moving and the server
// keeps emitting snapshots/deltas.
//
// Reported metrics:
//   - commands sent / sec (offered load)
//   - server messages + bytes received / sec
//   - effective server tick rate (from server_tick in deltas) -> health signal:
//     stays ~20Hz when healthy, drops when the server falls behind.
//
// Usage: loadtest <host> <port> <connections> <rooms_per_conn> <duration_sec>

#include "common/Protocol.h"
#include "rts.pb.h"

#include <boost/asio.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using boost::asio::ip::tcp;
using Clock = std::chrono::steady_clock;
namespace proto = rts::protocol;

namespace {

std::atomic<std::uint64_t> g_sent{0};
std::atomic<std::uint64_t> g_recv{0};
std::atomic<std::uint64_t> g_recv_bytes{0};
std::atomic<bool> g_running{true};

std::mutex g_lat_mtx;
std::vector<double> g_lat_us; // Ping/Pong round-trip samples (microseconds)

std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count());
}

void PutBE16(std::string& b, std::uint16_t v) {
    b.push_back(static_cast<char>((v >> 8) & 0xFF));
    b.push_back(static_cast<char>(v & 0xFF));
}
void PutBE32(std::string& b, std::uint32_t v) {
    b.push_back(static_cast<char>((v >> 24) & 0xFF));
    b.push_back(static_cast<char>((v >> 16) & 0xFF));
    b.push_back(static_cast<char>((v >> 8) & 0xFF));
    b.push_back(static_cast<char>(v & 0xFF));
}

std::string Frame(std::uint16_t msg_id, const std::string& body) {
    std::string out;
    PutBE16(out, proto::kPacketMagic);
    PutBE16(out, msg_id);
    PutBE16(out, 0); // flags
    PutBE32(out, static_cast<std::uint32_t>(body.size()));
    out += body;
    return out;
}

// Wrap an inner client message in a GateToGameEnvelope and frame it.
std::string Envelope(std::uint64_t uid, std::uint64_t room_id,
                     std::uint16_t inner_msg_id, const std::string& inner) {
    rts::v1::GateToGameEnvelope env;
    env.set_uid(uid);
    env.set_room_id(room_id);
    env.set_inner_msg_id(inner_msg_id);
    env.set_payload(inner);
    std::string body;
    env.SerializeToString(&body);
    return Frame(static_cast<std::uint16_t>(proto::MsgId::GateToGameEnvelope), body);
}

struct RoomState {
    std::uint64_t room_id{0};
    int phase{0}; // 0=created,1=ready-sent,2=playing
    std::vector<std::uint64_t> units;
    std::uint64_t first_tick{0};
    std::uint64_t last_tick{0};
    Clock::time_point first_t{};
    Clock::time_point last_t{};
};

struct Conn {
    explicit Conn(boost::asio::io_context& ioc) : socket(ioc) {}
    tcp::socket socket;
    std::mutex write_mtx;
    std::mutex state_mtx;
    std::map<std::uint64_t, RoomState> rooms; // keyed by uid
    std::vector<std::uint64_t> uids;
};

void SendRaw(Conn& c, const std::string& packet) {
    std::lock_guard<std::mutex> lk(c.write_mtx);
    boost::system::error_code ec;
    boost::asio::write(c.socket, boost::asio::buffer(packet), ec);
    if (!ec) {
        g_sent.fetch_add(1, std::memory_order_relaxed);
    }
}

bool ReadExact(tcp::socket& s, char* buf, std::size_t n) {
    boost::system::error_code ec;
    std::size_t got = boost::asio::read(s, boost::asio::buffer(buf, n), ec);
    return !ec && got == n;
}

void Reader(Conn& c) {
    char head[proto::kPacketHeaderLen];
    std::string body;
    while (g_running.load(std::memory_order_relaxed)) {
        if (!ReadExact(c.socket, head, proto::kPacketHeaderLen)) break;
        const auto* h = reinterpret_cast<unsigned char*>(head);
        std::uint16_t magic = (h[0] << 8) | h[1];
        if (magic != proto::kPacketMagic) break;
        std::uint16_t msg_id = (h[2] << 8) | h[3];
        std::uint32_t len = (h[6] << 24) | (h[7] << 16) | (h[8] << 8) | h[9];
        body.resize(len);
        if (len > 0 && !ReadExact(c.socket, body.data(), len)) break;

        g_recv.fetch_add(1, std::memory_order_relaxed);
        g_recv_bytes.fetch_add(proto::kPacketHeaderLen + len, std::memory_order_relaxed);

        // Ping/Pong round-trip latency probe (raw PongRsp, not enveloped)
        if (msg_id == static_cast<std::uint16_t>(proto::MsgId::PongRsp)) {
            rts::v1::PongRsp pong;
            if (pong.ParseFromArray(body.data(), static_cast<int>(body.size()))) {
                const std::uint64_t token = pong.client_time_ms();
                const std::uint64_t now = NowNs();
                if (now > token) {
                    std::lock_guard<std::mutex> lk(g_lat_mtx);
                    g_lat_us.push_back(static_cast<double>(now - token) / 1000.0);
                }
            }
            continue;
        }

        if (msg_id != static_cast<std::uint16_t>(proto::MsgId::GameToGateEnvelope)) {
            continue;
        }
        rts::v1::GameToGateEnvelope env;
        if (!env.ParseFromArray(body.data(), static_cast<int>(body.size()))) continue;
        if (env.target_uids_size() == 0) continue;
        const std::uint64_t uid = env.target_uids(0);
        const auto inner = static_cast<proto::MsgId>(env.inner_msg_id());

        if (inner == proto::MsgId::CreateRoomRsp) {
            rts::v1::CreateRoomRsp rsp;
            if (!rsp.ParseFromString(env.payload())) continue;
            std::uint64_t room_id = rsp.room_id();
            {
                std::lock_guard<std::mutex> lk(c.state_mtx);
                auto& st = c.rooms[uid];
                if (st.phase != 0 || room_id == 0) continue;
                st.room_id = room_id;
                st.phase = 1;
            }
            rts::v1::PlayerReadyReq req;
            req.set_room_id(room_id);
            req.set_ready(true);
            std::string inner_body;
            req.SerializeToString(&inner_body);
            SendRaw(c, Envelope(uid, room_id,
                static_cast<std::uint16_t>(proto::MsgId::PlayerReadyReq), inner_body));
        } else if (inner == proto::MsgId::SnapshotNtf) {
            rts::v1::SnapshotNtf snap;
            if (!snap.ParseFromString(env.payload())) continue;
            std::lock_guard<std::mutex> lk(c.state_mtx);
            auto& st = c.rooms[uid];
            st.units.clear();
            for (const auto& u : snap.units()) st.units.push_back(u.id());
            if (!st.units.empty()) st.phase = 2;
            const auto now = Clock::now();
            if (st.first_tick == 0) { st.first_tick = snap.server_tick(); st.first_t = now; }
            st.last_tick = snap.server_tick();
            st.last_t = now;
        } else if (inner == proto::MsgId::WorldDeltaNtf) {
            rts::v1::WorldDeltaNtf d;
            if (!d.ParseFromString(env.payload())) continue;
            std::lock_guard<std::mutex> lk(c.state_mtx);
            auto& st = c.rooms[uid];
            const auto now = Clock::now();
            if (st.first_tick == 0) { st.first_tick = d.server_tick(); st.first_t = now; }
            st.last_tick = d.server_tick();
            st.last_t = now;
        }
    }
}

void DriveConn(Conn& c, std::uint64_t base_uid, std::size_t rooms,
               int duration_sec, int move_interval_ms) {
    // hello
    rts::v1::GateLinkHello hello;
    hello.set_gate_id(1);
    std::string hb;
    hello.SerializeToString(&hb);
    SendRaw(c, Frame(static_cast<std::uint16_t>(proto::MsgId::GateLinkHello), hb));

    // create rooms (one virtual player each)
    for (std::size_t i = 0; i < rooms; ++i) {
        const std::uint64_t uid = base_uid + i + 1;
        c.uids.push_back(uid);
        {
            std::lock_guard<std::mutex> lk(c.state_mtx);
            c.rooms[uid] = RoomState{};
        }
        rts::v1::CreateRoomReq req;
        req.set_max_players(1);
        std::string inner;
        req.SerializeToString(&inner);
        SendRaw(c, Envelope(uid, 0,
            static_cast<std::uint16_t>(proto::MsgId::CreateRoomReq), inner));
    }

    std::thread reader([&c]() { Reader(c); });

    std::mt19937 rng(static_cast<unsigned>(base_uid));
    std::uniform_real_distribution<float> pos(-50.0f, 50.0f);

    const auto end = Clock::now() + std::chrono::seconds(duration_sec);
    while (Clock::now() < end) {
        const auto tick_start = Clock::now();
        std::vector<std::pair<std::uint64_t, std::pair<std::uint64_t, std::vector<std::uint64_t>>>> playing;
        {
            std::lock_guard<std::mutex> lk(c.state_mtx);
            for (auto& [uid, st] : c.rooms) {
                if (st.phase == 2 && !st.units.empty()) {
                    playing.push_back({uid, {st.room_id, st.units}});
                }
            }
        }
        for (auto& [uid, rd] : playing) {
            rts::v1::MoveCmd cmd;
            cmd.set_room_id(rd.first);
            for (auto id : rd.second) cmd.add_actor_unit_ids(id);
            auto* t = cmd.mutable_target_position();
            t->set_x(pos(rng));
            t->set_z(pos(rng));
            std::string inner;
            cmd.SerializeToString(&inner);
            SendRaw(c, Envelope(uid, rd.first,
                static_cast<std::uint16_t>(proto::MsgId::MoveCmd), inner));
        }

        // latency probe: raw PingReq carrying a monotonic token, server echoes it
        {
            rts::v1::PingReq ping;
            ping.set_client_time_ms(NowNs());
            std::string pb;
            ping.SerializeToString(&pb);
            SendRaw(c, Frame(static_cast<std::uint16_t>(proto::MsgId::PingReq), pb));
        }

        std::this_thread::sleep_until(tick_start + std::chrono::milliseconds(move_interval_ms));
    }

    g_running.store(false);
    boost::system::error_code ec;
    c.socket.shutdown(tcp::socket::shutdown_both, ec);
    c.socket.close(ec);
    reader.join();
}

double EffectiveTickHz(std::vector<std::unique_ptr<Conn>>& conns, std::size_t& playing_rooms) {
    double sum_hz = 0.0;
    std::size_t n = 0;
    for (auto& c : conns) {
        std::lock_guard<std::mutex> lk(c->state_mtx);
        for (auto& [uid, st] : c->rooms) {
            if (st.phase == 2 && st.last_tick > st.first_tick) {
                double secs = std::chrono::duration<double>(st.last_t - st.first_t).count();
                if (secs > 0.5) {
                    sum_hz += static_cast<double>(st.last_tick - st.first_tick) / secs;
                    ++n;
                }
            }
        }
    }
    playing_rooms = n;
    return n ? sum_hz / static_cast<double>(n) : 0.0;
}

} // namespace

int main(int argc, char** argv) {
    std::string host = argc > 1 ? argv[1] : "127.0.0.1";
    std::string port = argc > 2 ? argv[2] : "50051";
    std::size_t connections = argc > 3 ? std::stoul(argv[3]) : 4;
    std::size_t rooms_per_conn = argc > 4 ? std::stoul(argv[4]) : 250;
    int duration = argc > 5 ? std::stoi(argv[5]) : 10;
    int move_interval_ms = argc > 6 ? std::stoi(argv[6]) : 100;

    std::cout << "loadtest -> " << host << ":" << port
              << " conns=" << connections
              << " rooms/conn=" << rooms_per_conn
              << " (" << connections * rooms_per_conn << " rooms)"
              << " duration=" << duration << "s"
              << " move_interval=" << move_interval_ms << "ms\n";

    boost::asio::io_context ioc;
    std::vector<std::unique_ptr<Conn>> conns;
    for (std::size_t i = 0; i < connections; ++i) {
        auto c = std::make_unique<Conn>(ioc);
        tcp::resolver res(ioc);
        boost::system::error_code ec;
        boost::asio::connect(c->socket, res.resolve(host, port), ec);
        if (ec) {
            std::cerr << "connect failed: " << ec.message() << "\n";
            return 1;
        }
        c->socket.set_option(tcp::no_delay(true));
        conns.push_back(std::move(c));
    }

    const auto t0 = Clock::now();
    std::vector<std::thread> drivers;
    for (std::size_t i = 0; i < conns.size(); ++i) {
        drivers.emplace_back(DriveConn, std::ref(*conns[i]),
                             (i + 1) * 1000000ULL, rooms_per_conn,
                             duration, move_interval_ms);
    }
    for (auto& t : drivers) t.join();
    const double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();

    std::size_t playing = 0;
    const double hz = EffectiveTickHz(conns, playing);
    const auto sent = g_sent.load();
    const auto recv = g_recv.load();
    const auto bytes = g_recv_bytes.load();

    double avg_ms = 0.0;
    double p95_ms = 0.0;
    std::size_t lat_n = 0;
    {
        std::lock_guard<std::mutex> lk(g_lat_mtx);
        lat_n = g_lat_us.size();
        if (lat_n > 0) {
            std::sort(g_lat_us.begin(), g_lat_us.end());
            double sum = 0.0;
            for (double v : g_lat_us) sum += v;
            avg_ms = (sum / static_cast<double>(lat_n)) / 1000.0;
            p95_ms = g_lat_us[static_cast<std::size_t>(0.95 * (lat_n - 1))] / 1000.0;
        }
    }

    std::cout << "----- results -----\n";
    std::cout << "elapsed             : " << elapsed << " s\n";
    std::cout << "playing rooms       : " << playing << " / " << connections * rooms_per_conn << "\n";
    std::cout << "msgs sent           : " << sent << " (" << sent / elapsed << "/s)\n";
    std::cout << "msgs recv           : " << recv << " (" << recv / elapsed << "/s)\n";
    std::cout << "recv throughput     : " << (bytes / elapsed) / (1024.0 * 1024.0) << " MiB/s\n";
    std::cout << "latency samples     : " << lat_n << "\n";
    std::cout << "avg latency         : " << avg_ms << " ms\n";
    std::cout << "p95 latency         : " << p95_ms << " ms\n";
    std::cout << "effective tick rate : " << hz << " Hz  (target 20Hz; lower => server behind)\n";
    return 0;
}
