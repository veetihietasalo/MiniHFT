#pragma once

// UDP multicast over POSIX sockets: a receiver that joins a group, and a sender. Tested on Linux;
// Windows still has only the Winsock TCP code in Network.hpp (W09).
//
// Things that bite with multicast, handled here:
//   - The receiver binds to the group address, not INADDR_ANY. Bound to 0.0.0.0:port, a socket
//     also gets every other group any process on the host has joined on that port.
//   - Binding isn't joining: IP_ADD_MEMBERSHIP makes the kernel (and, through IGMP, the switches)
//     deliver the group to this host.
//   - When the reader falls behind, the kernel drops datagrams once the socket's receive buffer
//     is full, and says nothing. The receiver asks for a large buffer; without root, Linux caps it
//     at net.core.rmem_max, so receiveBufferBytes() reports what was granted.
//   - Loopback usually has no multicast. To send to a receiver on the same host, send through a
//     multicast-capable interface with IP_MULTICAST_LOOP on (the default here).
//
// receive() never blocks: the caller polls, as a latency-sensitive feed handler does, instead of
// sleeping in the kernel and paying for the wake-up.

#if defined(_WIN32)
#error "UdpSocket.hpp uses POSIX sockets; on Windows see Network.hpp"
#endif

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <optional>
#include <span>
#include <string>
#include <system_error>

class UdpSocket {
public:
    struct Datagram {
        size_t bytes = 0;
        int64_t kernelNs = -1; // when the kernel received it (CLOCK_REALTIME), or -1 if unknown
    };

    // Joins `group` on `port`. `interfaceAddress` picks the interface by its IPv4 address;
    // "0.0.0.0" lets the kernel choose from the routing table. Throws std::system_error.
    static UdpSocket receiver(const std::string& group, uint16_t port, const std::string& interfaceAddress = "0.0.0.0",
                              int receiveBufferBytes = 8 << 20) {
        UdpSocket s(::socket(AF_INET, SOCK_DGRAM, 0), "socket");
        const int on = 1;
        s.check(::setsockopt(s.fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on), "SO_REUSEADDR");
        s.check(::setsockopt(s.fd_, SOL_SOCKET, SO_RCVBUF, &receiveBufferBytes, sizeof receiveBufferBytes), "SO_RCVBUF");
#if defined(SO_TIMESTAMPNS)
        s.check(::setsockopt(s.fd_, SOL_SOCKET, SO_TIMESTAMPNS, &on, sizeof on), "SO_TIMESTAMPNS");
#endif
        const sockaddr_in addr = address(group, port);
        s.check(::bind(s.fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof addr), "bind " + group);

        ip_mreq join{};
        join.imr_multiaddr = addr.sin_addr;
        join.imr_interface = address(interfaceAddress, 0).sin_addr;
        s.check(::setsockopt(s.fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &join, sizeof join), "IP_ADD_MEMBERSHIP " + group);
        s.makeNonBlocking();
        return s;
    }

    // Sends to multicast groups through the interface with address `interfaceAddress` ("0.0.0.0":
    // the routing table decides). ttl 0 keeps packets on this host, 1 on the local network;
    // loopback delivers a copy to receivers on this host. Throws std::system_error.
    static UdpSocket sender(const std::string& interfaceAddress = "0.0.0.0", int ttl = 1, bool loopback = true) {
        UdpSocket s(::socket(AF_INET, SOCK_DGRAM, 0), "socket");
        const in_addr iface = address(interfaceAddress, 0).sin_addr;
        s.check(::setsockopt(s.fd_, IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof iface), "IP_MULTICAST_IF");
        const auto ttlByte = static_cast<unsigned char>(ttl);
        s.check(::setsockopt(s.fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttlByte, sizeof ttlByte), "IP_MULTICAST_TTL");
        const auto loopByte = static_cast<unsigned char>(loopback ? 1 : 0);
        s.check(::setsockopt(s.fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loopByte, sizeof loopByte), "IP_MULTICAST_LOOP");
        return s;
    }

    UdpSocket(UdpSocket&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    UdpSocket& operator=(UdpSocket&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    ~UdpSocket() { close(); }

    // The next datagram, or nothing if none is waiting. A datagram longer than `buffer` is
    // truncated by the kernel; MoldUDP64 then reports it malformed. Throws on a real error.
    [[nodiscard]] std::optional<Datagram> receive(std::span<uint8_t> buffer) const {
        iovec iov{buffer.data(), buffer.size()};
        alignas(cmsghdr) char control[64];
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof control;
        const ssize_t n = ::recvmsg(fd_, &msg, 0);
        if (n < 0) {
            const int e = errno;
            if (e == EAGAIN || e == EINTR) return std::nullopt; // nothing waiting (or a signal): poll again
#if EWOULDBLOCK != EAGAIN // the same value on Linux, where comparing both trips -Wlogical-op
            if (e == EWOULDBLOCK) return std::nullopt;
#endif
            throw std::system_error(e, std::generic_category(), "recvmsg");
        }
        Datagram d;
        d.bytes = static_cast<size_t>(n);
#if defined(SO_TIMESTAMPNS)
        for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPNS) {
                timespec ts{};
                std::memcpy(&ts, CMSG_DATA(c), sizeof ts);
                d.kernelNs = int64_t{ts.tv_sec} * 1'000'000'000 + ts.tv_nsec;
            }
        }
#endif
        return d;
    }

    // Sends one datagram to group:port. False if the kernel refused it (e.g. its send buffer is
    // full); UDP has no retry, so the caller decides.
    [[nodiscard]] bool sendTo(std::span<const uint8_t> datagram, const std::string& group, uint16_t port) const {
        const sockaddr_in addr = address(group, port);
        return ::sendto(fd_, datagram.data(), datagram.size(), 0, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) ==
               static_cast<ssize_t>(datagram.size());
    }

    // The receive buffer the kernel actually granted.
    [[nodiscard]] int receiveBufferBytes() const {
        int bytes = 0;
        socklen_t len = sizeof bytes;
        check(::getsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &bytes, &len), "getsockopt SO_RCVBUF");
        return bytes;
    }

    // CLOCK_REALTIME now, the clock kernel timestamps use.
    [[nodiscard]] static int64_t realtimeNs() {
        timespec ts{};
        ::clock_gettime(CLOCK_REALTIME, &ts);
        return int64_t{ts.tv_sec} * 1'000'000'000 + ts.tv_nsec;
    }

private:
    UdpSocket(int fd, const char* what) : fd_(fd) {
        if (fd_ < 0) throw std::system_error(errno, std::generic_category(), what);
    }

    void check(int result, const std::string& what) const {
        if (result < 0) throw std::system_error(errno, std::generic_category(), what);
    }

    void makeNonBlocking() const {
        const int flags = ::fcntl(fd_, F_GETFL, 0);
        check(flags, "fcntl F_GETFL");
        check(::fcntl(fd_, F_SETFL, flags | O_NONBLOCK), "fcntl F_SETFL");
    }

    static sockaddr_in address(const std::string& ip, uint16_t port) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
            throw std::system_error(std::make_error_code(std::errc::invalid_argument), "not an IPv4 address: " + ip);
        }
        return addr;
    }

    void close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

    int fd_ = -1;
};
