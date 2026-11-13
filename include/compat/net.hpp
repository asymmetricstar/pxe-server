#pragma once

/*
 * Cross-platform socket layer.
 *
 * On Windows sockets are SOCKET handles closed with closesocket() and the
 * Winsock runtime must be started before any socket call; on POSIX they are
 * plain file descriptors. Everything network related goes through this header.
 */

#include <string>
#include <stdexcept>
#include <cstring>
#include <cstdint>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#undef ERROR
#undef small

namespace pxe {

using socket_t = SOCKET;
using socklen_type = int;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;

inline bool socketValid(socket_t s) { return s != INVALID_SOCKET; }
inline void socketClose(socket_t s) { if (s != INVALID_SOCKET) ::closesocket(s); }
inline int socketLastError() { return WSAGetLastError(); }
inline bool socketWasTimeout() {
    int e = WSAGetLastError();
    return e == WSAETIMEDOUT || e == WSAEWOULDBLOCK;
}

struct NetRuntime {
    NetRuntime() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
    }
    ~NetRuntime() { WSACleanup(); }
    NetRuntime(const NetRuntime&) = delete;
    NetRuntime& operator=(const NetRuntime&) = delete;
};

inline void sleepMs(int ms) { ::Sleep(static_cast<DWORD>(ms > 0 ? ms : 0)); }

} // namespace pxe

#else

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#include <csignal>

namespace pxe {

using socket_t = int;
using socklen_type = socklen_t;
constexpr socket_t kInvalidSocket = -1;

inline bool socketValid(socket_t s) { return s >= 0; }
inline void socketClose(socket_t s) { if (s >= 0) ::close(s); }
inline int socketLastError() { return errno; }
inline bool socketWasTimeout() { return errno == EAGAIN || errno == EWOULDBLOCK; }

struct NetRuntime {
    NetRuntime() { std::signal(SIGPIPE, SIG_IGN); }
    ~NetRuntime() = default;
    NetRuntime(const NetRuntime&) = delete;
    NetRuntime& operator=(const NetRuntime&) = delete;
};

inline void sleepMs(int ms) {
    if (ms <= 0) return;
    ::usleep(static_cast<useconds_t>(ms) * 1000);
}

} // namespace pxe

#endif

namespace pxe {

inline bool socketSetReuse(socket_t s) {
    int one = 1;
    return ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                        reinterpret_cast<const char*>(&one), sizeof(one)) == 0;
}

inline bool socketSetBroadcast(socket_t s) {
    int one = 1;
    return ::setsockopt(s, SOL_SOCKET, SO_BROADCAST,
                        reinterpret_cast<const char*>(&one), sizeof(one)) == 0;
}

/*
 * Socket timeouts.
 *
 * Windows does NOT accept a POSIX timeval here: SO_RCVTIMEO/SO_SNDTIMEO take a
 * DWORD holding milliseconds. Passing a timeval made the kernel read only the
 * first four bytes (tv_sec), so a requested 30000 ms became 30 ms and 500 ms
 * became 0 (= wait forever). Every TFTP transfer that waited longer than
 * ~30 ms for an ACK died after five attempts (~150 ms) and every HTTP send
 * could time out mid-file, which showed up as intermittent PXE-E53/E77
 * boot failures. Keep the two implementations separate.
 */
#ifdef _WIN32

inline bool socketSetRecvTimeout(socket_t s, int ms) {
    const DWORD timeout = (ms > 0) ? static_cast<DWORD>(ms) : 0;
    return ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                        reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0;
}

inline bool socketSetSendTimeout(socket_t s, int ms) {
    const DWORD timeout = (ms > 0) ? static_cast<DWORD>(ms) : 0;
    return ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO,
                        reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0;
}

#else

inline bool socketSetRecvTimeout(socket_t s, int ms) {
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                        reinterpret_cast<const char*>(&tv), sizeof(tv)) == 0;
}

inline bool socketSetSendTimeout(socket_t s, int ms) {
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO,
                        reinterpret_cast<const char*>(&tv), sizeof(tv)) == 0;
}

#endif

// Half-close and drain before closing so queued response bytes are not
// discarded (closing with pending data sends RST and truncates large replies).
inline void socketGracefulClose(socket_t s, int drainMs = 2000) {
    if (!socketValid(s)) return;
#ifdef _WIN32
    ::shutdown(s, SD_SEND);
#else
    ::shutdown(s, SHUT_WR);
#endif
    socketSetRecvTimeout(s, drainMs);
    char sink[1024];
    for (;;) {
        const int n = ::recv(s, sink, sizeof(sink), 0);
        if (n <= 0) break;
    }
    socketClose(s);
}

inline std::string sockaddrToIp(const struct sockaddr_in& addr) {
    char buf[INET_ADDRSTRLEN] = {0};
    if (::inet_ntop(AF_INET, const_cast<struct in_addr*>(&addr.sin_addr), buf, sizeof(buf)) == nullptr) {
        return "0.0.0.0";
    }
    return buf;
}

inline bool isLoopback(uint32_t ip_network_order) {
    return (ntohl(ip_network_order) & 0xFF000000u) == 0x7F000000u;
}

} // namespace pxe
