#pragma once

#include <variant>
#include "byte_buffer.hpp"

// Plain event structs (no inheritance!)

// Transport lifecycle
struct InboundTransportActive {};

struct InboundTransportInactive {};

struct InboundTransportError {
  int err;

  explicit InboundTransportError(int e) : err(e) {}
};

// Data flow
struct InboundBytes {
  ByteBuf buf;

  explicit InboundBytes(ByteBuf&& b) : buf(std::move(b)) {}
};

struct OutboundBytes {
  ByteBuf buf;

  explicit OutboundBytes(ByteBuf&& b) : buf(std::move(b)) {}
};

// Control flow
struct InboundSuspend {};

struct InboundResume {};

struct OutboundSuspend {};

struct OutboundResume {};

struct OutboundClose {
  int reason;

  explicit OutboundClose(int r = 0) : reason(r) {}
};


using Event = std::variant<
    InboundTransportActive,
    InboundTransportInactive,
    InboundTransportError,
    InboundBytes,
    InboundSuspend,
    InboundResume,
    OutboundBytes,
    OutboundClose,
    OutboundSuspend,
    OutboundResume
>;
