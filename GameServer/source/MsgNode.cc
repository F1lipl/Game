#include "../include/MsgNode.h"

RecvNode::RecvNode(std::size_t max_len,
                   std::uint16_t msg_id,
                   std::uint16_t flags)
    : MsgNode(max_len),
      _msg_id(msg_id),
      _flags(flags) {
}

SendNode::SendNode(const char* msg,
                   std::uint32_t max_len,
                   std::uint16_t msg_id,
                   std::uint16_t flags)
    : MsgNode(max_len + HEAD_TOTAL_LEN),
      _msg_id(msg_id),
      _flags(flags) {
    std::uint16_t magic_host =
        boost::asio::detail::socket_ops::host_to_network_short(rts::protocol::kPacketMagic);
    std::memcpy(_data + HEAD_MAGIC_OFFSET, &magic_host, HEAD_MAGIC_LEN);

    std::uint16_t msg_id_host =
        boost::asio::detail::socket_ops::host_to_network_short(msg_id);
    std::memcpy(_data + HEAD_ID_OFFSET, &msg_id_host, HEAD_ID_LEN);

    std::uint16_t flags_host =
        boost::asio::detail::socket_ops::host_to_network_short(flags);
    std::memcpy(_data + HEAD_FLAGS_OFFSET, &flags_host, HEAD_FLAGS_LEN);

    std::uint32_t max_len_host =
        boost::asio::detail::socket_ops::host_to_network_long(max_len);
    std::memcpy(_data + HEAD_DATA_OFFSET, &max_len_host, HEAD_DATA_LEN);

    if (max_len > 0 && msg != nullptr) {
        std::memcpy(_data + HEAD_TOTAL_LEN, msg, max_len);
    }
}
