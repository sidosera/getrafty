#include "abstract_transport.hpp"
#include <unistd.h>
#include <bits/ttl/logger.hpp>
#include <bits/util.hpp>
#include <cerrno>
#include "event.hpp"

using namespace getrafty::io;

AbstractEventLoopTransport::AbstractEventLoopTransport(int fd, EventWatcher* ew)
    : fd_(fd),
      self_endpoint_(*bits::getSockOptHostPort(fd)),
      other_endpoint_(*bits::getPeerHostPort(fd)),
      pipeline_(this),
      ew_(ew) {}

AbstractEventLoopTransport::~AbstractEventLoopTransport() {
  ew_->unwatch(fd_, WatchFlag::RDONLY);
  ew_->unwatch(fd_, WatchFlag::WRONLY);
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

void AbstractEventLoopTransport::onReadable() {
  TTL_LOG(Debug) << "onReadable() : fd=" << fd_;

  ByteBuf buf{};
  int n;
  while ((n = readSome(buf, 4096)) > 0) {
    TTL_LOG(Debug) << "readSome(*, " << 4096 << ") : OK = " << n;
  }

  if (buf.readableBytes() > 0) {
    TTL_LOG(Debug) << "fireInbound(InboundBytes) : " << buf.readableBytes()
                   << " bytes";
    InboundBytes evt{std::move(buf)};
    pipeline_.fireInbound(evt);
  }

  if (n == EAGAIN) {
    TTL_LOG(Debug) << "readSome() : EAGAIN";
    return;
  }

  if (n == EWOULDBLOCK) {
    TTL_LOG(Debug) << "readSome() : EWOULDBLOCK";
    return;
  }

  if (n == 0) {
    TTL_LOG(Info) << "Connection closed by peer fd=" << fd_;
    InboundTransportInactive evt;
    pipeline_.fireInbound(evt);
    return;
  }

  TTL_LOG(Error) << "readSome() : error=" << n;
  InboundTransportError evt{n};
  pipeline_.fireInbound(evt);
}

void AbstractEventLoopTransport::onWritable() {
  TTL_LOG(Debug) << "onWritable() fd=" << fd_
                 << " wbuf_size=" << wbuf_.readableBytes();

  for (int i = 0; i < 256 && wbuf_.readableBytes() > 0; ++i) {
    int written = writeSome(wbuf_, wbuf_.readableBytes());

    if (written > 0) {
      TTL_LOG(Debug) << "writeSome() : " << written << " bytes";
      continue;
    }

    if (written < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        TTL_LOG(Debug) << "writeSome() : EAGAIN/EWOULDBLOCK";
        return;
      }

      TTL_LOG(Error) << "writeSome() : errno=" << errno;
      ew_->unwatch(fd_, WatchFlag::WRONLY);
      InboundTransportError evt{errno};
      pipeline_.fireInbound(evt);
      return;
    }
  }

  // Empty
  TTL_LOG(Debug) << "Write buffer empty, unwatching WRONLY fd=" << fd_;
  ew_->unwatch(fd_, WatchFlag::WRONLY);
  on_flush_completed_();
}

int AbstractEventLoopTransport::write(ByteBuf& buf) {
  if (buf.readableBytes() == 0) {
    return 0;
  }

  const size_t size = buf.readableBytes();
  TTL_LOG(Debug) << "write() fd=" << fd_ << " bytes=" << size;

  // Append buffer (move semantics)
  wbuf_.write(std::move(buf), size);

  // Watch for writable events
  ew_->watch(fd_, WatchFlag::WRONLY, [this]() { this->onWritable(); });

  return static_cast<int>(size);
}

void AbstractEventLoopTransport::flush() {
  if (wbuf_.readableBytes() > 0) {
    ew_->watch(fd_, WatchFlag::WRONLY, [this]() { this->onWritable(); });
  }
}

void AbstractEventLoopTransport::onFlushCompleted(F<void()> cb) {
  on_flush_completed_ = std::move(cb);
}

void AbstractEventLoopTransport::onWritableStateChanged(F<void(bool)> cb) {
  on_writable_state_changed_ = std::move(cb);
}

void AbstractEventLoopTransport::readableStateChanged(bool readable) {
  TTL_LOG(Debug) << "readableStateChanged(" << readable << ") fd=" << fd_;

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
