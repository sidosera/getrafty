#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>

#include <sys/epoll.h>
#include <sys/types.h>

#include <bits/mpsc_ring.hpp>

namespace getrafty::io {

class EventWatcher;

using EpollWaitFunc = std::move_only_function<int(int, epoll_event*, int, int)>;
using WatchCallback = std::move_only_function<void()>;
using WatchCallbackPtr = std::shared_ptr<WatchCallback>;

enum WatchFlag : uint8_t {
  RDONLY = EPOLLIN,   // (0x001)
  WRONLY = EPOLLOUT,  // (0x004)
};

namespace detail {

struct EventFd {
  EventFd();
  ~EventFd();

  void clear() const;
  void signal() const;
  int eventfd_;
};

struct WatchEntry {
  std::array<std::optional<WatchCallback>, 2> cb_{std::nullopt, std::nullopt};
  // equivalent to epoll events mask.
  int bitmask_{0};
};

// Block-based sparse array for watch entries.
//
// Unlike direct fd -> callback map this implementation uses a two-level structure.
// It utilizes the fact FDs in OSes are often allocated sequentially
// so we can pre-allocate memory for big chunk of FDs at once.
//
class FdWatchMap {
public:
  constexpr static size_t kBlockSize = 1 << 6;  // (64)

  FdWatchMap();
  ~FdWatchMap();

  std::pair<WatchEntry&, bool> try_emplace(int fd, WatchEntry&& value);

  void forEach(std::move_only_function<void(int)> fn) const;

private:
  std::unordered_map<int, std::array<WatchEntry, kBlockSize>> block_;
};

}  // namespace detail

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

  void loop(int timeout_ms = -1 /* no timeout */);

private:
  constexpr static int kQLimit = 1 << 10;  // (1024)

  void handleWatch(int fd, WatchFlag flag, WatchCallback callback);
  void handleUnwatch(int fd, WatchFlag flag);
  void handleFdWatchModeChange(detail::WatchEntry& entry, int fd, int op) const;

  detail::EventFd event_;
  detail::FdWatchMap fd_map_;
  bits::MPSCRing<WatchCallback, kQLimit> task_q_;

  int epoll_fd_;
  EpollWaitFunc epoll_impl_;
};

}  // namespace getrafty::io
