#pragma once

#include "Const.h"

#include <cstdint>
#include <memory>
#include <string>

class Csession;
class RecvNode;
class SendNode;
class WorkShard;

namespace gate::protocol {

std::shared_ptr<SendNode> BuildPacket(std::uint16_t msg_id,
                                      const std::string& payload,
                                      std::uint16_t flags = rts::protocol::kPacketFlagNone);

std::shared_ptr<SendNode> BuildGateLinkHello(std::uint32_t gate_id,
                                             std::uint32_t link_index);

std::shared_ptr<SendNode> BuildPingReq(std::uint64_t client_time_ms);

std::shared_ptr<SendNode> BuildGateToGameEnvelope(const Csession& session,
                                                  std::uint16_t inner_msg_id,
                                                  const RecvNode& body);

std::shared_ptr<SendNode> BuildCommandRejected(std::uint64_t room_id,
                                               std::uint64_t client_seq,
                                               int code,
                                               const std::string& reason);

bool ForwardGameToClients(WorkShard& shard, const RecvNode& body);

std::uint64_t MakeLoginUid(const Csession& session,
                           const std::string& account,
                           const std::string& token);

} // namespace gate::protocol
