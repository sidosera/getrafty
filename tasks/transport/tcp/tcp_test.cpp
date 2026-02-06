#include <gtest/gtest.h>

#include <bits/ttl/ttl.hpp>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "bits/queue.hpp"
#include "tcp/tcp_dialer.hpp"
#include "tcp/tcp_listener.hpp"
#include "transport.hpp"

using namespace std::chrono_literals;

template <typename T>
using Q = bits::MPMCBlockingQueue<T>;

using ITransportPtr = std::unique_ptr<ITransport>;

class BaseTransportTest : public ::testing::Test {
protected:
  void SetUp() override { 
    // DEBUG: bits::ttl::Ttl::init("stdout://"); 
    bits::ttl::Ttl::init("discard://"); 
  }

  void TearDown() override { bits::ttl::Ttl::shutdown(); }
};

TEST_F(BaseTransportTest, DialerConnectsToListener) {
  // Arrange
  EventWatcher ew{};
  Q<ITransportPtr> listener_accepted_q;
  Q<ITransportPtr> dialer_connected_q;
  Q<std::string> listener_started_q;

  TcpListener listener("127.0.0.1:0", ew);
  listener.onStarted(
      [&](const std::string& address) { listener_started_q.push(address); });
  listener.onAccepted([&](ITransportPtr transport) {
    listener_accepted_q.push(std::move(transport));
  });
  listener.onFailure([](int err) { FAIL() << "onFailure: " << strerror(err); });

  std::thread server_thread([&]() { listener.listenAndWait(); });

  // Act: wait for listener to start
  constexpr int kMaxIterations = 100;
  std::optional<std::string> listen_addr;
  for (int i = 0; i < kMaxIterations && !listen_addr.has_value(); ++i) {
    ew.loop(10);
    listen_addr = listener_started_q.tryPop();
  }
  ASSERT_TRUE(listen_addr.has_value()) << "Deadline";

  // Act: dial to listener
  TcpDialer dialer(*listen_addr, ew);
  dialer.onConnected([&](ITransportPtr transport) {
    dialer_connected_q.push(std::move(transport));
  });
  dialer.onFailure([](int err) { FAIL() << "onFailure: " << strerror(err); });
  dialer.dial();

  // Act: wait for both connections to establish
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

  // Assert: both sides have valid transports
  ASSERT_TRUE(server_transport.has_value() && client_transport.has_value())
      << "Deadline";

  auto& server_tp = *server_transport;
  auto& client_tp = *client_transport;

  ASSERT_TRUE(server_tp);
  ASSERT_TRUE(client_tp);

  // Assert: endpoints match correctly
  EXPECT_EQ(std::string(server_tp->otherEndpoint()),
            std::string(client_tp->selfEndpoint()));
  EXPECT_EQ(std::string(client_tp->otherEndpoint()),
            std::string(server_tp->selfEndpoint()));

  // Assert: pipeline attached
  EXPECT_NO_THROW(server_tp->pipeline());
  EXPECT_NO_THROW(client_tp->pipeline());

  // Cleanup
  listener.shutdown();
  for (int i = 0; i < 10; ++i) {
    ew.loop(10);
  }
  server_thread.join();
}

TEST_F(BaseTransportTest, MultipleAccepts) {
  // Arrange
  EventWatcher ew{};
  Q<ITransportPtr> listener_accepted_q;
  Q<std::string> listener_started_q;

  TcpListener listener("127.0.0.1:0", ew);
  listener.onStarted(
      [&](const std::string& address) { listener_started_q.push(address); });
  listener.onAccepted([&](ITransportPtr transport) {
    listener_accepted_q.push(std::move(transport));
  });

  std::thread server_thread([&]() { listener.listenAndWait(); });

  // Act: wait for listener to start
  constexpr int kMaxIterations = 100;
  std::optional<std::string> addr;
  for (int i = 0; i < kMaxIterations && !addr.has_value(); ++i) {
    ew.loop(10);
    addr = listener_started_q.tryPop();
  }
  ASSERT_TRUE(addr.has_value()) << "Deadline";

  // Act: create concurrent dialers
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

  // Act: wait for all connections to be accepted
  std::vector<ITransportPtr> accepted;
  for (int i = 0; i < kMaxIterations * 3 && accepted.size() < 3; ++i) {
    ew.loop(10);
    if (auto tp = listener_accepted_q.tryPop()) {
      accepted.push_back(std::move(*tp));
    }
  }

  // Assert: all connections succeeded
  EXPECT_EQ(accepted.size(), 3) << "Deadline";

  // Cleanup
  listener.shutdown();
  for (int i = 0; i < 10; ++i) {
    ew.loop(10);
  }
  server_thread.join();
}

