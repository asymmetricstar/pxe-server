#include "server/dhcp_server.hpp"
#include "utils/iso_menu.hpp"
#include "utils/logger.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace pxe {
namespace {

#pragma pack(push, 1)
struct RawPacket {
    uint8_t op;
    uint8_t htype;
    uint8_t hlen;
    uint8_t hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint8_t magic[4];
    uint8_t options[312];
};
#pragma pack(pop)

constexpr uint8_t kBootRequest = 1;
constexpr uint8_t kBootReply = 2;

enum : uint8_t {
    DHCPDISCOVER = 1,
    DHCPOFFER = 2,
    DHCPREQUEST = 3,
    DHCPDECLINE = 4,
    DHCPACK = 5,
    DHCPNAK = 6,
    DHCPRELEASE = 7,
    DHCPINFORM = 8
};

enum : uint8_t {
    OPT_PAD = 0,
    OPT_SUBNET_MASK = 1,
    OPT_ROUTER = 3,
    OPT_DNS = 6,
    OPT_HOSTNAME = 12,
    OPT_DOMAIN = 15,
    OPT_BROADCAST = 28,
    OPT_REQUESTED_IP = 50,
    OPT_LEASE_TIME = 51,
    OPT_MSG_TYPE = 53,
    OPT_SERVER_ID = 54,
    OPT_PARAM_LIST = 55,
    OPT_VENDOR_CLASS = 60,
    OPT_CLIENT_ID = 61,
    OPT_TFTP_NAME = 66,
    OPT_BOOTFILE = 67,
    OPT_USER_CLASS = 77,
    OPT_VENDOR_SPECIFIC = 43,
    OPT_CLIENT_ARCH = 93,
    OPT_END = 255
};

std::string macToString(const uint8_t* hw, uint8_t len) {
    static const char* hex = "0123456789abcdef";
    if (len == 0 || len > 16) len = 6;
    std::string out;
    out.reserve(17);
    for (uint8_t i = 0; i < len; ++i) {
        if (i) out += ':';
        out += hex[hw[i] >> 4];
        out += hex[hw[i] & 0x0F];
    }
    return out;
}

uint32_t ipToInt(const std::string& text) {
    struct in_addr addr{};
    if (::inet_pton(AF_INET, text.c_str(), &addr) != 1) return 0;
    return addr.s_addr;
}

std::string intToIp(uint32_t ip) {
    struct in_addr addr{};
    addr.s_addr = ip;
    char buf[INET_ADDRSTRLEN] = {0};
    if (::inet_ntop(AF_INET, &addr, buf, sizeof(buf)) == nullptr) return "0.0.0.0";
    return buf;
}

std::map<uint8_t, std::vector<uint8_t>> parseOptions(const uint8_t* opts, size_t len) {
    std::map<uint8_t, std::vector<uint8_t>> result;
    size_t i = 0;
    while (i < len) {
        const uint8_t code = opts[i++];
        if (code == OPT_PAD) continue;
        if (code == OPT_END) break;
        if (i >= len) break;
        const uint8_t size = opts[i++];
        if (i + size > len) break;
        result[code] = std::vector<uint8_t>(opts + i, opts + i + size);
        i += size;
    }
    return result;
}

struct ReplyBuilder {
    RawPacket pkt;
    uint8_t opts[312];
    size_t optLen = 0;
    size_t total = 0;

    ReplyBuilder(const RawPacket& req, uint8_t msgType) {
        std::memset(&pkt, 0, sizeof(pkt));
        std::memset(opts, 0, sizeof(opts));
        pkt.op = kBootReply;
        pkt.htype = req.htype;
        pkt.hlen = req.hlen;
        pkt.hops = req.hops;
        pkt.xid = req.xid;
        pkt.flags = req.flags;
        pkt.ciaddr = req.ciaddr;
        pkt.giaddr = req.giaddr;
        std::memcpy(pkt.chaddr, req.chaddr, sizeof(pkt.chaddr));
        pkt.magic[0] = 99;
        pkt.magic[1] = 130;
        pkt.magic[2] = 83;
        pkt.magic[3] = 99;
        addByte(OPT_MSG_TYPE, msgType);
    }

    void addByte(uint8_t code, uint8_t value) { add(code, &value, 1); }

    void addU32(uint8_t code, uint32_t value) {
        const uint32_t be = htonl(value);
        add(code, &be, sizeof(be));
    }

