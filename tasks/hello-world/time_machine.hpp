#pragma once

#include <cstdint>

namespace back_to_the_future {

class Delorean {
public:
  [[nodiscard]] uint32_t computeTimeTravelSpeed() const;
};

} // namespace back_to_the_future
