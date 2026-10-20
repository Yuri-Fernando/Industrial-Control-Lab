#include "net.hpp"
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace icl::net {
bool startup() {
#ifdef _WIN32
    static const bool ok = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ok;
#else
    return true;
#endif
}

void close_socket(socket_t fd) {
    if (fd == invalid_socket) return;
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

bool set_timeouts(socket_t fd, int recv_ms, int send_ms) {
#ifdef _WIN32
    const DWORD r = static_cast<DWORD>(recv_ms), s = static_cast<DWORD>(send_ms);
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&r), sizeof(r)) == 0 &&
           setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&s), sizeof(s)) == 0;
#else
    timeval r{recv_ms / 1000, (recv_ms % 1000) * 1000}, s{send_ms / 1000, (send_ms % 1000) * 1000};
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &r, sizeof(r)) == 0 &&
           setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &s, sizeof(s)) == 0;
#endif
}

void set_nodelay(socket_t fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
}

bool interrupted_by_signal() {
#ifdef _WIN32
    return WSAGetLastError() == WSAEINTR;
#else
    return errno == EINTR;
#endif
}

std::string last_error() {
#ifdef _WIN32
    return "WSA error " + std::to_string(WSAGetLastError());
#else
    return std::strerror(errno);
#endif
}

bool receive_all(socket_t fd, std::uint8_t* buf, std::size_t size) {
    for (std::size_t got = 0; got < size;) {
        const auto n = recv(fd, reinterpret_cast<char*>(buf + got), static_cast<int>(size - got), 0);
        if (n <= 0) return false;
        got += static_cast<std::size_t>(n);
    }
    return true;
}

bool send_all(socket_t fd, const std::uint8_t* buf, std::size_t size) {
#ifdef _WIN32
    constexpr int flags = 0;
#else
    constexpr int flags = MSG_NOSIGNAL;  // a dropped peer must not kill the controller with SIGPIPE
#endif
    for (std::size_t done = 0; done < size;) {
        const auto n = send(fd, reinterpret_cast<const char*>(buf + done), static_cast<int>(size - done), flags);
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

socket_t listen_tcp(const std::string& bind_ip, std::uint16_t port, int backlog) {
    if (!startup()) throw std::runtime_error("socket subsystem startup failed");
    socket_t server = socket(AF_INET, SOCK_STREAM, 0);
    if (server == invalid_socket) throw std::runtime_error("socket creation failed");
#ifndef _WIN32
    // On Windows SO_REUSEADDR allows port hijacking, so it is only enabled on POSIX.
    int reuse = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_ip.c_str(), &address.sin_addr) != 1) {
        close_socket(server);
        throw std::runtime_error("invalid bind address: " + bind_ip);
    }
    if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        const auto why = last_error();
        close_socket(server);
        throw std::runtime_error("bind " + bind_ip + ":" + std::to_string(port) + ": " + why);
    }
    if (listen(server, backlog) != 0) {
        close_socket(server);
        throw std::runtime_error("listen failed");
    }
    return server;
}

socket_t connect_tcp(const std::string& host, std::uint16_t port, int io_timeout_ms) {
    if (!startup()) return invalid_socket;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0 || !found) return invalid_socket;
    socket_t fd = socket(found->ai_family, found->ai_socktype, found->ai_protocol);
    if (fd != invalid_socket && connect(fd, found->ai_addr, static_cast<int>(found->ai_addrlen)) != 0) {
        close_socket(fd);
        fd = invalid_socket;
    }
    freeaddrinfo(found);
    if (fd != invalid_socket) {
        set_timeouts(fd, io_timeout_ms, io_timeout_ms);
        set_nodelay(fd);
    }
    return fd;
}
}
