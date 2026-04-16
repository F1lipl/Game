#pragma once
#include <string>
#include "ClientSession.h"
#include "Const.h"
#include <iostream>
#include <boost/asio.hpp>
using namespace std;
using boost::asio::ip::tcp;
class ClientSession;
class MsgNode
{
public:
	MsgNode(short maxlen) :_total_len(maxlen), _cur_len(0) {
		_data = new char[maxlen + 1];
		_data[maxlen] = '\0';
	}
	~MsgNode() {
		std::cout << "destruct MsgNode" << endl;
		delete[] _data;
	}
	void Clear() {
		::memset(_data, 0, _total_len);
		_cur_len = 0;
	}
	short _cur_len;
	short _total_len;
	char* _data;

};

class RecvNode :public MsgNode {
	// friend class LogicSystem;
	friend class ClientSession;
public:
	RecvNode(short max_len, short msg_id);
private:
	short _msg_id;
};

class SendNode :public MsgNode {
	friend class LogicSystem;
	friend class ClientSession;
public:
	SendNode(const char* msg, short max_len, short msg_id);
private:
	short _msg_id;
};
