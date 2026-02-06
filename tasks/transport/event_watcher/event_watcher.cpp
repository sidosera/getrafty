#include "event_watcher.hpp"

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/time.h>
#include <unistd.h>

#include <array>
#include <bits/ttl/logger.hpp>
#include <bits/util.hpp>
#include <cassert>
#include <cerrno>
#include <exception>
#include <functional>
#include <optional>
#include <unordered_map>
#include <utility>

namespace getrafty::io {
using detail::WatchEntry;

namespace detail {

[[nodiscard]] constexpr auto getBlockAndIndex(int fd) -> std::pair<int, int> {
  constexpr int kBlockSize = FdWatchMap::kBlockSize;
  return {fd / kBlockSize, fd % kBlockSize};
}

FdWatchMap::FdWatchMap() = default;

FdWatchMap::~FdWatchMap() = default;

std::pair<WatchEntry&, bool> FdWatchMap::try_emplace(int fd,
                                                     WatchEntry&& value) {
  auto [block_id, offset] = getBlockAndIndex(fd);
  bool inserted           = false;
  auto [it, _]            = block_.try_emplace(block_id);

  // Check if slot is empty
  if (it->second[offset].bitmask_ == 0) {
    inserted           = true;
    it->second[offset] = std::move(value);
  }

  return {it->second[offset], inserted};
}

void FdWatchMap::forEach(std::move_only_function<void(int)> fn) const {
  for (const auto& [block_id, block] : block_) {
    for (size_t offset = 0; offset < kBlockSize; ++offset) {
      const auto& entry = block[offset];
      if (entry.bitmask_ != 0) {
        fn(static_cast<int>((block_id * kBlockSize) + offset));
      }
    }
  }
}

}  // namespace detail

namespace {
static thread_local EventWatcher* curr_ = nullptr;  // NOLINT

[[nodiscard]] EventWatcher* self() noexcept {
  return curr_;
}

EventWatcher* self(EventWatcher* ew) noexcept {
  auto* prev = curr_;
  curr_      = ew;
  return prev;
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

constexpr int kMaxEpollRetries = 1 << 2; // (4)
constexpr size_t kMaxEvents    = 1 << 6; // (64)

}  // namespace

namespace detail {
EventFd::EventFd() : eventfd_(bits::makeEventFd()) {}

EventFd::~EventFd() {
  ::close(eventfd_);
}

void EventFd::clear() const {
  bits::eventFdRead(eventfd_);
}

void EventFd::signal() const {
  bits::eventFdWrite(eventfd_);
}

}  // namespace detail

EventWatcher::EventWatcher(EpollWaitFunc epoll_impl)
    : epoll_fd_(bits::makeEpoll()), epoll_impl_{std::move(epoll_impl)} {
  handleWatch(event_.eventfd_, RDONLY, [this]() {
    TTL_LOG(Trace) << "wakeup(" << event_.eventfd_ << ")";
    std::optional<WatchCallback> task;
    while ((task = task_q_.tryPop()) && task.has_value()) {
      handleCallback(*task);
    }
    event_.clear();
  });
}

EventWatcher::~EventWatcher() {
  unwatchAll();
  ::close(epoll_fd_);
}

void EventWatcher::handleFdWatchModeChange(detail::WatchEntry& entry, int fd,
                                           int op) const {
  epoll_event event{};
  event.data.fd = fd;
  event.events  = entry.bitmask_;

  if (::epoll_ctl(epoll_fd_, op, fd, &event) == -1) {
    TTL_LOG(Error) << "epoll_ctl(" << epoll_fd_ << "," << op << "," << fd
                   << ",*) : ERR = " << errno;
  }
}

void EventWatcher::handleWatch(int fd, WatchFlag flag, WatchCallback callback) {
  auto [entry, _] = fd_map_.try_emplace(fd, {});

  int fd_op = EPOLL_CTL_MOD;
  if (entry.bitmask_ == 0) {
    fd_op = EPOLL_CTL_ADD; 
  }

  entry.bitmask_ |= flag;

  switch (flag) {
    case RDONLY:
      if (std::exchange(/* reader */ entry.cb_[0], std::move(callback)) == std::nullopt) {
        handleFdWatchModeChange(entry, fd, fd_op);
      }
      break;
    case WRONLY:
      if (std::exchange(/* writer */ entry.cb_[1], std::move(callback)) == std::nullopt) {
        handleFdWatchModeChange(entry, fd, fd_op);
      }
      break;
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
    handleWatch(fd, flag, std::move(cb));
  });
}

void EventWatcher::handleUnwatch(int fd, WatchFlag flag) {
  auto [entry, _] = fd_map_.try_emplace(fd, {});

  entry.bitmask_ &= ~flag;

  int fd_op = EPOLL_CTL_MOD;
  if (entry.bitmask_ == 0) {
    fd_op = EPOLL_CTL_DEL;
  }

  switch (flag) {
    case RDONLY:
      if (std::exchange(/* reader */ entry.cb_[0], std::nullopt) != std::nullopt) {
        handleFdWatchModeChange(entry, fd, fd_op);
      }
      break;
    case WRONLY:
      if (std::exchange(/* writer */ entry.cb_[1], std::nullopt) != std::nullopt) {
        handleFdWatchModeChange(entry, fd, fd_op);
      }
      break;
  }
}

void EventWatcher::unwatch(const int fd, const WatchFlag flag) {
  TTL_LOG(Trace) << "unwatch(" << fd << "," << static_cast<int>(flag) << ")";

  if (fd < 0) {
    TTL_LOG(Error) << "unwatch() called with negative fd: " << fd;
    return;
  }

  runInEventWatcherLoop([this, fd, flag] { handleUnwatch(fd, flag); });
}

void EventWatcher::unwatchAll() {
  runInEventWatcherLoop([this]() mutable {
    fd_map_.forEach([epoll_fd_ = epoll_fd_](int fd) {
      if (::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) == -1) {
        TTL_LOG(Error) << "epoll_ctl(" << epoll_fd_ << "," << EPOLL_CTL_DEL
                       << "," << fd << ",*) : ERR = " << errno;
      }
      // entry->bitmask_ = 0;
    });
  });
}

