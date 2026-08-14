#include "health_server.hpp"

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

namespace matchbox::service {
namespace {

constexpr int kAcceptPollMs = 200;
constexpr int kClientTimeoutSeconds = 2;

const char* kReadyResponse =
    "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 6\r\nConnection: "
    "close\r\n\r\nready\n";
const char* kUnreadyResponse =
    "HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\nContent-Length: "
    "8\r\nConnection: close\r\n\r\nstarting\n";

// Only the request target is needed, so this reads the second token of the request line
// rather than parsing HTTP properly - which is all a probe and a scrape ever send.
std::string request_path(const std::string& request) {
  const std::size_t start = request.find(' ');
  if (start == std::string::npos) {
    return {};
  }
  const std::size_t end = request.find(' ', start + 1);
  if (end == std::string::npos) {
    return {};
  }
  return request.substr(start + 1, end - start - 1);
}

}  // namespace

void HealthServer::respond(int client, const std::string& request) const {
  if (metrics_ && request_path(request) == "/metrics") {
    const std::string body = metrics_();
    char header[256];
    const int header_length = std::snprintf(
        header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain; version=0.0.4\r\nContent-Length: "
        "%zu\r\nConnection: close\r\n\r\n",
        body.size());
    ::send(client, header, static_cast<std::size_t>(header_length), 0);
    ::send(client, body.data(), body.size(), 0);
    return;
  }

  const char* response = ready_() ? kReadyResponse : kUnreadyResponse;
  ::send(client, response, std::strlen(response), 0);
}

HealthServer::HealthServer(int port, std::function<bool()> ready,
                           std::function<std::string()> metrics)
    : port_(port), ready_(std::move(ready)), metrics_(std::move(metrics)) {}

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

    // One accept loop serves both the readiness probe and the metrics scrape, so a client
    // that connects and then says nothing would otherwise block it forever - and a stalled
    // health endpoint is a killed pod. Timeouts bound how long any one peer can hold it.
    timeval timeout{};
    timeout.tv_sec = kClientTimeoutSeconds;
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    char scratch[2048];
    const ssize_t received = ::recv(client, scratch, sizeof(scratch) - 1, 0);
    scratch[received > 0 ? received : 0] = '\0';

    respond(client, std::string(scratch));
    ::close(client);
  }
}

}  // namespace matchbox::service
