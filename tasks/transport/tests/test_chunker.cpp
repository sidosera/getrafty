#include <gtest/gtest.h>

#include <bits/ttl/ttl.hpp>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "chunker.hpp"
#include "transport.hpp"

using namespace ::testing;
using namespace std::string_literals;

namespace {

struct TestTransport : ITransport {
  Conveyor* pipeline_ptr = nullptr;
  int what_              = 0;

  Conveyor& pipeline() noexcept override { return *pipeline_ptr; }

  int write(ByteBuf& /*unused*/) override { return 0; }

  void flush() override {}

  void onFlushCompleted(F<void()> /*unused*/) override {}

  void onWritableStateChanged(F<void(bool)> /*unused*/) override {}

  void readableStateChanged(bool /*unused*/) override {}

  void shutdown(int what) override { what_ = what; }

  void onShutdownCompleted(F<void(int)> /*unused*/) override {}

  [[nodiscard]] const char* otherEndpoint() const override { return "test"; }

  [[nodiscard]] const char* selfEndpoint() const override { return "test"; }
};

ByteBuf toBuf(std::string_view data) {
  ByteBuf buf;
  if (!data.empty()) {
    buf.write(data.data(), data.size());
  }
  return buf;
}

std::string toStr(ByteBuf& buf) {
  std::string out(buf.readableBytes(), '\0');
  if (!out.empty()) {
    buf.peek(out.data(), out.size());
  }
  return out;
}

// TODO: Integrity tests, bit flips, broken format, error cases.

class ChunkerTest : public ::testing::Test {
protected:
  TestTransport transport_;
  Conveyor pipeline_{&transport_};

  void SetUp() override {
    bits::ttl::Ttl::init("discard://");
    transport_.pipeline_ptr = &pipeline_;
  }

  void TearDown() override { bits::ttl::Ttl::shutdown(); }
};

struct InboundCapture {
  std::vector<std::string>* frames;

  void onInbound(StageContext& ctx, InboundBytes& evt) /*NOLINT*/ {
    frames->push_back(toStr(evt.buf));
    ctx.fireInbound(evt);
  }
};

struct OutboundCapture {
  std::vector<ByteBuf>* frames;

  void onOutbound(StageContext& ctx, OutboundBytes& evt) /*NOLINT*/ {
    frames->push_back(std::move(evt.buf));
    ctx.fireOutbound(evt);
  }
};

}  // namespace

TEST_F(ChunkerTest, InboundOneChunk) {
  std::vector<std::string> recv;

  pipeline_.addLast<Chunker>();
  pipeline_.addLast<InboundCapture>(&recv);

  std::string chunk = "hello";

  // header
  ByteBuf h;
  h.writeUI32(chunk.size());
  InboundBytes evt{std::move(h)};
  pipeline_.fireInbound(evt);

  // data
  for (auto byte : chunk) {
    ByteBuf ch;
    ch.write(&byte, 1);
    InboundBytes evt{std::move(ch)};
    pipeline_.fireInbound(evt);
  }

  ASSERT_EQ(recv.size(), 1U);
  EXPECT_EQ(recv[0], "hello");
}

TEST_F(ChunkerTest, InboundMultiChunk) {
  std::vector<std::string> recv;

  pipeline_.addLast<Chunker>();
  pipeline_.addLast<InboundCapture>(&recv);

  for (const auto& chunk : {"hello"s, "goodby"s}) {
    // header
    ByteBuf h;
    h.writeUI32(chunk.size());
    InboundBytes evt{std::move(h)};
    pipeline_.fireInbound(evt);

    // data
    for (auto byte : chunk) {
      ByteBuf ch;
      ch.write(&byte, 1);
      InboundBytes evt{std::move(ch)};
      pipeline_.fireInbound(evt);
    }
  }

  ASSERT_EQ(recv.size(), 2U);
  EXPECT_EQ(recv[0], "hello");
  EXPECT_EQ(recv[1], "goodby");
}

TEST_F(ChunkerTest, InboundEmptyChunk) {
  std::vector<std::string> recv;

  pipeline_.addLast<Chunker>();
  pipeline_.addLast<InboundCapture>(&recv);

  ByteBuf ch;
  ch.writeUI32(0);
  InboundBytes evt{std::move(ch)};
  pipeline_.fireInbound(evt);

  ASSERT_EQ(recv.size(), 1U);
  EXPECT_EQ(recv[0], "");
}

TEST_F(ChunkerTest, OutboundOneChunk) {
  std::vector<ByteBuf> send;

  pipeline_.addLast<OutboundCapture>(&send);
  pipeline_.addLast<Chunker>();

  OutboundBytes out{toBuf("hello")};
  pipeline_.fireOutbound(out);

  ASSERT_EQ(send.size(), 1U);
  EXPECT_EQ(send[0].readableBytes(), 9U);

  uint32_t len;
  ASSERT_TRUE(send[0].readUI32(len));
  EXPECT_EQ(len, 5U);
  EXPECT_EQ(toStr(send[0]), "hello");
}

TEST_F(ChunkerTest, OutboundEmptyChunk) {
  std::vector<ByteBuf> send;

  pipeline_.addLast<OutboundCapture>(&send);
  pipeline_.addLast<Chunker>();

  OutboundBytes out{toBuf("")};
  pipeline_.fireOutbound(out);

  ASSERT_EQ(send.size(), 1U);
  EXPECT_EQ(send[0].readableBytes(), 4U);

  uint32_t len;
  ASSERT_TRUE(send[0].readUI32(len));
  EXPECT_EQ(len, 0U);
}

TEST_F(ChunkerTest, InboundChunkLimit) {
  pipeline_.addLast<Chunker>();

  ByteBuf wire;
  wire.writeUI32(1 << 21 /*2MB*/);
  InboundBytes evt{std::move(wire)};
  pipeline_.fireInbound(evt);

  EXPECT_EQ(transport_.what_, -EMSGSIZE);
}

TEST_F(ChunkerTest, OutboundChunkLimit) {
  pipeline_.addLast<Chunker>();

  std::string large_payload(1 << 21 /*2MB*/, 'x');
  OutboundBytes out{toBuf(large_payload)};
  pipeline_.fireOutbound(out);

  EXPECT_EQ(transport_.what_, -EMSGSIZE);
}
