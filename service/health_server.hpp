#pragma once

#include <atomic>
#include <functional>
#include <thread>

namespace matchbox::service {

// A deliberately tiny HTTP listener for Kubernetes readiness probing. Answers 200 once the
// supplied predicate is true and 503 before that, on any path. An HTTP framework would be
// several orders of magnitude more dependency than one status code justifies.
class HealthServer {
 public:
  HealthServer(int port, std::function<bool()> ready);
  ~HealthServer();

  HealthServer(const HealthServer&) = delete;
  HealthServer& operator=(const HealthServer&) = delete;

  bool start();
  void stop();

 private:
  void serve();

  int port_;
  std::function<bool()> ready_;
  int listen_fd_{-1};
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace matchbox::service
