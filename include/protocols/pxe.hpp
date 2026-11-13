#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <map>

namespace pxe {

enum class State {
    DISCOVER,
    REQUEST,
    ACTIVE
};

struct BootPxeRequest {
    std::string client_ip;
    std::string mac_address;
    std::string boot_file;
};

struct BootPxeResponse {
    bool success;
    std::string server_ip;
    std::string server_mac;
    std::string boot_file;
    std::string options;
};

class PXEBoot {
public:
    PXEBoot();
    ~PXEBoot();
    bool handleDhcpOffer(const std::string& offer_data);
    bool handleDhcpAck(const std::string& ack_data);
    bool requestBootFile(const std::string& filename);
private:
    State state_;
    bool parseOffer(const std::string& data);
    bool parseAck(const std::string& data);
};

} // namespace pxe
