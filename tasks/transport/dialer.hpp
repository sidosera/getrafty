#pragma once
#include <memory>

#include "transport.hpp"

class IDialer {
 public:
  virtual ~IDialer()                                             = default;
  virtual void onFailure(F<void(int)>)                           = 0;
  virtual void onConnected(F<void(std::unique_ptr<ITransport>)>) = 0;
  virtual void dial()                                            = 0;
};
