#pragma once
#include "byte_buffer.hpp"

using type_id_t = const void*;

template <class T>
constexpr type_id_t type_id() noexcept {
  static const int tag{};
  return &tag;
}

struct Event {
  virtual ~Event() = default;
  virtual type_id_t type() const noexcept = 0;
};

// CRTP
template <class Derived>
struct EventBase : Event {
  static constexpr type_id_t static_type() noexcept {
    return type_id<Derived>();
  }
  type_id_t type() const noexcept override { return static_type(); }
};

// Transport lifecycle events
struct InboundTransportActive : EventBase<InboundTransportActive> {};
struct InboundTransportInactive : EventBase<InboundTransportInactive> {};
struct InboundTransportError : EventBase<InboundTransportError> {
  int err;
  explicit InboundTransportError(int e) : err(e) {}
};

// Data events
struct InboundBytes : EventBase<InboundBytes> {
  ByteBuf buf;
  explicit InboundBytes(ByteBuf&& b) : buf(std::move(b)) {}
};

struct OutboundBytes : EventBase<OutboundBytes> {
  ByteBuf buf;
  explicit OutboundBytes(ByteBuf&& b) : buf(std::move(b)) {}
};

// Flow control events
struct InboundSuspend : EventBase<InboundSuspend> {};
struct InboundResume : EventBase<InboundResume> {};
struct OutboundSuspend : EventBase<OutboundSuspend> {};
struct OutboundResume : EventBase<OutboundResume> {};

struct OutboundClose : EventBase<OutboundClose> {
  int reason;
  explicit OutboundClose(int r = 0) : reason(r) {}
};