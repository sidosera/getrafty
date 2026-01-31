#include "event_watcher.hpp"

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <bits/ttl/logger.hpp>
#include <bits/util.hpp>
#include <cerrno>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include "panic.hpp"

namespace getrafty::io {

template <typename T>
FdMap<T>::FdMap()
    : block_(std::make_unique<
             std::unordered_map<int, std::array<T, kBlockSize>>>()) {}

template <typename T>
FdMap<T>::~FdMap() = default;

template <typename T>
std::pair<T&, bool> FdMap<T>::try_emplace(int fd, T&& value) {
  auto [block_id, offset] = getBlockAndIndex(fd);

  auto& block = (*block_)[block_id];
  T& entry    = block[offset];

  const bool inserted = entry.empty();
  if (inserted) {
    entry = std::move(value);
  }

  return {entry, inserted};
}

template <typename T>
bool FdMap<T>::erase(int fd) {
  auto [block_id, offset] = getBlockAndIndex(fd);

  auto it = block_->find(block_id);
  if (it == block_->end()) {
    return false;
  }

  T& entry = it->second[offset];
  if (entry.empty()) {
    return false;
  }

  entry = T{};
  return true;
}

// Explicit template instantiation
template class FdMap<Entry>;

namespace {
static thread_local EventWatcher* curr_ = nullptr;  // NOLINT

struct EventWatcherThreadScopeGuard {
  explicit EventWatcherThreadScopeGuard(EventWatcher* ew) noexcept
      : prev_(std::exchange(curr_, ew)) {}

  ~EventWatcherThreadScopeGuard() { curr_ = prev_; }

