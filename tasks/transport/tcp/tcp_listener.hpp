#pragma once
#include <latch>
#include <string>
#include <tasks/transport/listener.hpp>
#include <tasks/transport/transport.hpp>

#include <tasks/transport/event_watcher/event_watcher.hpp>

using getrafty::io::EventWatcher;

class TcpListener : public IListener {
 public:
  explicit TcpListener(const std::string& address, EventWatcher& ew);
  ~TcpListener() override;

  void onAccepted(F<void(std::unique_ptr<ITransport>)> cb) override {
    on_accepted_ = std::move(cb);
  }

  void onFailure(F<void(int)> cb) override { on_failure_ = std::move(cb); }

  void onError(F<void(int)> cb) override { on_error_ = std::move(cb); }

  void onStarted(F<void(const std::string&)> cb) override {
    on_started_ = std::move(cb);
  }

  void onShutdown(F<void()> cb) override { on_shutdown_ = std::move(cb); }

  void listenAndWait() override;
  void shutdown() override;

 private:
  void onReadable();
  std::unique_ptr<ITransport> createTransport(int client_fd);

  std::string address_;
  EventWatcher& ew_;
  int listen_fd_ = -1;
  std::string bound_address_;

  F<void(std::unique_ptr<ITransport>)> on_accepted_ = [](auto) {};
  F<void(int)> on_failure_ = [](int) {};
  F<void(int)> on_error_ = [](int) {};
  F<void(const std::string&)> on_started_ = [](const std::string&) {};
  F<void()> on_shutdown_ = []() {};
  std::latch shutdown_latch_{1};
};