TEST_F(BaseTransportTest, DialerConnectFailure) {
  // Arrange
  EventWatcher ew{};
  Q<int> failure_errno_q;

  TcpDialer dialer(/*malformed*/ "0.0.0.0:8080", ew);
  dialer.onFailure([&](int err) { failure_errno_q.push(err); });
  dialer.onConnected([](auto) { FAIL() << "onConnected"; });

  // Act: attempt to dial to non existent listener
  dialer.dial();

  // Act: wait for failure
  constexpr int kMaxIterations = 100;
  std::optional<int> err;
  for (int i = 0; i < kMaxIterations && !err.has_value(); ++i) {
    ew.loop(10);
    err = failure_errno_q.tryPop();
  }

  // Assert: connection failed
  ASSERT_TRUE(err.has_value()) << "Deadline";
  EXPECT_EQ(*err, ECONNREFUSED);
}

TEST_F(BaseTransportTest, InvalidAddressFormat) {
  // Arrange
  EventWatcher ew{};
  std::vector<std::pair<std::string, std::string>> invalid_cases = {
      {"invalid", "No port separator"},
      {"127.0.0.1", "Missing port"},
      {"127.0.0.1:", "Empty port"},
      {":12345", "Missing host"},
  };

  // Act: dial to invalid address
  for (const auto& [invalid_addr, desc] : invalid_cases) {
    Q<int> failure_q;
    TcpDialer dialer(invalid_addr, ew);

    dialer.onFailure([&](int err) { failure_q.push(err); });
    dialer.onConnected([&](auto) { FAIL() << "Should not connect: " << desc; });

    // Act: attempt to dial with invalid address
    dialer.dial();

    // Act: wait for failure
    constexpr int kMaxIterations = 50;
    std::optional<int> err;
    for (int i = 0; i < kMaxIterations && !err.has_value(); ++i) {
      ew.loop(10);
      err = failure_q.tryPop();
    }

    // Assert: dial failed
    EXPECT_TRUE(err.has_value()) << "Should fail for: " << desc;
    if (err.has_value()) {
      EXPECT_NE(*err, 0) << "Error code should be non-zero for: " << desc;
    }
  }
}

TEST_F(BaseTransportTest, MultipleDialAttempts) {
  // Arrange
  EventWatcher ew{};
  Q<std::string> listener_started_q;
  Q<ITransportPtr> listener_accepted_q;

  TcpListener listener("127.0.0.1:0", ew);
  listener.onStarted(
      [&](const std::string& address) { listener_started_q.push(address); });
  listener.onAccepted([&](ITransportPtr transport) {
    listener_accepted_q.push(std::move(transport));
  });

  std::thread server_thread([&]() { listener.listenAndWait(); });

  // Act: wait for listener to start
  constexpr int kMaxIterations = 100;
  std::optional<std::string> addr;
  for (int i = 0; i < kMaxIterations && !addr.has_value(); ++i) {
    ew.loop(10);
    addr = listener_started_q.tryPop();
  }
  ASSERT_TRUE(addr.has_value());

  // Act: create concurrent dialers
  Q<ITransportPtr> dialer_connected_q;
  std::vector<std::unique_ptr<TcpDialer>> dialers;

  for (int i = 0; i < 5; ++i) {
    auto dialer = std::make_unique<TcpDialer>(*addr, ew);
    dialer->onConnected([&](ITransportPtr transport) {
      dialer_connected_q.push(std::move(transport));
    });
    dialer->onFailure(
        [](int err) { FAIL() << "Dial failed: " << strerror(err); });
    dialer->dial();
    dialers.push_back(std::move(dialer));
  }

  // Act: wait for all connections
  std::vector<ITransportPtr> connected;
  for (int i = 0; i < kMaxIterations * 5 && connected.size() < 5; ++i) {
    ew.loop(10);
    if (auto tp = dialer_connected_q.tryPop()) {
      connected.push_back(std::move(*tp));
    }
  }

  // Assert: all connections succeeded
  EXPECT_EQ(connected.size(), 5);

  // Cleanup
  listener.shutdown();
  for (int i = 0; i < 10; ++i) {
    ew.loop(10);
  }
  server_thread.join();
}

