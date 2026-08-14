#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace matchbox::service {

// A deliberately tiny HTTP listener serving two endpoints: /healthz for Kubernetes
// readiness (200 once the predicate is true, 503 before) and /metrics for Prometheus. An
// HTTP framework or a metrics client library would be several orders of magnitude more
// dependency than one status code and one text body justify.
class HealthServer {
 public:
  HealthServer(int port, std::function<bool()> ready, std::function<std::string()> metrics = {});
  ~HealthServer();

  HealthServer(const HealthServer&) = delete;
  HealthServer& operator=(const HealthServer&) = delete;

  bool start();
  void stop();

 private:
  void serve();
  void respond(int client, const std::string& request) const;

  int port_;
  std::function<bool()> ready_;
  std::function<std::string()> metrics_;
  int listen_fd_{-1};
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace matchbox::service
