#pragma once
#include <string>
#include "byte_buffer.hpp"
#include "conveyor.hpp"
#include "event_watcher/event_watcher.hpp"
#include "transport.hpp"

class AbstractEventLoopTransport : public ITransport {
  using EventWatcher = getrafty::io::EventWatcher;

public:
  explicit AbstractEventLoopTransport(int fd, EventWatcher* ew);
  ~AbstractEventLoopTransport() override;

  Conveyor& pipeline() noexcept override { return pipeline_; }

  int write(ByteBuf& buf) override;
  void flush() override;
  void onFlushCompleted(F<void()> cb) override;
  void onWritableStateChanged(F<void(bool)> cb) override;
  void readableStateChanged(bool readable) override;
  void shutdown(int how) override;
  void onShutdownCompleted(F<void(int)> cb) override;

  [[nodiscard]] const char* otherEndpoint() const override {
    return other_endpoint_.c_str();
  }

  [[nodiscard]] const char* selfEndpoint() const override {
    return self_endpoint_.c_str();
  }

protected:
  virtual int readSome(ByteBuf& dst, size_t max_bytes)  = 0;
  virtual int writeSome(ByteBuf& src, size_t max_bytes) = 0;

  int fd_;
  std::string self_endpoint_;
  std::string other_endpoint_;

private:
  void onReadable();
  void onWritable();

  Conveyor pipeline_;
  EventWatcher* ew_;

  ByteBuf wbuf_;

  F<void()> on_flush_completed_ = []() {
  };
  F<void(bool)> on_writable_state_changed_ = [](bool) {
  };
  F<void(int)> on_shutdown_completed_ = [](int) {
  };
};