    void add(uint8_t code, const void* data, size_t len) {
        if (optLen + len + 2 > sizeof(opts)) return;
        opts[optLen++] = code;
        opts[optLen++] = static_cast<uint8_t>(len);
        std::memcpy(opts + optLen, data, len);
        optLen += len;
    }

    void addString(uint8_t code, const std::string& value) {
        add(code, value.data(), value.size());
    }

    const uint8_t* finish(size_t& outLen) {
        if (optLen < sizeof(opts)) opts[optLen++] = OPT_END;
        std::memcpy(pkt.options, opts, optLen);
        outLen = 240 + optLen;
        // Many PXE/BOOTP ROMs expect at least 300 bytes total.
        if (outLen < 300) outLen = 300;
        if (outLen > sizeof(RawPacket)) outLen = sizeof(RawPacket);
        total = outLen;
        return reinterpret_cast<const uint8_t*>(&pkt);
    }
};

} // namespace

DHCPServer::DHCPServer(const Config& cfg) : cfg_(cfg) {
    range_start_ = ipToInt(cfg_.range_start);
    range_end_ = ipToInt(cfg_.range_end);
    server_ip_ = ipToInt(cfg_.server_ip);
}

DHCPServer::~DHCPServer() {
    stop();
}

uint32_t DHCPServer::allocate(const std::string& mac) {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto existing = leases_.find(mac);
    if (existing != leases_.end()) return existing->second;

    const uint32_t start = ntohl(range_start_);
    const uint32_t end = ntohl(range_end_);
    if (start == 0 || end < start) return 0;

    for (uint32_t candidate = start; candidate <= end; ++candidate) {
        const uint32_t ip = htonl(candidate);
        /*
         * Never hand out our own address (the pool usually starts right after
         * the gateway, so the server IP can fall inside it), nor the network
         * or broadcast address of the subnet.
         */
        if (ip == server_ip_) continue;
        if ((candidate & 0xFF) == 0 || (candidate & 0xFF) == 0xFF) continue;
        bool used = false;
        for (const auto& lease : leases_) {
            if (lease.second == ip) { used = true; break; }
        }
        if (!used) {
            leases_[mac] = ip;
            return ip;
        }
    }
    return 0;
}

void DHCPServer::release(const std::string& mac) {
    std::lock_guard<std::mutex> lock(mutex_);
    leases_.erase(mac);
}

