#pragma once

#include <cstdint>

#include "byte_buffer.hpp"
#include "conveyor.hpp"

class Chunker {
public:
  Chunker()  = default;
  ~Chunker() = default;

  void onInbound(StageContext& ctx, InboundBytes& evt);
  void onOutbound(StageContext& ctx, OutboundBytes& evt);

private:
  ByteBuf in_;
  uint32_t pending_ = 0;
};
