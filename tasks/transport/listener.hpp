#pragma once
#include <memory>
#include <string>
#include "transport.hpp"

class IListener {
 public:
  virtual ~IListener()                                          = default;
  virtual void onFailure(F<void(int)>)                          = 0;
  virtual void onError(F<void(int)>)                            = 0;
  virtual void onAccepted(F<void(std::unique_ptr<ITransport>)>) = 0;
  virtual void onStarted(F<void(const std::string&)>)           = 0;
  virtual void onShutdown(F<void()>)                            = 0;
  virtual void listenAndWait()                                  = 0;
  virtual void shutdown()                                       = 0;
};
