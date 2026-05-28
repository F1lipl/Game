#include "../include/Cserver.h"
#include "../include/IniConfig.h"

#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>
#include <spdlog/spdlog.h>

#include <csignal>
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

        const int port_value = ini.Get<int>("GateServer.port", 8888);
        if (port_value <= 0 || port_value > 65535) {
            spdlog::error("invalid GateServer.port {}", port_value);
            return 1;
        }

        boost::asio::io_context ioc;
        Cserver server(ioc, static_cast<unsigned short>(port_value));

        boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
        signals.async_wait([&](const boost::system::error_code& ec, int signal_number) {
            if (ec) {
                return;
            }

            spdlog::info("GateServer received signal {}, stopping", signal_number);
            server.stop();
            ioc.stop();
        });

        server.start();
        spdlog::info("GateServer listening on 0.0.0.0:{}", port_value);
        ioc.run();
        server.stop();
        return 0;
    } catch (const std::exception& e) {
        spdlog::critical("GateServer fatal error: {}", e.what());
        return 1;
    }
}

