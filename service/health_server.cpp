#include "health_server.hpp"

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

namespace matchbox::service {
namespace {

constexpr int kAcceptPollMs = 200;

const char* kReadyResponse =
    "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 6\r\nConnection: "
    "close\r\n\r\nready\n";
const char* kUnreadyResponse =
    "HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\nContent-Length: "
    "8\r\nConnection: close\r\n\r\nstarting\n";

}  // namespace

HealthServer::HealthServer(int port, std::function<bool()> ready)
    : port_(port), ready_(std::move(ready)) {}

HealthServer::~HealthServer() { stop(); }

bool HealthServer::start() {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    return false;
  }

  int reuse = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(static_cast<std::uint16_t>(port_));

  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
      ::listen(listen_fd_, 16) != 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }

  running_ = true;
  thread_ = std::thread([this] { serve(); });
  return true;
}

void HealthServer::stop() {
  if (!running_.exchange(false)) {
    return;
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
}

void HealthServer::serve() {
  while (running_) {
    pollfd waiting{};
    waiting.fd = listen_fd_;
    waiting.events = POLLIN;

    // Polling rather than blocking in accept() so stop() does not have to race a
    // connection in to make the thread joinable.
    const int events = ::poll(&waiting, 1, kAcceptPollMs);
    if (events <= 0) {
      continue;
    }

    const int client = ::accept(listen_fd_, nullptr, nullptr);
    if (client < 0) {
      continue;
    }

    char scratch[1024];
    ::recv(client, scratch, sizeof(scratch), 0);

    const char* response = ready_() ? kReadyResponse : kUnreadyResponse;
    ::send(client, response, std::strlen(response), 0);
    ::close(client);
  }
}

}  // namespace matchbox::service
