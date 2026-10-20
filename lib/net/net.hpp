#pragma once
// Minimal portable TCP socket layer: POSIX sockets on Linux/WSL, Winsock2 on Windows (MinGW-w64).
// Keeps protocol code (Modbus server/client) free of platform #ifdefs.
#include <cstdint>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace icl::net {
#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t invalid_socket = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t invalid_socket = -1;
#endif

// Must be called once per process before any socket call (WSAStartup on Windows, no-op elsewhere).
bool startup();
void close_socket(socket_t fd);
bool set_timeouts(socket_t fd, int recv_ms, int send_ms);
void set_nodelay(socket_t fd);
bool interrupted_by_signal();
std::string last_error();

bool receive_all(socket_t fd, std::uint8_t* buf, std::size_t size);
bool send_all(socket_t fd, const std::uint8_t* buf, std::size_t size);

// Listening socket bound to bind_ip:port. Throws std::runtime_error on failure.
socket_t listen_tcp(const std::string& bind_ip, std::uint16_t port, int backlog = 16);
// Blocking connect with socket timeouts applied afterwards. Returns invalid_socket on failure.
socket_t connect_tcp(const std::string& host, std::uint16_t port, int io_timeout_ms);
}
