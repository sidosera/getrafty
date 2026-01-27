prologue()

add_task_library(
  event_watcher
  event_watcher/event_watcher.cpp
  event_watcher/event_watcher.hpp
  event_watcher/queue.hpp
)

target_task_link_libraries(
  event_watcher
  PRIVATE
  bits
  ttl
)

add_task_library(
  transport
  abstract_transport.cpp
  abstract_transport.hpp
  bytebuf.hpp
  listener.hpp
  dialer.hpp
  transport.hpp
  conveyor.hpp
)

target_task_link_libraries(
  transport
  PRIVATE
  event_watcher
  bits
  ttl
)

add_task_test(
  test
  test.cpp
)

target_task_link_libraries(
  test
  PRIVATE
  gtest
  transport
  event_watcher
  bits
  ttl
)

epilogue()
