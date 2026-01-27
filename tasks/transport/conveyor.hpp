#pragma once
#include "transport.hpp"

class Conveyor {
  const ITransport* transport_;

 public:
  explicit Conveyor(const ITransport& t) : transport_(&t) {}

  [[nodiscard]] const ITransport& transport() const { return *transport_; }
};
