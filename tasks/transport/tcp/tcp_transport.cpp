#include "tcp_transport.hpp"
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <vector>

TcpTransport::TcpTransport(int fd, EventWatcher* ew)
    : AbstractEventLoopTransport(fd, ew) {}

int TcpTransport::readSome(ByteBuf& dst, size_t max_bytes) {
  std::vector<uint8_t> buffer(max_bytes);

  ssize_t n = ::recv(fd_, buffer.data(), max_bytes, 0);

  if (n > 0) {
    dst.write(buffer.data(), static_cast<size_t>(n));
    return static_cast<int>(n);
  }

  if (n == 0) {
    return 0;
  }

  return -1;
}

int TcpTransport::writeSome(ByteBuf& src, size_t max_bytes) {
  size_t tail = src.readableBytes();
  if (tail == 0) {
    return 0;
  }

  size_t to_send = std::min(tail, max_bytes);
  auto iov = src.headroom<std::vector<iovec>>();
  if (iov.empty()) {
    return 0;
  }
  size_t remaining = to_send;
  size_t count = 0;
  for (auto& entry : iov) {
    if (remaining == 0) {
      break;
    }
    if (entry.iov_len > remaining) {
      entry.iov_len = remaining;
    }
    remaining -= entry.iov_len;
    ++count;
  }
  iov.resize(count);
  ssize_t n = ::writev(fd_, iov.data(), static_cast<int>(iov.size()));

  if (n > 0) {
    src.advance(static_cast<size_t>(n));
    return static_cast<int>(n);
  }

  if (n == 0) {
    return 0;
  }

  return -1;
}
