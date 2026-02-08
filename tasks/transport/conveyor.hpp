#pragma once

#include <concepts>
#include <cstddef>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

#include "event.hpp"

class Conveyor;
class ITransport;
class StageContext;

namespace traits {

template <class T>
concept HasOnAdded = requires(T& t, StageContext& c) { t.onAdded(c); };

template <class T>
concept HasOnRemoved = requires(T& t, StageContext& c) { t.onRemoved(c); };

template <class T, class E>
concept HasOnInbound =
    requires(T& t, StageContext& c, E& e) { t.onInbound(c, e); };

template <class T, class E>
concept HasOnOutbound =
    requires(T& t, StageContext& c, E& e) { t.onOutbound(c, e); };

}  // namespace traits

struct Stage {
  virtual ~Stage() = default;

  virtual void onAdded(StageContext& /*unused*/) {}

  virtual void onRemoved(StageContext& /*unused*/) {}

  virtual void onInboundEvent(StageContext&, Event&) noexcept  = 0;
  virtual void onOutboundEvent(StageContext&, Event&) noexcept = 0;
};

class StageContext {
  Conveyor* p_;
  size_t i_;

public:
  StageContext(Conveyor* p, size_t i) noexcept : p_(p), i_(i) {}

  [[nodiscard]] size_t index() const noexcept { return i_; }

  [[nodiscard]] ITransport& transport() noexcept;

  template <typename E>
  void fireInbound(E& evt) noexcept;

  template <typename E>
  void fireOutbound(E& evt) noexcept;
};

class Conveyor {
  ITransport* transport_;
  std::vector<std::unique_ptr<Stage>> stages_;

public:
  explicit Conveyor(ITransport* t) noexcept : transport_(t) {}

  ~Conveyor() {
    for (size_t i = 0; i < stages_.size(); ++i) {
      StageContext ctx(this, i);
      stages_[i]->onRemoved(ctx);
    }
  }

  [[nodiscard]] ITransport* transport() const noexcept { return transport_; }

  template <class T, class... Args>
  Conveyor& addLast(Args&&... args);

  template <typename E>
    requires(!std::same_as<std::decay_t<E>, Event>)
  void fireInbound(E&& evt) {
    if (stages_.empty()) {
      return;
    }
    Event wrapped{std::forward<E>(evt)};
    fireInboundFrom(0, wrapped);
  }

  template <typename E>
    requires(!std::same_as<std::decay_t<E>, Event>)
  void fireOutbound(E&& evt) {
    if (stages_.empty()) {
      return;
    }
    Event wrapped{std::forward<E>(evt)};
    fireOutboundFrom(stages_.size() - 1, wrapped);
  }

  void fireInboundFrom(size_t idx, Event& evt) {
    if (idx < stages_.size()) {
      StageContext ctx(this, idx);
      stages_[idx]->onInboundEvent(ctx, evt);
    }
  }

  void fireOutboundFrom(size_t idx, Event& evt) {
    if (idx < stages_.size()) {
      StageContext ctx(this, idx);
      stages_[idx]->onOutboundEvent(ctx, evt);
    }
  }

private:
  friend class StageContext;
};

inline ITransport& StageContext::transport() noexcept {
  return *p_->transport();
}

template <typename E>
inline void StageContext::fireInbound(E& evt) noexcept {
  Event wrapped{evt};
  p_->fireInboundFrom(i_ + 1, wrapped);
}

template <>
inline void StageContext::fireInbound<Event>(Event& evt) noexcept {
  p_->fireInboundFrom(i_ + 1, evt);
}

template <typename E>
inline void StageContext::fireOutbound(E& evt) noexcept {
  if (i_ > 0) {
    Event wrapped{evt};
    p_->fireOutboundFrom(i_ - 1, wrapped);
  }
}

template <>
inline void StageContext::fireOutbound<Event>(Event& evt) noexcept {
  if (i_ > 0) {
    p_->fireOutboundFrom(i_ - 1, evt);
  }
}

template <class T>
class StageAdapter final : public Stage {
public:
  template <class... Args>
  explicit StageAdapter(Args&&... args) : impl_(std::forward<Args>(args)...) {}

  void onAdded(StageContext& ctx) override {
    if constexpr (traits::HasOnAdded<T>) {
      impl_.onAdded(ctx);
    }
  }

  void onRemoved(StageContext& ctx) override {
    if constexpr (traits::HasOnRemoved<T>) {
      impl_.onRemoved(ctx);
    }
  }

  void onInboundEvent(StageContext& ctx, Event& evt) noexcept override {
    std::visit([&](auto& e) { handleInbound(ctx, evt, e); }, evt);
  }

  void onOutboundEvent(StageContext& ctx, Event& evt) noexcept override {
    std::visit([&](auto& e) { handleOutbound(ctx, evt, e); }, evt);
  }

private:
  template <typename E>
  void handleInbound(StageContext& ctx, Event& evt, E& e) noexcept {
    if constexpr (traits::HasOnInbound<T, E>) {
      impl_.onInbound(ctx, e);
    } else {
      ctx.fireInbound(evt);
    }
  }

  template <typename E>
  void handleOutbound(StageContext& ctx, Event& evt, E& e) noexcept {
    if constexpr (traits::HasOnOutbound<T, E>) {
      impl_.onOutbound(ctx, e);
    } else {
      ctx.fireOutbound(evt);
    }
  }

  T impl_;
};

template <class T, class... Args>
inline Conveyor& Conveyor::addLast(Args&&... args) {
  auto st = std::make_unique<StageAdapter<T>>(std::forward<Args>(args)...);
  StageContext ctx(this, stages_.size());
  st->onAdded(ctx);
  stages_.push_back(std::move(st));
  return *this;
}
