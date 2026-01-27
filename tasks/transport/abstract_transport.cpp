#include "abstract_transport.hpp"
#include <unistd.h>
#include <bits/util.hpp>
#include <cerrno>

using namespace getrafty::io;

AbstractEventLoopTransport::AbstractEventLoopTransport(int fd, EventWatcher* ew)
    : fd_(fd),
      self_endpoint_(*bits::getSockOptHostPort(fd)),
      other_endpoint_(*bits::getPeerHostPort(fd)),
      pipeline_(*this),
      ew_(ew) {}

AbstractEventLoopTransport::~AbstractEventLoopTransport() {
  ew_->unwatch(fd_, WatchFlag::RDONLY);
  ew_->unwatch(fd_, WatchFlag::WRONLY);
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

void AbstractEventLoopTransport::onReadable() {
  // TODO
}

void AbstractEventLoopTransport::onWritable() {
  // TODO
}

int AbstractEventLoopTransport::write(ByteBuf& buf) {
  // TODO
}

void AbstractEventLoopTransport::flush() {
  // TODO
}

void AbstractEventLoopTransport::onFlushCompleted(F<void()> cb) {
  on_flush_completed_ = std::move(cb);
}

void AbstractEventLoopTransport::onWritableStateChanged(F<void(bool)> cb) {
  on_writable_state_changed_ = std::move(cb);
}

void AbstractEventLoopTransport::readableStateChanged(bool readable) {
  if (readable) {
    ew_->watch(fd_, WatchFlag::RDONLY, [this]() { this->onReadable(); });
  } else {
    ew_->unwatch(fd_, WatchFlag::RDONLY);
  }
}

void AbstractEventLoopTransport::shutdown(int how) {
  ::shutdown(fd_, how);
}

void AbstractEventLoopTransport::onShutdownCompleted(F<void(int)> cb) {
  on_shutdown_completed_ = std::move(cb);
}
