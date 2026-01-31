#include "tcp_transport.hpp"
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <algorithm>
#include <vector>

TcpTransport::TcpTransport(int fd, EventWatcher* ew)
    : AbstractEventLoopTransport(fd, ew) {
}

int TcpTransport::readSome(ByteBuf& dst, size_t max_bytes) {
  std::vector<uint8_t> buffer(max_bytes);

  ssize_t n = ::recv(fd_, buffer.data(), max_bytes, 0);

  if (n > 0) {
    dst.writeBinary(buffer.data(), static_cast<size_t>(n));
    return static_cast<int>(n);
  }

  if (n == 0) {
    return 0;
  }

  return -1;
}

int TcpTransport::writeSome(ByteBuf& src, size_t max_bytes) {
  size_t tail = src.size() - src.offset();
  if (tail == 0) {
    return 0;
  }

  size_t to_send = std::min(tail, max_bytes);
  ssize_t n = ::send(fd_, src.to<uint8_t*>(), to_send, 0);

  if (n > 0) {
    src.seek(src.offset() + n);
    return static_cast<int>(n);
  }

  if (n == 0) {
    return 0;
  }

  return -1;
}