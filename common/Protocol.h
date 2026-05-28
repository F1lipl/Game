#pragma once

#include <cstddef>
#include <cstdint>

namespace rts::protocol {

constexpr std::uint16_t kPacketMagic = 0x5254; // "RT"
constexpr std::size_t kPacketMagicLen = 2;
constexpr std::size_t kPacketMsgIdLen = 2;
constexpr std::size_t kPacketFlagsLen = 2;
constexpr std::size_t kPacketBodyLenLen = 4;
constexpr std::size_t kPacketHeaderLen =
    kPacketMagicLen + kPacketMsgIdLen + kPacketFlagsLen + kPacketBodyLenLen;

constexpr std::size_t kPacketMagicOffset = 0;
constexpr std::size_t kPacketMsgIdOffset = kPacketMagicOffset + kPacketMagicLen;
constexpr std::size_t kPacketFlagsOffset = kPacketMsgIdOffset + kPacketMsgIdLen;
constexpr std::size_t kPacketBodyLenOffset = kPacketFlagsOffset + kPacketFlagsLen;

constexpr std::uint16_t kPacketFlagNone = 0;
constexpr std::uint16_t kPacketFlagCompressed = 1 << 0;
constexpr std::uint16_t kPacketCompressionMask = 0x000E;
constexpr std::uint16_t kPacketCompressionLz4 = 1 << 1;
constexpr std::uint16_t kPacketCompressionZstd = 2 << 1;
constexpr std::uint16_t kPacketCompressionGzip = 3 << 1;

constexpr std::size_t kMaxPacketBodyLen = 64 * 1024;

enum class MsgId : std::uint16_t {
    LoginReq = 10001,
    LoginRsp = 10002,
    PingReq = 10003,
    PongRsp = 10004,

    CreateRoomReq = 10101,
    CreateRoomRsp = 10102,
    JoinRoomReq = 10103,
    JoinRoomRsp = 10104,
    LeaveRoomReq = 10105,
    RoomStateNtf = 10106,
    PlayerReadyReq = 10107,
    GameStartNtf = 10108,

    EnterBattleReq = 11001,
    MoveCmd = 11002,
    AttackCmd = 11003,
    SkillCmd = 11004,
    HarvestCmd = 11005,
    StoreResourceCmd = 11006,
    PickupResourceCmd = 11007,
    BuildCmd = 11008,
    ConstructCmd = 11009,
    TrainUnitCmd = 11010,
    StopCmd = 11011,

    EnterBattleRsp = 12001,
    CommandFrameNtf = 12002,
    CommandRejectedNtf = 12003,
    EntitySpawnNtf = 12004,
    EntityDespawnNtf = 12005,
    WorldDeltaNtf = 12006,
    SnapshotNtf = 12007,
    ResourceDeltaNtf = 12008,
    GameOverNtf = 12009,

    GateToGameEnvelope = 20001,
    GameToGateEnvelope = 20002,
    GateLinkHello = 20003,
    ClientDisconnectedNtf = 20004,

    ErrorNtf = 60001,
};

inline bool IsClientGameplayCommand(MsgId id) {
    return id >= MsgId::EnterBattleReq && id <= MsgId::StopCmd;
}

} // namespace rts::protocol
