#pragma once

// 轻量进程内指标注册表 (GameServer)。
// 计数器用原子, tick 时延用一个加锁的环形采样缓冲, 抓取时算 p50/p99。
// 通过 /metrics 端点以 Prometheus 文本格式暴露。

#include <atomic>
#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace metrics {

inline std::atomic<std::uint64_t> commands_total{0};   // 入站命令处理数
inline std::atomic<std::uint64_t> snapshots_total{0};  // 全量快照下发数
inline std::atomic<std::uint64_t> deltas_total{0};     // 增量下发数
inline std::atomic<std::uint64_t> send_drops_total{0}; // 背压丢包数

inline constexpr std::size_t kMaxShards = 64;
inline std::array<std::atomic<std::uint64_t>, kMaxShards> rooms_by_shard{};
inline std::array<std::atomic<std::uint64_t>, kMaxShards> units_by_shard{};

// ---- tick 时延采样 (微秒) ----
inline std::mutex tick_mtx;
inline std::vector<std::uint32_t> tick_samples;
inline std::size_t tick_pos = 0;
inline constexpr std::size_t kTickWindow = 4096;

inline void RecordTickUs(std::uint32_t us) {
    std::lock_guard<std::mutex> lk(tick_mtx);
    if (tick_samples.size() < kTickWindow) {
        tick_samples.push_back(us);
    } else {
        tick_samples[tick_pos] = us;
        tick_pos = (tick_pos + 1) % kTickWindow;
    }
}

inline void SetShardGauges(std::size_t shard, std::uint64_t rooms, std::uint64_t units) {
    if (shard >= kMaxShards) {
        return;
    }
    rooms_by_shard[shard].store(rooms, std::memory_order_relaxed);
    units_by_shard[shard].store(units, std::memory_order_relaxed);
}

inline std::string Render() {
    std::uint64_t rooms = 0;
    std::uint64_t units = 0;
    for (std::size_t i = 0; i < kMaxShards; ++i) {
        rooms += rooms_by_shard[i].load(std::memory_order_relaxed);
        units += units_by_shard[i].load(std::memory_order_relaxed);
    }

    std::uint32_t p50 = 0, p99 = 0, pmax = 0;
    {
        std::lock_guard<std::mutex> lk(tick_mtx);
        if (!tick_samples.empty()) {
            std::vector<std::uint32_t> s(tick_samples);
            std::sort(s.begin(), s.end());
            p50 = s[s.size() * 50 / 100];
            p99 = s[std::min(s.size() - 1, s.size() * 99 / 100)];
            pmax = s.back();
        }
    }

    std::string out;
    auto line = [&out](const char* name, const char* help, std::uint64_t v) {
        out += "# HELP ";
        out += name;
        out += ' ';
        out += help;
        out += "\n# TYPE ";
        out += name;
        out += " counter\n";
        out += name;
        out += ' ';
        out += std::to_string(v);
        out += '\n';
    };

    line("gameserver_commands_total", "inbound commands processed", commands_total.load());
    line("gameserver_snapshots_total", "full snapshots sent", snapshots_total.load());
    line("gameserver_deltas_total", "world deltas sent", deltas_total.load());
    line("gameserver_send_drops_total", "packets dropped by send backpressure", send_drops_total.load());

    out += "# TYPE gameserver_rooms gauge\ngameserver_rooms " + std::to_string(rooms) + "\n";
    out += "# TYPE gameserver_units gauge\ngameserver_units " + std::to_string(units) + "\n";
    out += "# TYPE gameserver_tick_duration_us gauge\n";
    out += "gameserver_tick_duration_us{quantile=\"0.5\"} " + std::to_string(p50) + "\n";
    out += "gameserver_tick_duration_us{quantile=\"0.99\"} " + std::to_string(p99) + "\n";
    out += "gameserver_tick_duration_us{quantile=\"1.0\"} " + std::to_string(pmax) + "\n";
    return out;
}

} // namespace metrics
