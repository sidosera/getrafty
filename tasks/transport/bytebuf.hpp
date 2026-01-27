#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

class ByteBuf {
 public:
  ByteBuf() = default;

  [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
  [[nodiscard]] bool empty() const noexcept { return data_.empty(); }

  void append(const void* data, std::size_t len) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    data_.insert(data_.end(), bytes, bytes + len);
  }

  void clear() { data_.clear(); }

  [[nodiscard]] const uint8_t* data() const noexcept { return data_.data(); }
  [[nodiscard]] uint8_t* data() noexcept { return data_.data(); }

 private:
  std::vector<uint8_t> data_;
};
