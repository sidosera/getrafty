#include "chunker.hpp"

#include <bits/ttl/logger.hpp>
#include <cerrno>

#include "transport.hpp"

namespace {
constexpr size_t kChunkLimit = 1 << 20;  // 1MB.
}

void Chunker::onInbound(StageContext& ctx, InboundBytes& evt) {
  size_t n = evt.buf.readableBytes();
  in_.write(std::move(evt.buf), n);

  while (true) {
    if (pending_ == 0) {
      if (!in_.readUI32(pending_)) {
        break;
      }

      if (pending_ > kChunkLimit) {
        TTL_LOG(Error) << "Chunk size " << pending_ << " exceeds limit of "
                       << kChunkLimit;
        in_      = ByteBuf{};
        pending_ = 0;
        ctx.failure(-EMSGSIZE);
        return;
      }
    }

    if (in_.readableBytes() < pending_) {
      break;
    }

    ByteBuf payload;
    if (pending_ > 0) {
      in_.read(payload, pending_);
    }
    pending_ = 0;

    InboundBytes chunk{std::move(payload)};
    ctx.fireInbound(chunk);
  }
}

void Chunker::onOutbound(StageContext& ctx, OutboundBytes& evt) /*NOLINT*/ {
  const auto len = evt.buf.readableBytes();

  if (len > kChunkLimit) {
    TTL_LOG(Error) << "Chunk size " << len << " exceeds limit of "
                   << kChunkLimit;
    ctx.failure(-EMSGSIZE);
    return;
  }

  ByteBuf out;
  if (!out.writeUI32(len)) {
    ctx.failure(-ENOMEM);
    return;
  }

  out.write(std::move(evt.buf), len);

  OutboundBytes chunk{std::move(out)};
  ctx.fireOutbound(chunk);
}