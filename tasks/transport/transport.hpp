#pragma once

#include <functional>

template <typename T>
using F = std::move_only_function<T>;

class Conveyor;
class ByteBuf;

class ITransport {
 public:
  virtual ~ITransport()                                   = default;
  virtual Conveyor& pipeline() noexcept                   = 0;
  virtual int write(ByteBuf& buf)                         = 0;
  virtual void flush()                                    = 0;
  virtual void onFlushCompleted(F<void()>)                = 0;
  virtual void onWritableStateChanged(F<void(bool)>)      = 0;
  virtual void readableStateChanged(bool)                 = 0;
  virtual void shutdown(int)                              = 0;
  virtual void onShutdownCompleted(F<void(int)>)          = 0;
  [[nodiscard]] virtual const char* otherEndpoint() const = 0;
  [[nodiscard]] virtual const char* selfEndpoint() const  = 0;
};
