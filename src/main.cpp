#include "compat/net.hpp"
#include "server/pxe_server.hpp"
#include "utils/config_loader.hpp"
#include "utils/logger.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_running{true};

void handleSignal(int) {
    g_running.store(false);
}

std::string defaultConfigPath() {
    // Distribution layout keeps config/ next to the executable
    // (build/bin/config/pxe.cfg); fall back to the source tree layout.
    const char* candidates[] = {"config/pxe.cfg", "../config/pxe.cfg"};
    for (const char* candidate : candidates) {
        if (std::ifstream(candidate).good()) return candidate;
    }
    return "config/pxe.cfg";
}

pxe::LogLevel parseLevel(const std::string& value) {
    if (value == "debug") return pxe::LogLevel::DEBUG;
    if (value == "warning" || value == "warn") return pxe::LogLevel::WARNING;
    if (value == "error") return pxe::LogLevel::ERR;
    return pxe::LogLevel::INFO;
}

} // namespace

int main(int argc, char* argv[]) {
    pxe::NetRuntime net;  // WSAStartup on Windows, SIGPIPE ignore on POSIX

    // Set console window title for Windows
    ::SetConsoleTitleA("PXE Service");

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    std::string configPath = argc > 1 ? argv[1] : defaultConfigPath();

    std::cout << "=== C++ PXE Server ===" << std::endl;
    std::cout << "Config: " << configPath << std::endl;

    const pxe::Config config = pxe::ConfigLoader::load(configPath);
    if (!config.ok) {
        std::cerr << "Failed to load configuration: " << config.error << std::endl;
        return 1;
    }

    pxe::Logger::instance().setLevel(parseLevel(config.log_level));
    if (!config.log_file.empty()) {
        pxe::Logger::instance().setFile(config.log_file);
    }

    for (const auto& warning : config.warnings) {
        pxe::Logger::instance().warn(warning);
    }

    pxe::Logger::instance().info("Config loaded from " + configPath);
    pxe::Logger::instance().info("Server root: " + config.root);
    pxe::Logger::instance().info("Server IP: " + config.server_ip);

    pxe_server::PXE core;
    if (!core.init(config)) {
        std::cerr << "Failed to initialize PXE server" << std::endl;
        return 1;
    }

    if (!core.start()) {
        std::cerr << "Failed to start PXE server" << std::endl;
        return 1;
    }

    while (g_running.load()) {
        pxe::sleepMs(200);
    }

    pxe::Logger::instance().info("Shutting down...");
    core.stop();
    return 0;
}
