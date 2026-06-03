#pragma once

#include "Const.h"

#include <boost/asio.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

using namespace std;
using boost::asio::ip::tcp;

class MsgNode
{
public:
    explicit MsgNode(std::size_t maxlen) : _cur_len(0), _total_len(maxlen) {
        _data = new char[maxlen + 1];
        _data[maxlen] = '\0';
    }

    ~MsgNode() {
        delete[] _data;
    }

    void Clear() {
        ::memset(_data, 0, _total_len);
        _cur_len = 0;
    }

    std::size_t _cur_len;
    std::size_t _total_len;
    char* _data;
};

class RecvNode : public MsgNode {
    friend class ClientSession;

public:
    RecvNode(std::size_t max_len,
             std::uint16_t msg_id,
             std::uint16_t flags = rts::protocol::kPacketFlagNone);

    std::uint16_t MsgId() const { return _msg_id; }
    std::uint16_t Flags() const { return _flags; }

private:
    std::uint16_t _msg_id;
    std::uint16_t _flags;
};

class SendNode : public MsgNode {
    friend class LogicSystem;
    friend class ClientSession;

public:
    SendNode(const char* msg,
             std::uint32_t max_len,
             std::uint16_t msg_id,
             std::uint16_t flags = rts::protocol::kPacketFlagNone);

    std::uint16_t MsgId() const { return _msg_id; }
    std::uint16_t Flags() const { return _flags; }

private:
    std::uint16_t _msg_id;
    std::uint16_t _flags;
};
