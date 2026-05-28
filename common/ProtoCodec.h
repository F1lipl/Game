#pragma once

#include <google/protobuf/message_lite.h>

#include <cstddef>
#include <limits>
#include <string>
#include <string_view>

namespace rts::protocol {

inline bool SerializeProtoToString(const google::protobuf::MessageLite& message,
                                   std::string& out) {
    out.clear();
    return message.SerializeToString(&out);
}

inline bool ParseProtoFromBytes(std::string_view bytes,
                                google::protobuf::MessageLite& message) {
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }

    return message.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()));
}

inline bool ParseProtoFromBytes(const char* data,
                                std::size_t size,
                                google::protobuf::MessageLite& message) {
    if (data == nullptr && size != 0) {
        return false;
    }

    return ParseProtoFromBytes(std::string_view(data, size), message);
}

} // namespace rts::protocol

