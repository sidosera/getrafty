#include "tcp_dialer.hpp"
#include "tcp_transport.hpp"
#include <bits/util.hpp>
#include <bits/ttl/logger.hpp>
#include <unistd.h>
#include <cerrno>

using getrafty::io::WRONLY;

TcpDialer::TcpDialer(const std::string& address, EventWatcher& ew)
    : address_(address), ew_(ew) {
}

TcpDialer::~TcpDialer() {
  if (fd_ >= 0) {
    ew_.unwatch(fd_, WRONLY);
    ::close(fd_);
  }
}

void TcpDialer::dial() {
  TTL_LOG(Debug) << "dial()";

  auto parsed = bits::parseAddress(address_);
  if (!parsed) {
    TTL_LOG(Error) << "parseAddress(" << address_ << ") : ERR = " << EINVAL;
    on_failure_(EINVAL);
    return;
  }

  const auto& [host, port] = *parsed;

  fd_ = bits::makeSockTcp();
  if (fd_ < 0) {
    const int err = errno;
    TTL_LOG(Error) << "makeSockTcp() : ERR = " << err;
    on_failure_(err);
    return;
  }
  TTL_LOG(Debug) << "makeSockTcp() : OK = " << fd_;

  if (bits::setSockOptNonBlocking(fd_) < 0) {
    const int err = errno;
    TTL_LOG(Error) << "setSockOptNonBlocking(" << fd_ << ") : ERR = " << err;
    ::close(fd_);
    fd_ = -1;
    on_failure_(err);
    return;
  }

  TTL_LOG(Debug) << "sockConnect(" << fd_ << ", " << host << ":" << port << ")";
  int result = bits::sockConnect(fd_, port, host);

  if (result == 0) {
    TTL_LOG(Debug) << "sockConnect() : OK";
    onWritable();
  } else if (errno == EINPROGRESS) {
    TTL_LOG(Debug) << "sockConnect() : EINPROGRESS";
    ew_.watch(fd_, WRONLY, [this]() { this->onWritable(); });
  } else {
    const int err = errno;
    TTL_LOG(Error) << "sockConnect() : ERR = " << err;
    ::close(fd_);
    fd_ = -1;
    on_failure_(err);
  }
}

void TcpDialer::onWritable() {
  TTL_LOG(Debug) << "onWritable()";

  ew_.unwatch(fd_, WRONLY);

  int error = bits::getSockOptError(fd_);

  if (error != 0) {
    TTL_LOG(Error) << "getSockOptError(" << fd_ << ") : ERR = " << error;
    ::close(fd_);
    fd_ = -1;
    on_failure_(error);
    return;
  }

  TTL_LOG(Debug) << "sockConnect() : OK";
  auto transport = std::make_unique<TcpTransport>(fd_, &ew_);
  fd_ = -1;

  on_connected_(std::move(transport));
}