void DHCPServer::handlePacket(const char* data, size_t len, const struct sockaddr_in& from) {
    if (len < 240) return;

    RawPacket req;
    std::memset(&req, 0, sizeof(req));
    std::memcpy(&req, data, sizeof(req) < len ? sizeof(req) : len);

    if (req.op != kBootRequest) return;

    const auto options = parseOptions(
        reinterpret_cast<const uint8_t*>(data) + 240, len - 240);
    const auto typeIt = options.find(OPT_MSG_TYPE);
    if (typeIt == options.end() || typeIt->second.empty()) return;
    const uint8_t msgType = typeIt->second[0];

    const std::string mac = macToString(req.chaddr, req.hlen);

    auto msgName = [](uint8_t t) -> std::string {
        switch (t) {
            case DHCPDISCOVER: return "DISCOVER";
            case DHCPOFFER: return "OFFER";
            case DHCPREQUEST: return "REQUEST";
            case DHCPDECLINE: return "DECLINE";
            case DHCPACK: return "ACK";
            case DHCPNAK: return "NAK";
            case DHCPRELEASE: return "RELEASE";
            case DHCPINFORM: return "INFORM";
            default: return "TYPE_" + std::to_string(t);
        }
    };

    Logger::instance().info("DHCP recv " + msgName(msgType) + " from " + mac + " xid=" +
                            std::to_string(ntohl(req.xid)) + " ciaddr=" + intToIp(req.ciaddr) +
                            " flags=0x" + [&]() { char b[8]; std::snprintf(b, sizeof(b), "%04x", ntohs(req.flags)); return std::string(b); }());

    if (msgType == DHCPRELEASE || msgType == DHCPDECLINE) {
        release(mac);
        Logger::instance().info("DHCP " + std::string(msgType == DHCPRELEASE ? "RELEASE" : "DECLINE") +
                                " from " + mac + " -> lease released");
        return;
    }

    if (msgType != DHCPDISCOVER && msgType != DHCPREQUEST && msgType != DHCPINFORM) {
        Logger::instance().debug("DHCP unsupported message type " + std::to_string(msgType) + " from " + mac);
        return;
    }

    /*
     * PXE ROMs often broadcast the same DISCOVER/REQUEST twice within a few
     * milliseconds. Replying to both doubles every OFFER/ACK on the wire;
     * answer only the first copy inside a short window.
     */
    const std::string seenKey = mac + "|" + std::to_string(msgType) + "|" +
                                std::to_string(ntohl(req.xid));
    {
        std::lock_guard<std::mutex> lock(seenMutex_);
        const auto now = std::chrono::steady_clock::now();
        if (seen_.size() > 256) {
            for (auto it = seen_.begin(); it != seen_.end();) {
                if (std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count() > 10) {
                    it = seen_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        const auto it = seen_.find(seenKey);
        if (it != seen_.end() &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count() < 1000) {
            Logger::instance().debug("DHCP duplicate " + msgName(msgType) + " from " + mac + " suppressed");
            return;
        }
        seen_[seenKey] = now;
    }

    if (msgType == DHCPDISCOVER) {
        // The client is about to fetch the boot menu: refresh the generated ISO
        // block first so a newly copied .iso is listed without a restart.
        IsoMenu::refresh(cfg_);
    }

    /*
     * Only answer a REQUEST that was addressed to this server.
     *
     * On a LAN with a second DHCP server (a home router, VMware's vmnetdhcp,
     * ...) the client broadcasts its REQUEST but honours the ACK of exactly
     * one server, identified by option 54. Replying to requests meant for
     * somebody else produces duplicate ACKs and a client that ends up with
     * a lease that has no boot filename -> "PXE-E53: No boot filename
     * received" and "Operating System not found".
     */
    if (msgType == DHCPREQUEST) {
        if (const auto sid = options.find(OPT_SERVER_ID);
            sid != options.end() && sid->second.size() == 4) {
            uint32_t chosen = 0;
            std::memcpy(&chosen, sid->second.data(), sizeof(chosen));
            if (chosen != 0 && chosen != server_ip_) {
                Logger::instance().warn("DHCP REQUEST from " + mac + " selected another DHCP server " +
                                        intToIp(chosen) + " (this server is " + intToIp(server_ip_) +
                                        "); ignoring it - two DHCP servers on one LAN make PXE boot unreliable");
                return;
            }
        }
    }

    const uint32_t yiaddr = allocate(mac);
    if (yiaddr == 0) {
        Logger::instance().error("DHCP address pool exhausted, cannot reply to " + mac);
        return;
    }

    uint16_t arch = 0;
    const auto archIt = options.find(OPT_CLIENT_ARCH);
    if (archIt != options.end() && archIt->second.size() >= 2) {
        arch = static_cast<uint16_t>((archIt->second[0] << 8) | archIt->second[1]);
    }
    /*
     * Client system architecture (option 93). 0 = legacy BIOS, 2 = Itanium
     * EFI and everything from 6 up is an EFI flavour (6=IA32, 7=x64,
     * 8=XScale, 9=ARM, 10=AArch64, 11=RISC-V).
     */
    const bool isEfi = (arch == 2) || (arch >= 6 && arch <= 11);

    bool isIpxe = false;
    bool isPxeClient = false;
    auto containsIpxe = [](const std::vector<uint8_t>& v) {
        if (v.size() < 4) return false;
        for (size_t i = 0; i + 3 < v.size(); ++i) {
            if ((v[i] == 'i' || v[i] == 'I') && (v[i + 1] == 'P' || v[i + 1] == 'p') &&
                (v[i + 2] == 'X' || v[i + 2] == 'x') && (v[i + 3] == 'E' || v[i + 3] == 'e')) {
                return true;
            }
        }
        return false;
    };
    auto containsPxeClient = [](const std::vector<uint8_t>& v) {
        const std::string s(v.begin(), v.end());
        return s.find("PXEClient") != std::string::npos;
    };
    if (const auto it = options.find(OPT_USER_CLASS); it != options.end()) isIpxe = isIpxe || containsIpxe(it->second);
    if (const auto it = options.find(OPT_VENDOR_CLASS); it != options.end()) {
        isIpxe = isIpxe || containsIpxe(it->second);
        isPxeClient = isPxeClient || containsPxeClient(it->second);
    }
    if (archIt != options.end()) isPxeClient = true; // client system architecture option implies PXE boot intention

    /*
     * Fallback for firmwares that omit option 93: look for an explicit EFI
     * marker in the vendor/user class.
     *
     * The hardware vendor name must NOT be treated as an EFI marker: "VMware"
     * is present in BIOS ROMs too, and matching it handed grubx64.efi to a
     * legacy BIOS client, which cannot execute an EFI image (PXE-E53/E77).
     */
    bool isUefi = isEfi;
    auto hasEfiMarker = [](const std::vector<uint8_t>& v) {
        std::string s(v.begin(), v.end());
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s.find("efi") != std::string::npos;
    };
    if (const auto uIt = options.find(OPT_VENDOR_CLASS); uIt != options.end()) {
        isUefi = isUefi || hasEfiMarker(uIt->second);
    }
    if (const auto uIt = options.find(OPT_USER_CLASS); uIt != options.end()) {
        isUefi = isUefi || hasEfiMarker(uIt->second);
    }
    std::string bootFile;
    if (isUefi && !cfg_.boot_file_efi.empty()) {
        bootFile = cfg_.boot_file_efi;
    } else if (isIpxe) {
        // Stage 2 (iPXE already loaded): hand off to HTTP boot script for fast, reliable loading.
        bootFile = "http://" + cfg_.server_ip + "/boot.ipxe";
    } else {
        // Stage 1 (BIOS PXE ROM): load iPXE via TFTP.
        bootFile = cfg_.boot_file;
    }

    const uint8_t replyType = (msgType == DHCPDISCOVER) ? DHCPOFFER : DHCPACK;
    ReplyBuilder reply(req, replyType);
    reply.pkt.yiaddr = yiaddr;
    /*
     * next-server (siaddr) is always filled in, even when the boot file itself
     * is an http:// URL. PXE ROMs that do not understand HTTP boot still need
     * siaddr/option 66 to know where to fetch the first stage from, and
     * omitting them is exactly what produces a PXE-E53 "no boot filename".
     */
    reply.pkt.siaddr = server_ip_;
    // sname (next-server host name) - some PXE ROMs look at this.
    std::memset(reply.pkt.sname, 0, sizeof(reply.pkt.sname));
    std::strncpy(reinterpret_cast<char*>(reply.pkt.sname), cfg_.server_ip.c_str(),
                 sizeof(reply.pkt.sname) - 1);

    reply.addU32(OPT_SERVER_ID, ntohl(server_ip_));
    reply.addU32(OPT_LEASE_TIME, static_cast<uint32_t>(cfg_.lease_time));
    const uint32_t mask = ipToInt(cfg_.subnet_mask);
    reply.add(OPT_SUBNET_MASK, &mask, 4);
    if (!cfg_.router.empty()) {
        const uint32_t router = ipToInt(cfg_.router);
        reply.add(OPT_ROUTER, &router, 4);
    }
    if (!cfg_.dns.empty()) {
        const uint32_t dns = ipToInt(cfg_.dns);
        reply.add(OPT_DNS, &dns, 4);
    }
    if (!cfg_.domain.empty()) reply.addString(OPT_DOMAIN, cfg_.domain);
    // Option 66: TFTP server name/address. Always present - see siaddr note above.
    reply.addString(OPT_TFTP_NAME, cfg_.server_ip);
    // Option 67: boot file name.
    reply.addString(OPT_BOOTFILE, bootFile);
    /*
     * Option 60 is deliberately NOT set here.
     *
     * Advertising "PXEClient" turns the reply into a PXE *proxy* offer: the
     * Intel/VMware PXE ROM then expects a PXE boot server on UDP 4011
     * (option 43 discovery control) before it will accept the filename, and
     * while it waits it shows "DHCP...." and re-sends DISCOVER every 20 s.
     * A plain DHCP reply with siaddr + option 66/67 is what the ROM uses to
     * download the NBP directly, so option 60 only adds a failure mode here.
     */

    std::memset(reply.pkt.file, 0, sizeof(reply.pkt.file));
    std::strncpy(reinterpret_cast<char*>(reply.pkt.file), bootFile.c_str(),
                 sizeof(reply.pkt.file) - 1);

    size_t replyLen = 0;
    const uint8_t* buffer = reply.finish(replyLen);

    struct sockaddr_in dst;
    std::memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(68);

    if (req.giaddr != 0) {
        // Relay agent listens on BOOTPS (67).
        dst.sin_addr.s_addr = req.giaddr;
        dst.sin_port = htons(static_cast<uint16_t>(cfg_.dhcp_port));
    } else if (isLoopback(from.sin_addr.s_addr)) {
        // Local development/testing: answer the sender directly.
        dst.sin_addr.s_addr = from.sin_addr.s_addr;
        dst.sin_port = from.sin_port;
    } else if ((ntohs(req.flags) & 0x8000) != 0 || req.ciaddr == 0) {
        /*
         * Directed (subnet) broadcast, not 255.255.255.255.
         *
         * On a multi-homed Windows host the limited broadcast leaves through
         * whichever interface the route table happens to pick (often the
         * physical Ethernet), so a VM attached to a TAP/virtual adapter never
         * sees the reply. The subnet broadcast is routed through the interface
         * that owns server_ip, which is exactly the segment the client is on.
         */
        const uint32_t mask = ipToInt(cfg_.subnet_mask);
        dst.sin_addr.s_addr = (server_ip_ & mask) | ~mask;
    } else {
        dst.sin_addr.s_addr = req.ciaddr;
    }

    const int primarySent = ::sendto(sock_, reinterpret_cast<const char*>(buffer), static_cast<int>(replyLen), 0,
                                     reinterpret_cast<struct sockaddr*>(&dst), sizeof(dst));
    if (primarySent < 0) {
        Logger::instance().error(std::string("DHCP send failed ") + (replyType == DHCPOFFER ? "OFFER" : "ACK") +
                                 " to " + mac + " dst=" + intToIp(dst.sin_addr.s_addr) + ":" +
                                 std::to_string(ntohs(dst.sin_port)) +
                                 " err=" + std::to_string(socketLastError()));
        return;
    }
    const std::string dstDesc = intToIp(dst.sin_addr.s_addr) + ":" + std::to_string(ntohs(dst.sin_port));

    Logger::instance().info(std::string("DHCP ") + (replyType == DHCPOFFER ? "OFFER" : "ACK") +
                            " " + intToIp(yiaddr) + " -> " + mac +
                            " arch=" + std::to_string(arch) +
                            " boot=" + bootFile +
                            " dst=" + dstDesc +
                            " bytes=" + std::to_string(primarySent) +
                            " siaddr=" + intToIp(reply.pkt.siaddr) +
                            " pxe=" + (isPxeClient ? "yes" : "no") +
                            " ipxe=" + (isIpxe ? "yes" : "no"));
}

void DHCPServer::loop() {
    char buffer[1500];
    while (running_.load()) {
        struct sockaddr_in from;
        std::memset(&from, 0, sizeof(from));
        socklen_type fromLen = sizeof(from);

        const int n = ::recvfrom(sock_, buffer, sizeof(buffer), 0,
                                 reinterpret_cast<struct sockaddr*>(&from), &fromLen);
        if (n <= 0) {
            if (!running_.load()) break;
            continue;
        }
        handlePacket(buffer, static_cast<size_t>(n), from);
    }
}

bool DHCPServer::start() {
    if (running_.load()) return true;

    sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (!socketValid(sock_)) {
        error_ = "socket() failed";
        Logger::instance().error("DHCP " + error_);
        return false;
    }

    socketSetReuse(sock_);
    socketSetBroadcast(sock_);

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(cfg_.dhcp_port));
    /*
     * Always bind INADDR_ANY: DHCP clients broadcast from 0.0.0.0 and the
     * offer may have to leave through a different interface than the one
     * named in the config. Binding a single address silently drops replies
     * whenever the config's host does not match the answering NIC.
     */
    addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(sock_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        error_ = "bind failed on port " + std::to_string(cfg_.dhcp_port) +
                 " (error " + std::to_string(socketLastError()) + ")";
        Logger::instance().error("DHCP " + error_);
        socketClose(sock_);
        sock_ = kInvalidSocket;
        return false;
    }

    socketSetRecvTimeout(sock_, 500);
    running_.store(true);
    thread_ = std::thread(&DHCPServer::loop, this);

    Logger::instance().info("DHCP listening on " + cfg_.host + ":" + std::to_string(cfg_.dhcp_port) +
                            " range " + cfg_.range_start + "-" + cfg_.range_end +
                            " server " + cfg_.server_ip);
    return true;
}

void DHCPServer::stop() {
    if (!running_.exchange(false)) {
        if (socketValid(sock_)) {
            socketClose(sock_);
            sock_ = kInvalidSocket;
        }
        return;
    }
    if (socketValid(sock_)) {
        socketClose(sock_);
        sock_ = kInvalidSocket;
    }
    if (thread_.joinable()) thread_.join();
    Logger::instance().info("DHCP stopped");
}

} // namespace pxe
