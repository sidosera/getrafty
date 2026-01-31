#include <gtest/gtest.h>

#include <bits/ttl/ttl.hpp>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "bits/queue.hpp"
#include "bits/util.hpp"
#include "event_watcher/event_watcher.hpp"
#include "tcp/tcp_dialer.hpp"
#include "tcp/tcp_listener.hpp"
#include "transport.hpp"

using namespace std::chrono_literals;

template<typename T>
using Q = bits::MPMCBlockingQueue<T>;

using ITransportPtr = std::unique_ptr<ITransport>;

class BaseTransportTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bits::ttl::Ttl::init("stdout://");
  }

  void TearDown() override {
    bits::ttl::Ttl::shutdown();
  }
};

TEST_F(BaseTransportTest, DialerConnectsToListener) {
  EventWatcher ew{};

  Q<ITransportPtr> listener_accepted_q;
  Q<ITransportPtr> dialer_connected_q;
  Q<std::string> listener_started_q;

  // Listener
  TcpListener listener("127.0.0.1:0", ew);

  listener.onStarted([&](const std::string& address) {
    listener_started_q.push(address);
  });

  listener.onAccepted([&](ITransportPtr transport) {
    listener_accepted_q.push(std::move(transport));
  });

  listener.onFailure([](int err) {
    FAIL() << "onFailure: " << strerror(err);
  });

  std::thread server_thread([&]() { listener.listenAndWait(); });

  constexpr int kMaxIterations = 100;
  std::optional<std::string> listen_addr;
  for (int i = 0; i < kMaxIterations && !listen_addr.has_value(); ++i) {
    ew.loop(10);
    listen_addr = listener_started_q.tryPop();
  }
  ASSERT_TRUE(listen_addr.has_value()) << "Deadline";

  // Dialer
  TcpDialer dialer(*listen_addr, ew);

  dialer.onConnected([&](ITransportPtr transport) {
    dialer_connected_q.push(std::move(transport));
  });

  dialer.onFailure([](int err) {
    FAIL() << "onFailure: " << strerror(err);
  });

  dialer.dial();

  std::optional<ITransportPtr> server_transport;
  std::optional<ITransportPtr> client_transport;

  for (int i = 0; i < kMaxIterations; ++i) {
    ew.loop(10);
    if (!server_transport.has_value()) {
      server_transport = listener_accepted_q.tryPop();
    }
    if (!client_transport.has_value()) {
      client_transport = dialer_connected_q.tryPop();
    }
    if (server_transport.has_value() && client_transport.has_value()) {
      break;
    }
  }

  ASSERT_TRUE(server_transport.has_value() && client_transport.has_value()) << "Deadline";

  auto& server_tp = *server_transport;
  auto& client_tp = *client_transport;

  ASSERT_TRUE(server_tp);
  ASSERT_TRUE(client_tp);

  EXPECT_EQ(std::string(server_tp->otherEndpoint()),
            std::string(client_tp->selfEndpoint()));
  EXPECT_EQ(std::string(client_tp->otherEndpoint()),
            std::string(server_tp->selfEndpoint()));

  EXPECT_NO_THROW(server_tp->pipeline());
  EXPECT_NO_THROW(client_tp->pipeline());

  listener.shutdown();
  for (int i = 0; i < 10; ++i) {
    ew.loop(10);
  }
  server_thread.join();
}

TEST_F(BaseTransportTest, MultipleAccepts) {
  EventWatcher ew{};
  Q<ITransportPtr> listener_accepted_q;
  Q<std::string> listener_started_q;

  // Listener
  TcpListener listener("127.0.0.1:0", ew);
  listener.onStarted([&](const std::string& address) {
    listener_started_q.push(address);
  });

  listener.onAccepted([&](ITransportPtr transport) {
    listener_accepted_q.push(std::move(transport));
  });

  std::thread server_thread([&]() { listener.listenAndWait(); });

  // Ramp up listener
  constexpr int kMaxIterations = 100;
  std::optional<std::string> addr;
  for (int i = 0; i < kMaxIterations && !addr.has_value(); ++i) {
    ew.loop(10);
    addr = listener_started_q.tryPop();
  }
  ASSERT_TRUE(addr.has_value()) << "Deadline";

  // Dialer
  std::vector<std::unique_ptr<TcpDialer>> dialers;
  Q<ITransportPtr> dialer_connected_q;

  for (int i = 0; i < 3; ++i) {
    auto dialer = std::make_unique<TcpDialer>(*addr, ew);
    dialer->onConnected([&](ITransportPtr transport) {
      dialer_connected_q.push(std::move(transport));
    });
    dialer->dial();
    dialers.push_back(std::move(dialer));
  }

  // Ramp up dialers
  std::vector<ITransportPtr> accepted;
  for (int i = 0; i < kMaxIterations * 3 && accepted.size() < 3; ++i) {
    ew.loop(10);
    if (auto tp = listener_accepted_q.tryPop()) {
      accepted.push_back(std::move(*tp));
    }
  }

  EXPECT_EQ(accepted.size(), 3) << "Deadline";

  listener.shutdown();
  for (int i = 0; i < 10; ++i) {
    ew.loop(10);
  }
  server_thread.join();
}

TEST_F(BaseTransportTest, DialerConnectFailure) {
  EventWatcher ew{};

  // Dialer
  TcpDialer dialer(/*malformed*/"0.0.0.0:8080", ew);

  Q<int> failure_errno_q;
  dialer.onFailure([&](int err) { failure_errno_q.push(err); });

  dialer.onConnected([](auto) { FAIL() << "onConnected"; });

  dialer.dial();

  // Ramp up
  constexpr int kMaxIterations = 100;
  std::optional<int> err;
  for (int i = 0; i < kMaxIterations && !err.has_value(); ++i) {
    ew.loop(10);
    err = failure_errno_q.tryPop();
  }

  ASSERT_TRUE(err.has_value()) << "Deadline";
  EXPECT_EQ(*err, ECONNREFUSED);
}
