#pragma once
#include <sys/uio.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// ByteBuf is a sequential stream buffer for I/O.
// TODO: Zero-copy implementation with fixed-size segments (see README.md).
// TODO: ZigZag encoding for endianess independent storage.
// TODO: Helper methods to read/write/peek of primitive types such as uint32, string, etc.
class ByteBuf {
public:
  ByteBuf() = default;

  // Returns total readable bytes.
  [[nodiscard]] size_t readableBytes() const noexcept;

  // Returns currently reserved writable bytes (no new allocation).
  [[nodiscard]] size_t writableBytes() const noexcept;

  // Best-effort read:
  // Transfers up to n bytes from this into dst.
  // Bytes are removed from this and appended to dst.
  // Returns number of bytes transferred.
  size_t read(ByteBuf& dst, size_t n) noexcept;

  // Best-effort peek:
  // Copies up to n bytes into dst, no state change.
  size_t peek(void* dst, size_t n) const noexcept;

  // Appends raw bytes to the tail; grows the buffer.
  // Returns false on allocation failure.
  bool write(const void* src, size_t n) noexcept;

  // Transfer n bytes from src.
  // Transfers up to n bytes (min(n, src.readableBytes())).
  void write(ByteBuf&& src, size_t n) noexcept;

  // Reserve n writable bytes (may allocate new segments).
  // Returns false on allocation failure.
  bool reserve(size_t n) noexcept;

  // View of available writable bytes (tailroom) for readv.
  // Does not allocate; caller must reserve() first.
  // <T> can be iovec or a custom scatter/gather type.
  // Valid until the buffer is mutated.
  template <typename T>
  T tailroom() noexcept;

  // View of available readable bytes (headroom) for writev.
  // <T> can be iovec or a custom scatter/gather type.
  // Valid until the buffer is mutated.
  template <typename T>
  T headroom() noexcept;

  // Drop up to n readable bytes from the head.
  // Returns bytes dropped (<= n).
  size_t advance(size_t n);

  // Make up to n bytes from tailroom readable.
  // Returns bytes committed (<= n).
  size_t commit(size_t n);

  bool readUI32(uint32_t& out) noexcept;

  bool writeUI32(uint32_t val) noexcept;

private:
  size_t r_ = 0;
  size_t w_ = 0;
  std::vector<uint8_t> data_;
};

inline size_t ByteBuf::readableBytes() const noexcept {
  return w_ - r_;
}

inline size_t ByteBuf::writableBytes() const noexcept {
  return data_.size() - w_;
}

inline size_t ByteBuf::read(ByteBuf& dst, size_t n) noexcept {
  if (this == &dst) {
    return 0;
  }

  const size_t m = std::min(n, readableBytes());
  if (m == 0 || !dst.reserve(m)) {
    return 0;
  }

  std::memcpy(dst.data_.data() + dst.w_, data_.data() + r_, m);
  dst.w_ += m;
  advance(m);
  return m;
}

inline size_t ByteBuf::peek(void* dst, size_t n) const noexcept {
  const size_t m = std::min(n, readableBytes());
  if (m == 0) {
    return 0;
  }
  std::memcpy(dst, data_.data() + r_, m);
  return m;
}

inline bool ByteBuf::write(const void* src, size_t n) noexcept {
  if (n == 0) {
    return true;
  }
  if (!reserve(n)) {
    return false;
  }

  std::memcpy(data_.data() + w_, src, n);
  w_ += n;
  return true;
}

inline void ByteBuf::write(ByteBuf&& src, size_t n) noexcept {
  const size_t m = std::min(n, src.readableBytes());
  if (m == 0 || !reserve(m)) {
    return;
  }

  std::memcpy(data_.data() + w_, src.data_.data() + src.r_, m);
  w_ += m;
  src.advance(m);
}

inline bool ByteBuf::reserve(size_t n) noexcept {
  if (writableBytes() >= n) {
    return true;
  }

  // Simple impl: no compaction, just grow.
  try {
    data_.resize(w_ + n);
    return true;
  } catch (...) {
    return false;
  }
}

inline size_t ByteBuf::advance(size_t n) {
  const size_t m = std::min(n, readableBytes());
  r_ += m;
  if (r_ == w_) {
    r_ = w_ = 0;
  }
  return m;
}

inline size_t ByteBuf::commit(size_t n) {
  const size_t m = std::min(n, writableBytes());
  w_ += m;
  return m;
}

inline bool ByteBuf::readUI32(uint32_t& out) noexcept {
  if (readableBytes() < 4) {
    return false;
  }

  uint8_t buf[4];
  peek(buf, 4);
  advance(4);

  out = buf[0] | (static_cast<uint32_t>(buf[1]) << 8) |
        (static_cast<uint32_t>(buf[2]) << 16) |
        (static_cast<uint32_t>(buf[3]) << 24);
  return true;
}

inline bool ByteBuf::writeUI32(uint32_t val) noexcept {
  uint8_t buf[4] = {
      static_cast<uint8_t>(val),
      static_cast<uint8_t>(val >> 8),
      static_cast<uint8_t>(val >> 16),
      static_cast<uint8_t>(val >> 24),
  };
  return write(buf, 4);
}

template <>
inline std::vector<iovec> ByteBuf::tailroom<std::vector<iovec>>() noexcept {
  std::vector<iovec> out;
  const size_t n = writableBytes();
  if (n == 0) {
    return out;
  }

  iovec v{};
  v.iov_base = static_cast<void*>(data_.data() + w_);
  v.iov_len  = n;

  out.push_back(v);
  return out;
}

template <>
inline std::vector<iovec> ByteBuf::headroom<std::vector<iovec>>() noexcept {
  std::vector<iovec> out;
  const size_t n = readableBytes();
  if (n == 0) {
    return out;
  }

  iovec v{};
  v.iov_base = static_cast<void*>(data_.data() + r_);
  v.iov_len  = n;

  out.push_back(v);
  return out;
}
