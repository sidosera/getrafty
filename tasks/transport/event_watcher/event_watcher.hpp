#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

#include <sys/epoll.h>
#include <sys/types.h>

#include <bits/mpsc_ring.hpp>

namespace getrafty::io {

using WatchCallback    = std::move_only_function<void()>;
using WatchCallbackPtr = std::shared_ptr<WatchCallback>;
using EpollWaitFunc = std::move_only_function<int(int, epoll_event*, int, int)>;

enum WatchFlag : uint8_t {
  RDONLY = EPOLLIN,   // 0x001
  WRONLY = EPOLLOUT,  // 0x004
};

namespace detail {
struct Semaphore {
  Semaphore();
  ~Semaphore();
  int fd_;
  std::atomic<uint64_t> counter_;
  uint64_t release(uint64_t val = 1);
  uint64_t acquire(uint64_t val = 1);
};
}  // namespace detail

struct Entry {
  std::optional<WatchCallback> read_cb;
  std::optional<WatchCallback> write_cb;
  uint32_t events = 0;

  [[nodiscard]] bool empty() const { return events == 0; }
};

template <typename T>
class FdMap {
public:
  FdMap();
  ~FdMap();

  std::pair<T&, bool> try_emplace(int fd, T&& value);
  bool erase(int fd);

  template <typename Fn>
  void forEach(Fn&& fn) {
    for (auto& [block_id, block] : *block_) {
      for (size_t offset = 0; offset < kBlockSize; ++offset) {
        int fd   = static_cast<int>((block_id * kBlockSize) + offset);
        T& entry = block[offset];
        if (!entry.is_empty()) {
          fn(fd, entry);
        }
      }
    }
  }

private:
  static constexpr size_t kBlockSize = 1024;

  [[nodiscard]] constexpr auto getBlockAndIndex(int fd) -> std::pair<int, int> {
    constexpr int kBlockSize = FdMap<T>::kBlockSize;
    return {fd / kBlockSize, fd % kBlockSize};
  }

  std::unique_ptr<std::unordered_map<int, std::array<Entry, kBlockSize>>>
      block_;
};

class EventWatcher {
public:
  explicit EventWatcher(EpollWaitFunc epoll_impl = ::epoll_wait);
  virtual ~EventWatcher();

  EventWatcher(const EventWatcher&)            = delete;
  EventWatcher& operator=(const EventWatcher&) = delete;
  EventWatcher(EventWatcher&&)                 = delete;
  EventWatcher& operator=(EventWatcher&&)      = delete;

  virtual void watch(int fd, WatchFlag flag, WatchCallback callback);
  virtual void unwatch(int fd, WatchFlag flag);
  virtual void unwatchAll();
  virtual void runInEventWatcherLoop(WatchCallback task);

  void loop(int timeout_ms = -1);

private:
  void handleLookupWatchModeChange(Entry& entry, int fd, int op) const;
  void doWatch(int fd, WatchFlag flag, WatchCallback callback);
  void doUnwatch(int fd, WatchFlag flag);

  int epoll_fd_;
  detail::Semaphore sem_;

  bits::MPSCRing<WatchCallback, 256> task_q_;
  FdMap<Entry> fd_map_;

  EpollWaitFunc epoll_impl_;
};

}  // namespace getrafty::io
