#include <gtest/gtest.h>

#include <cstddef>
#include <functional>
#include <vector>

#include "conveyor.hpp"
#include "event.hpp"
#include "transport.hpp"

namespace {

struct TestTransport final : ITransport {
  Conveyor* pipeline_ptr = nullptr;

  Conveyor& pipeline() noexcept override { return *pipeline_ptr; }

  int write(ByteBuf& /*buf*/) override { return 0; }

  void flush() override {}

  void onFlushCompleted(F<void()> /*unused*/) override {}

  void onWritableStateChanged(F<void(bool)> /*unused*/) override {}

  void readableStateChanged(bool /*unused*/) override {}

  void shutdown(int /*unused*/) override {}

  void onShutdownCompleted(F<void(int)> /*unused*/) override {}

  [[nodiscard]] const char* otherEndpoint() const override { return "test"; }

  [[nodiscard]] const char* selfEndpoint() const override { return "test"; }
};

template <typename T = void>
struct MockStage {
  std::function<void(StageContext&)> on_added = [](auto&) {
  };
  std::function<void(StageContext&)> on_removed = [](auto&) {
  };
  std::function<void(StageContext&)> on_inbound = [](auto&) {
  };
  std::function<void(StageContext&)> on_outbound = [](auto&) {
  };

  void onAdded(StageContext& ctx) { on_added(ctx); }

  void onRemoved(StageContext& ctx) { on_removed(ctx); }

  template <typename E>
  void onInbound(StageContext& ctx, E& evt) {
    on_inbound(ctx);
    ctx.fireInbound(evt);
  }

  template <typename E>
  void onOutbound(StageContext& ctx, E& evt) {
    on_outbound(ctx);
    ctx.fireOutbound(evt);
  }
};

}  // namespace

class ConveyorTest : public ::testing::Test {
protected:
  TestTransport transport_;
  Conveyor pipeline_{&transport_};

  void SetUp() override { transport_.pipeline_ptr = &pipeline_; }
};

TEST_F(ConveyorTest, InboundFlowsForward) {
  std::vector<size_t> order;

  auto capture = [&](size_t id) {
    return MockStage<>{
        .on_added = [&, id](auto&) { order.push_back(id); },
    };
  };

  pipeline_.addLast<MockStage<>>(capture(1));
  pipeline_.addLast<MockStage<>>(capture(2));
  pipeline_.addLast<MockStage<>>(capture(3));

  ASSERT_EQ(order.size(), 3U);
  EXPECT_EQ(order[0], 1U);
  EXPECT_EQ(order[1], 2U);
  EXPECT_EQ(order[2], 3U);
}

TEST_F(ConveyorTest, OutboundFlowsBackward) {
  std::vector<size_t> order;

  auto capture = [&](size_t id) {
    return MockStage<>{
        .on_outbound = [&, id](auto&) { order.push_back(id); },
    };
  };

  pipeline_.addLast<MockStage<>>(capture(1));
  pipeline_.addLast<MockStage<>>(capture(2));
  pipeline_.addLast<MockStage<>>(capture(3));

  OutboundClose evt{};
  pipeline_.fireOutbound(evt);

  ASSERT_EQ(order.size(), 3U);
  EXPECT_EQ(order[0], 3U);
  EXPECT_EQ(order[1], 2U);
  EXPECT_EQ(order[2], 1U);
}

TEST_F(ConveyorTest, Forward) {
  size_t received = 0;

  pipeline_.addLast<MockStage<>>();
  pipeline_.addLast<MockStage<>>(MockStage<>{
      .on_inbound = [&](auto&) { ++received; },
  });

  InboundBytes e1{{}};
  pipeline_.fireInbound(e1);

  EXPECT_EQ(received, 1U);

  InboundTransportActive e2{};
  pipeline_.fireInbound(e2);

  EXPECT_EQ(received, 2U);
}

TEST_F(ConveyorTest, EmptyPipeline) {
  InboundTransportActive inbound{};
  pipeline_.fireInbound(inbound);

  OutboundClose outbound{};
  pipeline_.fireOutbound(outbound);

  SUCCEED();
}