TEST_F(BaseTransportTest, DialToClosedServer) {
  // Arrange
  EventWatcher ew{};
  Q<std::string> listener_started_q;

  TcpListener listener("127.0.0.1:0", ew);
  listener.onStarted(
      [&](const std::string& address) { listener_started_q.push(address); });

  std::thread server_thread([&]() { listener.listenAndWait(); });

  // Act: wait for listener to start
  constexpr int kMaxIterations = 100;
  std::optional<std::string> addr;
  for (int i = 0; i < kMaxIterations && !addr.has_value(); ++i) {
    ew.loop(10);
    addr = listener_started_q.tryPop();
  }
  ASSERT_TRUE(addr.has_value());

  // Act: shutdown listener
  listener.shutdown();
  for (int i = 0; i < 10; ++i) {
    ew.loop(10);
  }
  server_thread.join();

  // Act: attempt to dial to closed listener
  Q<int> failure_q;
  TcpDialer dialer(*addr, ew);

  dialer.onFailure([&](int err) { failure_q.push(err); });
  dialer.onConnected(
      [](auto) { FAIL() << "Should not connect to closed listener"; });

  dialer.dial();

  // Act: wait
  std::optional<int> err;
  for (int i = 0; i < kMaxIterations && !err.has_value(); ++i) {
    ew.loop(10);
    err = failure_q.tryPop();
  }

  // Assert: connection failed
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(*err, ECONNREFUSED);
}

TEST_F(BaseTransportTest, ListenerShutdownStopsAccepting) {
  // Arrange
  EventWatcher ew{};
  Q<std::string> listener_started_q;
  Q<ITransportPtr> listener_accepted_q;

  TcpListener listener("127.0.0.1:0", ew);
  listener.onStarted(
      [&](const std::string& address) { listener_started_q.push(address); });
  listener.onAccepted([&](ITransportPtr transport) {
    listener_accepted_q.push(std::move(transport));
  });

  std::thread server_thread([&]() { listener.listenAndWait(); });

  // Act: wait for listener to start
  constexpr int kMaxIterations = 100;
  std::optional<std::string> addr;
  for (int i = 0; i < kMaxIterations && !addr.has_value(); ++i) {
    ew.loop(10);
    addr = listener_started_q.tryPop();
  }
  ASSERT_TRUE(addr.has_value());

  // Act: establish connection
  Q<ITransportPtr> dialer1_connected_q;
  TcpDialer dialer1(*addr, ew);
  dialer1.onConnected([&](ITransportPtr transport) {
    dialer1_connected_q.push(std::move(transport));
  });
  dialer1.dial();

  std::optional<ITransportPtr> tp1;
  for (int i = 0; i < kMaxIterations && !tp1.has_value(); ++i) {
    ew.loop(10);
    if (!tp1.has_value()) {
      tp1 = listener_accepted_q.tryPop();
    }
    if (!tp1.has_value()) {
      tp1 = dialer1_connected_q.tryPop();
    }
  }
  // Assert: first connection succeeded

  ASSERT_TRUE(tp1.has_value()) << "First connection should succeed";

  // Act: shutdown listener
  listener.shutdown();
  for (int i = 0; i < 10; ++i) {
    ew.loop(10);
  }

  // Act: attempt another connection
  Q<int> dialer2_failure_q;
  TcpDialer dialer2(*addr, ew);
  dialer2.onFailure([&](int err) { dialer2_failure_q.push(err); });
  dialer2.onConnected(
      [](auto) { FAIL() << "Should not connect after shutdown"; });
  dialer2.dial();

  // Act: wait
  std::optional<int> err;
  for (int i = 0; i < kMaxIterations && !err.has_value(); ++i) {
    ew.loop(10);
    err = dialer2_failure_q.tryPop();
  }

  // Assert: subsequent connection failed
  EXPECT_TRUE(err.has_value());
  if (err.has_value()) {
    EXPECT_EQ(*err, ECONNREFUSED);
  }

  // Cleanup
  server_thread.join();
}
