#include <gtest/gtest.h>

#include <cstddef>
#include <functional>
#include <utility>
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

  [[nodiscard]] const char* otherEndpoint() const override { return "other"; }

  [[nodiscard]] const char* selfEndpoint() const override { return "self"; }
};

template <size_t N = 0>
struct AnyStage {
  static constexpr size_t kN = N;

  explicit AnyStage(
      std::move_only_function<void(size_t)> on_inbound_injected  = [](auto) {},
      std::move_only_function<void(size_t)> on_outbound_injected = [](auto) {},
      std::move_only_function<void(size_t)> on_added_injected    = [](auto) {})
      : on_inbound_injected_(std::move(on_inbound_injected)),
        on_outbound_injected_(std::move(on_outbound_injected)),
        on_added_injected_(std::move(on_added_injected)) {};

  void onAdded(StageContext& /*unused*/) { on_added_injected_(kN); }

  template <typename E>
  void onInbound(StageContext& ctx, E& evt) {
    on_inbound_injected_(kN);
    ctx.fireInbound(evt);
  }

  template <typename E>
  void onOutbound(StageContext& ctx, E& evt) {
    on_outbound_injected_(kN);
    ctx.fireOutbound(evt);
  }

  std::move_only_function<void(size_t)> on_inbound_injected_;
  std::move_only_function<void(size_t)> on_outbound_injected_;
  std::move_only_function<void(size_t)> on_added_injected_;
};

struct NoneStage {};

}  // namespace

TEST(ConveyorTest, InboundSequence) {
  TestTransport transport;
  Conveyor pipeline(&transport);
  transport.pipeline_ptr = &pipeline;

  std::vector<size_t> order;

  pipeline.addLast<AnyStage<1>>(
      /*on_inbound_injected=*/[&](size_t s) { order.push_back(s); });
  pipeline.addLast<AnyStage<2>>(
      /*on_inbound_injected=*/[&](size_t s) { order.push_back(s); });

  InboundTransportActive evt{};
  pipeline.fireInbound(evt);

  ASSERT_EQ(order.size(), 2U);
  EXPECT_EQ(order[0], 1U);
  EXPECT_EQ(order[1], 2U);
}

TEST(ConveyorTest, OutboundSequence) {
  TestTransport transport;
  Conveyor pipeline(&transport);
  transport.pipeline_ptr = &pipeline;

  std::vector<size_t> order;

  pipeline.addLast<AnyStage<1>>(
      /*on_inbound_injected=*/[](auto) {},
      /*on_outbound_injected=*/[&](size_t s) { order.push_back(s); });
  pipeline.addLast<AnyStage<2>>(
      /*on_inbound_injected=*/[](auto) {},
      /*on_outbound_injected=*/[&](size_t s) { order.push_back(s); });

  OutboundClose evt{};
  pipeline.fireOutbound(evt);

  ASSERT_EQ(order.size(), 2U);
  EXPECT_EQ(order[0], 2U);
  EXPECT_EQ(order[1], 1U);
}

TEST(ConveyorTest, EmptyStagePassthrough) {
  TestTransport transport;
  Conveyor pipeline(&transport);
  transport.pipeline_ptr = &pipeline;

  int counter = 0;
  pipeline.addLast<NoneStage>();  // passthrough
  pipeline.addLast<AnyStage<>>(
      /*on_inbound_injected=*/[&](size_t) { ++counter; });

  InboundBytes e1{{}};
  pipeline.fireInbound(e1);
  EXPECT_EQ(counter, 1);

  InboundTransportActive e2{};
  pipeline.fireInbound(e2);
  EXPECT_EQ(counter, 2);
}

TEST(ConveyorTest, EmptyPipeline) {
  TestTransport transport;
  Conveyor pipeline(&transport);
  transport.pipeline_ptr = &pipeline;

  // OK
  InboundTransportActive in{};
  pipeline.fireInbound(in);

  // OK
  OutboundClose out{};
  pipeline.fireOutbound(out);
}

TEST(ConveyorTest, OnAdded) {
  TestTransport transport;
  Conveyor pipeline(&transport);
  transport.pipeline_ptr = &pipeline;

  int count = 0;
  pipeline.addLast<AnyStage<1>>(/*on_inbound_injected=*/[](auto) {},
                                /*on_outbound_injected=*/[](auto) {},
                                /*on_added_injected=*/
                                [&](auto s) {
                                  EXPECT_EQ(s, 1);
                                  ++count;
                                });

  EXPECT_EQ(count, 1);
}
