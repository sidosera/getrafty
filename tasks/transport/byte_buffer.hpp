#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <type_traits>
#include <vector>


class ByteBuf {
 public:
  ByteBuf() = default;

  [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }

  [[nodiscard]] bool empty() const noexcept { return data_.empty(); }

  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

  void seek(std::size_t off) {
    offset_ = std::min(off, data_.size());
  }

  int readBinary(void* dst, std::size_t n) noexcept {
    std::size_t available = data_.size() - offset_;
    std::size_t to_read = std::min(n, available);
    if (to_read > 0) {
      std::memcpy(dst, data_.data() + offset_, to_read);
      offset_ += to_read;
    }
    return static_cast<int>(to_read);
  }

  bool writeBinary(const void* src, std::size_t n) noexcept {
    const auto* bytes = static_cast<const uint8_t*>(src);
    data_.insert(data_.end(), bytes, bytes + n);
    offset_ += n;
    return true;
  }

  template <class T>
  T to() const noexcept {
    if constexpr (std::is_same_v<T, uint8_t*>) {
      return const_cast<uint8_t*>(data_.data() + offset_);
    } else if constexpr (std::is_same_v<T, const uint8_t*>) {
      return data_.data() + offset_;
    }
  }

  [[nodiscard]] ByteBuf slice(std::size_t off, std::size_t len) const {
    ByteBuf result;
    if (off < data_.size()) {
      std::size_t actual_len = std::min(len, data_.size() - off);
      auto begin_it = data_.begin() + static_cast<std::ptrdiff_t>(off);
      auto end_it = begin_it + static_cast<std::ptrdiff_t>(actual_len);
      result.data_.assign(begin_it, end_it);
    }
    return result;
  }

  void append(ByteBuf&& other) noexcept {
    data_.insert(data_.end(), other.data_.begin(), other.data_.end());
    offset_ += other.size();
    other.data_.clear();
    other.offset_ = 0;
  }


 private:
  std::vector<uint8_t> data_;
  std::size_t offset_ = 0;
};
