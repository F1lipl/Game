#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct RoomPlayerState {
    std::uint64_t uid {};
    std::uint32_t team {};
    bool ready {};
};

class DungeonRoom {
public:
    DungeonRoom() = default;

    DungeonRoom(std::uint64_t room_id,
                std::string room_name,
                std::uint32_t max_players)
        : room_id_(room_id),
          room_name_(std::move(room_name)),
          max_players_(max_players == 0 ? 2 : max_players) {}

    std::uint64_t RoomId() const {
        return room_id_;
    }

    const std::string& RoomName() const {
        return room_name_;
    }

    std::uint32_t MaxPlayers() const {
        return max_players_;
    }

    bool HasPlayer(std::uint64_t uid) const {
        return players_.find(uid) != players_.end();
    }

    bool IsFull() const {
        return players_.size() >= max_players_;
    }

    bool AddPlayer(std::uint64_t uid, std::uint32_t* assigned_team = nullptr) {
        auto it = players_.find(uid);
        if (it != players_.end()) {
            if (assigned_team) {
                *assigned_team = it->second.team;
            }
            return true;
        }

        if (IsFull()) {
            return false;
        }

        RoomPlayerState state;
        state.uid = uid;
        state.team = static_cast<std::uint32_t>(players_.size());
        state.ready = false;
        players_.emplace(uid, state);

        if (assigned_team) {
            *assigned_team = state.team;
        }

        return true;
    }

    bool SetReady(std::uint64_t uid, bool ready) {
        auto it = players_.find(uid);
        if (it == players_.end()) {
            return false;
        }

        it->second.ready = ready;
        return true;
    }

    bool RemovePlayer(std::uint64_t uid) {
        return players_.erase(uid) > 0;
    }

    bool Empty() const {
        return players_.empty();
    }

    bool AllReady() const {
        if (players_.empty()) {
            return false;
        }

        for (const auto& [uid, player] : players_) {
            if (!player.ready) {
                return false;
            }
        }

        return true;
    }

    bool Started() const {
        return started_;
    }

    void SetStarted(bool started) {
        started_ = started;
    }

    std::uint64_t ServerTick() const {
        return server_tick_;
    }

    std::uint64_t NextServerTick() {
        return ++server_tick_;
    }

    std::uint64_t NextCommandId() {
        return ++next_command_id_;
    }

    std::vector<RoomPlayerState> Players() const {
        std::vector<RoomPlayerState> result;
        result.reserve(players_.size());
        for (const auto& [uid, player] : players_) {
            result.push_back(player);
        }
        return result;
    }

    std::vector<std::uint64_t> PlayerUids() const {
        std::vector<std::uint64_t> result;
        result.reserve(players_.size());
        for (const auto& [uid, player] : players_) {
            result.push_back(uid);
        }
        return result;
    }

private:
    std::uint64_t room_id_ {};
    std::string room_name_;
    std::uint32_t max_players_ {2};
    bool started_ {false};
    std::uint64_t server_tick_ {};
    std::uint64_t next_command_id_ {};
    std::unordered_map<std::uint64_t, RoomPlayerState> players_;
};