TEST_F(ConveyorTest, OnAdded) {
  std::vector<size_t> indices;

  pipeline_.addLast<MockStage<>>(MockStage<>{
      .on_added = [&](auto& ctx) { indices.push_back(ctx.index()); },
  });
  pipeline_.addLast<MockStage<>>(MockStage<>{
      .on_added = [&](auto& ctx) { indices.push_back(ctx.index()); },
  });
  pipeline_.addLast<MockStage<>>(MockStage<>{
      .on_added = [&](auto& ctx) { indices.push_back(ctx.index()); },
  });

  ASSERT_EQ(indices.size(), 3U);
  EXPECT_EQ(indices[0], 0U);
  EXPECT_EQ(indices[1], 1U);
  EXPECT_EQ(indices[2], 2U);
}

TEST_F(ConveyorTest, TypedDispatch) {
  size_t bytes_count   = 0;
  size_t active_count  = 0;
  size_t suspend_count = 0;

  struct TypedStage {
    size_t* bytes;
    size_t* active;
    size_t* suspend;

    void onInbound(StageContext& ctx, InboundBytes& evt) {
      ++(*bytes);
      ctx.fireInbound(evt);
    }

    void onInbound(StageContext& ctx, InboundTransportActive& evt) {
      ++(*active);
      ctx.fireInbound(evt);
    }

    void onInbound(StageContext& ctx, InboundSuspend& evt) {
      ++(*suspend);
      ctx.fireInbound(evt);
    }
  };

  pipeline_.addLast<TypedStage>(&bytes_count, &active_count, &suspend_count);

  InboundBytes bytes{{}};
  pipeline_.fireInbound(bytes);

  EXPECT_EQ(bytes_count, 1U);
  EXPECT_EQ(active_count, 0U);

  InboundTransportActive active{};
  pipeline_.fireInbound(active);

  EXPECT_EQ(bytes_count, 1U);
  EXPECT_EQ(active_count, 1U);

  InboundSuspend suspend{};
  pipeline_.fireInbound(suspend);

  EXPECT_EQ(suspend_count, 1U);
}

TEST_F(ConveyorTest, Context) {
  size_t index          = 0;
  ITransport* transport = nullptr;

  pipeline_.addLast<MockStage<>>();
  pipeline_.addLast<MockStage<>>(MockStage<>{
      .on_added =
          [&](auto& ctx) {
            index     = ctx.index();
            transport = &ctx.transport();
          },
  });
  pipeline_.addLast<MockStage<>>();

  EXPECT_EQ(index, 1U);
  EXPECT_EQ(transport, &transport_);
}

TEST_F(ConveyorTest, Transform) {
  size_t bytes_received   = 0;
  size_t suspend_received = 0;

  struct Transform {
    size_t* count;

    void onInbound(StageContext& ctx, InboundBytes& /*evt*/) {
      ++(*count);
      InboundSuspend suspend{};
      ctx.fireInbound(suspend);
    }
  };

  pipeline_.addLast<Transform>(&bytes_received);
  pipeline_.addLast<MockStage<>>(MockStage<>{
      .on_inbound = [&](auto&) { ++suspend_received; },
  });

  InboundBytes bytes{{}};
  pipeline_.fireInbound(bytes);

  EXPECT_EQ(bytes_received, 1U);
  EXPECT_EQ(suspend_received, 1U);
}

TEST_F(ConveyorTest, SingleStage) {
  size_t count = 0;

  pipeline_.addLast<MockStage<>>(MockStage<>{
      .on_inbound = [&](auto&) { ++count; },
  });

  InboundTransportActive evt{};
  pipeline_.fireInbound(evt);

  EXPECT_EQ(count, 1U);
}

TEST_F(ConveyorTest, InboundToOutbound) {
  static constexpr int kCloseReason = 42;

  size_t outbound_count = 0;
  int close_reason      = 0;

  struct Trigger {
    void onInbound(StageContext& ctx, InboundTransportActive& /*evt*/) {
      OutboundClose close{kCloseReason};
      ctx.fireOutbound(close);
    }
  };

  struct Capture {
    size_t* count;
    int* reason;

    void onOutbound(StageContext& ctx, OutboundClose& evt) {
      ++(*count);
      *reason = evt.reason;
      ctx.fireOutbound(evt);
    }
  };

  pipeline_.addLast<Capture>(&outbound_count, &close_reason);
  pipeline_.addLast<Trigger>();

  InboundTransportActive evt{};
  pipeline_.fireInbound(evt);

  EXPECT_EQ(outbound_count, 1U);
  EXPECT_EQ(close_reason, kCloseReason);
}