  EventWatcherThreadScopeGuard(const EventWatcherThreadScopeGuard&) = delete;
  EventWatcherThreadScopeGuard& operator=(const EventWatcherThreadScopeGuard&) =
      delete;

private:
  EventWatcher* prev_;
};

[[nodiscard]] EventWatcher* currentEventWatcher() noexcept {
  return curr_;
}

void handleCallback(WatchCallback& task) noexcept {
  try {
    task();
  } catch (const std::exception& ex) {
    TTL_LOG(Error) << "Unhandled exception: " << ex.what();
  } catch (...) {
    TTL_LOG(Error) << "Unhandled exception";
  }
}

constexpr int kMaxTaskBatch    = 10;
constexpr int kMaxEpollRetries = 10;
constexpr size_t kMaxEvents    = 64;

}  // namespace

namespace detail {
Semaphore::Semaphore() : fd_(bits::makeEventFd()), counter_(0) {}

Semaphore::~Semaphore() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

uint64_t Semaphore::release(uint64_t val) {
  uint64_t prev = counter_.fetch_add(val, std::memory_order_release);
  if (prev == 0) {
    bits::eventFdWrite(fd_);
  }
  return prev;
}

uint64_t Semaphore::acquire(uint64_t val) {
  if (val == 0) {
    return 0;
  }

  uint64_t curr = counter_.load(std::memory_order_acquire);
  while (curr != 0) {
    uint64_t limit = std::min(curr, val);
    if (counter_.compare_exchange_weak(curr, curr - limit,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {

      if (curr == limit) {
        bits::eventFdRead(fd_);
        if (counter_.load(std::memory_order_acquire) != 0) {
          bits::eventFdWrite(fd_);
        }
      }
      return limit;
    }
  }
  return 0;
}
}  // namespace detail

EventWatcher::EventWatcher(EpollWaitFunc epoll_impl)
    : epoll_fd_(bits::makeEpoll()), epoll_impl_{std::move(epoll_impl)} {
  doWatch(sem_.fd_, RDONLY, [this]() {
    TTL_LOG(Trace) << "wakeup(" << sem_.fd_ << ")";
    uint64_t i = 0;
    for (; i < kMaxTaskBatch; ++i) {
      auto task = task_q_.tryPop();
      if (!task.has_value()) {
        break;
      }
      handleCallback(*task);
    }
    sem_.acquire(i);
  });
}

EventWatcher::~EventWatcher() {
  unwatchAll();
  if (::close(epoll_fd_) == -1) {
    bits::panic("close failed");
  }
}

void EventWatcher::handleLookupWatchModeChange(Entry& entry, int fd,
                                               int op) const {
  epoll_event event{};
  event.data.fd = fd;
  event.events  = entry.events;

  if (::epoll_ctl(epoll_fd_, op, fd, &event) == -1) {
    TTL_LOG(Error) << "epoll_ctl(" << epoll_fd_ << "," << op << "," << fd
                   << ",*) : ERR = " << errno;
  }
}

void EventWatcher::doWatch(int fd, WatchFlag flag, WatchCallback callback) {
  auto [entry, _] = fd_map_.try_emplace(fd, Entry{});

  const uint32_t prev_events = entry.events;
  entry.events |= flag;

  std::optional<WatchCallback>* slot = nullptr;
  if (flag == RDONLY) {
    slot = &entry.read_cb;
  } else {
    slot = &entry.write_cb;
  }

  if (std::exchange(*slot, std::move(callback)) == std::nullopt) {
    int op = EPOLL_CTL_MOD;
    if (prev_events == 0) {
      op = EPOLL_CTL_ADD;
    }
    handleLookupWatchModeChange(entry, fd, op);
  }
}

void EventWatcher::watch(const int fd, const WatchFlag flag,
                         WatchCallback callback) {
  TTL_LOG(Trace) << "watch(" << fd << "," << static_cast<int>(flag) << ")";

  if (fd < 0) {
    TTL_LOG(Error) << "watch() called with negative fd: " << fd;
    return;
  }

  runInEventWatcherLoop([this, fd, flag, cb = std::move(callback)]() mutable {
    doWatch(fd, flag, std::move(cb));
  });
}

void EventWatcher::doUnwatch(int fd, WatchFlag flag) {
  auto [entry, _] = fd_map_.try_emplace(fd, Entry{});

  std::optional<WatchCallback>* slot = nullptr;
  if (flag == RDONLY) {
    slot = &entry.read_cb;
  } else {
    slot = &entry.write_cb;
  }

  entry.events &= ~flag;

  if (std::exchange(*slot, std::nullopt) != std::nullopt) {
    int op = EPOLL_CTL_MOD;
    if (entry.events == 0) {
      op = EPOLL_CTL_DEL;
    }
    handleLookupWatchModeChange(entry, fd, op);
  }
}

void EventWatcher::unwatch(const int fd, const WatchFlag flag) {
  TTL_LOG(Trace) << "unwatch(" << fd << "," << static_cast<int>(flag) << ")";

  if (fd < 0) {
    TTL_LOG(Error) << "unwatch() called with negative fd: " << fd;
    return;
  }

  runInEventWatcherLoop([this, fd, flag] { doUnwatch(fd, flag); });
}

void EventWatcher::unwatchAll() {
  runInEventWatcherLoop([this]() {
    fd_map_.forEach([this](int fd, Entry& entry) {
      if (::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) == -1) {
        TTL_LOG(Error) << "epoll_ctl(" << epoll_fd_ << "," << EPOLL_CTL_DEL
                       << "," << fd << ",*) : ERR = " << errno;
      }
      entry.read_cb.reset();
      entry.write_cb.reset();
    });
  });
}

void EventWatcher::loop(int timeout_ms) {
  TTL_LOG(Trace) << "loop(" << timeout_ms << ")";

  const EventWatcherThreadScopeGuard guard(this);

  static thread_local std::array<epoll_event, kMaxEvents> events;

  // Add retries for sporadic interrupts
  int nfds = 0;
  for (int retry = 0; retry < kMaxEpollRetries; ++retry) {
    nfds = epoll_impl_(epoll_fd_, events.data(),
                       static_cast<int>(events.size()), timeout_ms);
    if (nfds >= 0 || errno != EINTR) {
      break;
    }
    TTL_LOG(Trace) << "epoll_impl_(" << epoll_fd_ << ",*," << events.size()
                   << "," << timeout_ms << ") interrupted";
  }

  if (nfds < 0) {
    bits::panic("epoll_wait failed");
  }

  TTL_LOG(Trace) << "epoll_impl_(" << epoll_fd_ << ",*," << events.size() << ","
                 << timeout_ms << ") returned " << nfds << " events";

  for (int i = 0; i < nfds; ++i) {
    const auto& event = events[i];
    const int fd      = event.data.fd;

    auto [entry, _] = fd_map_.try_emplace(fd, Entry{});

    if ((event.events & EPOLLIN) != 0 && entry.read_cb.has_value()) {
      TTL_LOG(Trace) << "readable(" << fd << ")";
      handleCallback(entry.read_cb.value());
    }

    if ((event.events & EPOLLOUT) != 0 && entry.write_cb.has_value()) {
      TTL_LOG(Trace) << "writable(" << fd << ")";
      handleCallback(entry.write_cb.value());
    }
  }
}

void EventWatcher::runInEventWatcherLoop(WatchCallback task) {
  if (!task) {
    return;
  }

  // Inline if called from within the event watcher thread.
  if (currentEventWatcher() == this) {
    handleCallback(task);
    return;
  }

  task_q_.push(std::move(task));
  sem_.release(1);
}

}  // namespace getrafty::io
