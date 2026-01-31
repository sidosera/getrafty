#include "tcp_listener.hpp"
#include "tcp_transport.hpp"

#include <unistd.h>

#include <cerrno>

#include "bits/util.hpp"
#include "bits/ttl/logger.hpp"

using getrafty::io::RDONLY;

TcpListener::TcpListener(const std::string& address,
                         EventWatcher& ew)
    : address_(address), ew_(ew) {}

TcpListener::~TcpListener() {
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
}

void TcpListener::listenAndWait() {
  TTL_LOG(Debug) << "listenAndWait()";

  auto parsed = bits::parseAddress(address_);
  if (!parsed) {
    TTL_LOG(Error) << "parseAddress(" << address_ << ") ERR = " << errno;
    ew_.runInEventWatcherLoop([this]() {
      on_failure_(EINVAL);
      on_shutdown_();
      shutdown_latch_.count_down();
    });
    shutdown_latch_.wait();
    return;
  }

  listen_fd_ = bits::makeSockTcp();
  if (listen_fd_ < 0) {
    const int err = errno;
    TTL_LOG(Error) << "makeSockTcp() : ERR = " << err;
    ew_.runInEventWatcherLoop([this, err]() {
      on_failure_(err);
      on_shutdown_();
      shutdown_latch_.count_down();
    });
    shutdown_latch_.wait();
    return;
  }
  TTL_LOG(Debug) << "makeSockTcp() : OK = " << listen_fd_;

  if (bits::setSockOptNonBlocking(listen_fd_) < 0) {
    const int err = errno;
    ::close(listen_fd_);
    listen_fd_ = -1;
    ew_.runInEventWatcherLoop([this, err]() {
      if (on_failure_) {
        on_failure_(err);
      }
      if (on_shutdown_) {
        on_shutdown_();
      }
      shutdown_latch_.count_down();
    });
    shutdown_latch_.wait();
    return;
  }

  bits::setSockOptShared(listen_fd_);

  if (bits::sockBind(listen_fd_, parsed->second, parsed->first) < 0) {
    const int err = errno;
    TTL_LOG(Error) << "sockBind(" << listen_fd_ << ") : ERR = " << err;
    ::close(listen_fd_);
    listen_fd_ = -1;
    ew_.runInEventWatcherLoop([this, err]() {
      on_failure_(err);
      on_shutdown_();
      shutdown_latch_.count_down();
    });
    shutdown_latch_.wait();
    return;
  }
  TTL_LOG(Debug) << "sockBind(" << listen_fd_ << ") : OK = " << parsed->first << ":" << parsed->second;

  if (bits::sockListen(listen_fd_) < 0) {
    const int err = errno;
    TTL_LOG(Error) << "sockListen(" << listen_fd_ << "): ERR =" << err;
    ::close(listen_fd_);
    listen_fd_ = -1;
    ew_.runInEventWatcherLoop([this, err]() {
      on_failure_(err);
      on_shutdown_();
      shutdown_latch_.count_down();
    });
    shutdown_latch_.wait();
    return;
  }

  const auto bound = bits::getSockOptHostPort(listen_fd_);
  if (!bound) {
    const int err = errno;
    TTL_LOG(Error) << "getSockOptHostPort(" << listen_fd_ << ") : errno=" << err;
    ::close(listen_fd_);
    listen_fd_ = -1;
    ew_.runInEventWatcherLoop([this, err]() {
      on_failure_(err);
      on_shutdown_();
      shutdown_latch_.count_down();
    });
    shutdown_latch_.wait();
    return;
  }

  bound_address_ = *bound;
  TTL_LOG(Debug) << "sockListen(" << listen_fd_ << ") : OK = " << bound_address_;

  ew_.runInEventWatcherLoop([this, address = bound_address_]() {
    TTL_LOG(Debug) << "on_started_(" << address << ")";
    on_started_(address);
  });

  ew_.runInEventWatcherLoop([this]() {
    TTL_LOG(Debug) << "watch(" << listen_fd_ << ", RDONLY)";
    ew_.watch(listen_fd_, RDONLY, [this]() { onReadable(); });
  });

  shutdown_latch_.wait();
}

void TcpListener::shutdown() {
  ew_.runInEventWatcherLoop([this]() {
    if (listen_fd_ >= 0) {
      ew_.unwatch(listen_fd_, RDONLY);
      ::close(listen_fd_);
      listen_fd_ = -1;
    }
    if (on_shutdown_) {
      on_shutdown_();
    }
    shutdown_latch_.count_down();
  });
}

void TcpListener::onReadable() {
  TTL_LOG(Debug) << "onReadable()";

  for (auto i = 0; i < 256; ++i) {
    int client_fd = bits::sockAccept(listen_fd_);

    if (client_fd < 0) {
      if (errno == EINTR) {
        TTL_LOG(Debug) << "sockAccept() : EINTR, retrying";
        continue;
      }

      if (errno == EMFILE || errno == ENFILE || errno == ENOMEM || errno == EAGAIN || errno == EWOULDBLOCK) {
        TTL_LOG(Debug) << "sockAccept() : errno=" << errno << " (transient)";
        on_error_(errno);
        return;
      }
      TTL_LOG(Error) << "sockAccept() : ERR = " << errno;
      on_failure_(errno);
      return;
    }

    TTL_LOG(Debug) << "sockAccept() : OK = " << client_fd;
    auto transport = createTransport(client_fd);
    on_accepted_(std::move(transport));
  }
}

std::unique_ptr<ITransport> TcpListener::createTransport(int client_fd) {
  TTL_LOG(Debug) << "createTransport(" << client_fd << ")";

  if (bits::setSockOptNonBlocking(client_fd) < 0) {
    const int err = errno;
    TTL_LOG(Error) << "setSockOptNonBlocking(" << client_fd << ") : ERR = " << err;
    ::close(client_fd);
    return nullptr;
  }

  TTL_LOG(Debug) << "createTransport(" << client_fd << ") : OK";
  return std::make_unique<TcpTransport>(client_fd, &ew_);
}