void EventWatcher::loop(int timeout_ms) {
  TTL_LOG(Trace) << "loop(" << timeout_ms << ")";

  // If loop is called from inside another event loop preserve context
  auto* ew = self(this);

  static thread_local std::array<epoll_event, kMaxEvents> events;

  // Retry in case of interrupts.
  // Park here until (any of) FD is ready. 
  int n_ready = -1;
  for (int retry = 0; retry < kMaxEpollRetries; ++retry) {
    n_ready = epoll_impl_(epoll_fd_, events.data(),
                          static_cast<int>(events.size()), timeout_ms);
    if (n_ready >= 0 || errno != EINTR) {
      break;
    }
    TTL_LOG(Trace) << "epoll_impl_(" << epoll_fd_ << ",*," << events.size()
                   << "," << timeout_ms << ") interrupted";
  }

  if (n_ready < 0) {
    TTL_LOG(Trace) << "epoll_impl_(" << epoll_fd_ << ",*," << events.size()
                   << "," << timeout_ms << ") failed with " << n_ready
                   << " ret";
    return;
  }

  TTL_LOG(Trace) << "epoll_impl_(" << epoll_fd_ << ",*," << events.size() << ","
                 << timeout_ms << ") returned " << n_ready << " events";

  for (int i = 0; i < n_ready; ++i) {
    const auto& event = events[i];
    const int fd      = event.data.fd;

    auto [entry, _] = fd_map_.try_emplace(fd, {});


    // FD is broken, let caller handle
    if ((event.events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0) {
      TTL_LOG(Trace) << "error on fd(" << fd << "), events=0x" << std::hex
                     << event.events << std::dec;
      if ((entry.bitmask_ & RDONLY) != 0) {
        handleCallback(/* reader */ *entry.cb_[0]);
      }
      if ((entry.bitmask_ & WRONLY) != 0) {
        handleCallback(/* writer */ *entry.cb_[1]);
      }
      continue;
    }

    if ((event.events & EPOLLIN) != 0 && (entry.bitmask_ & RDONLY) != 0) {
      TTL_LOG(Trace) << "readable(" << fd << ")";
      handleCallback(/* reader */ *entry.cb_[0]);
    }

    if ((event.events & EPOLLOUT) != 0 && (entry.bitmask_ & WRONLY) != 0) {
      TTL_LOG(Trace) << "writable(" << fd << ")";
      handleCallback(/* writer */ *entry.cb_[1]);
    }
  }

  assert(self(ew) == this);
}

void EventWatcher::runInEventWatcherLoop(WatchCallback task) {
  // Shortcut to run inline
  if (self() == this) {
    handleCallback(task);
    return;
  }

  if (!task_q_.push(std::move(task))) {
    // TODO: Propagate
    return;
  }
  event_.signal();
}

}  // namespace getrafty::io
