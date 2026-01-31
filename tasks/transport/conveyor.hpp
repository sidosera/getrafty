#pragma once
#include "transport.hpp"

// Forward declare Event
struct Event;

class Conveyor {
  ITransport* transport_;

 public:
  explicit Conveyor(ITransport* transport) : transport_(transport) {}

  [[nodiscard]] ITransport* transport() const noexcept {
    return transport_;
  }

  // Fires an inbound event from the head toward tail
  // For now, stub implementation (will add pipeline stages later)
  void fireInbound(Event& /* evt */) {
    // TODO: Propagate through pipeline stages
  }

  // Fires an outbound event from tail toward head
  // For now, stub implementation (will add pipeline stages later)
  void fireOutbound(Event& /* evt */) {
    // TODO: Propagate through pipeline stages
  }
};
