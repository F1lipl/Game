#include "../include/Const.h"
#include "../include/GameServer.h"
#include "../include/IniConfig.h"

#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>
#include <spdlog/spdlog.h>

#include <csignal>
#include <cstddef>
#include <exception>
#include <string>

namespace {

bool LoadConfig(IniConfig& ini) {
    std::string err;
    const char* paths[] = {
        "config/config.ini",
        "../config/config.ini",
        "./config.ini",
    };
    for (const auto* path : paths) {
        if (ini.Load(path, &err)) {
            spdlog::info("loaded config from {}", path);
            return true;
        }
    }
    spdlog::warn("load config failed, using defaults. last error: {}", err);
    return false;
}

} // namespace

int main() {
    try {
        IniConfig ini;
        LoadConfig(ini);

        const int port_value = ini.Get<int>("GameServer.port", 50051);
        if (port_value <= 0 || port_value > 65535) {
            spdlog::error("invalid GameServer.port {}", port_value);
            return 1;
        }

        const auto listen_ip = ini.Get<std::string>("GameServer.listen_ip", "0.0.0.0");
        const auto logic_shards = ini.Get<std::size_t>("GameServer.logic_shards", WORK_SHARD_NUMBER);
        const auto network_shards = ini.Get<std::size_t>("GameServer.network_shards", 1);
        const auto default_gateway_links = WORK_SHARD_NUMBER * GAMESERVER_CONN_CNT;
        const auto gateway_links = ini.Get<std::size_t>(
            "GameServer.gateway_link_count", default_gateway_links);

        GameServer server(logic_shards, network_shards, gateway_links);
        server.Start(listen_ip, static_cast<unsigned short>(port_value));
        spdlog::info("GameServer listening on {}:{} (network_shards={})",
                     listen_ip, port_value, network_shards);

        boost::asio::io_context sig_ioc;
        boost::asio::signal_set signals(sig_ioc, SIGINT, SIGTERM);
        signals.async_wait([&](const boost::system::error_code& ec, int signal_number) {
            if (ec) {
                return;
            }
            spdlog::info("GameServer received signal {}, stopping", signal_number);
            server.Stop();
            sig_ioc.stop();
        });

        sig_ioc.run();
        server.Stop();
        return 0;
    } catch (const std::exception& e) {
        spdlog::critical("GameServer fatal error: {}", e.what());
        return 1;
    }
}
