prologue()

add_task_library(
  event_watcher
  event_watcher/event_watcher.cpp
  event_watcher/event_watcher.hpp
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
  byte_buffer.hpp
  event.hpp
  conveyor.hpp
  tcp/tcp_listener.cpp
  tcp/tcp_listener.hpp
  tcp/tcp_transport.cpp
  tcp/tcp_transport.hpp
  tcp/tcp_dialer.cpp
  tcp/tcp_dialer.hpp
  listener.hpp
  dialer.hpp
  transport.hpp
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
