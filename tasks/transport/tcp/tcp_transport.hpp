#pragma once
#include <tasks/transport/abstract_transport.hpp>
#include "tcp/tcp_dialer.hpp"

class TcpTransport : public AbstractEventLoopTransport {
  using EventWatcher = getrafty::io::EventWatcher;

public:
  explicit TcpTransport(int fd, EventWatcher* ew);
  ~TcpTransport() override = default;

protected:
  int readSome(ByteBuf& dst, size_t max_bytes) override;
  int writeSome(ByteBuf& src, size_t max_bytes) override;
};
