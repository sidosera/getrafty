# Inter-process communication library


## Overview
This design follows Netty pipeline model with:
- `Transport` (the I/O owner)
- `Conveyor` (per-connection handler chain)
- `Stage` (handler)
- `StageContext` (propagation + access to transport/pipeline)
- `Dialer` (async connect)
- `Listener` (accept loop)


## Concurrency model
- A Transport is pinned to a single event-loop thread for its lifetime.
- All callbacks and pipeline mutations run on the event-loop thread.
- Transport read/write loops are written as "drain until would-block" which is required for edge-triggered and safe for level-triggered.


## ByteBuf

ByteBuf is a sequential, zero-copy-oriented stream buffer built on fixed-size segments.

Representation:
- Storage is a pool of fixed-size segments (blocks); segments are never resized.
- Buffers do not own segments; they hold a list of views: (segment*, offset, len).
- Segments are reference-counted by the allocator. When no buffers reference a
  segment, it is returned to the pool.
- Reads traverse segment views; segments are never moved between buffers.
- The initial allocator is a simple proxy for malloc/free. The API allows a
  future pooled allocator without changing callers.

API contract:
```cpp
class ByteBuf {
public:
    ByteBuf() = default;

    // Returns total readable bytes.
    std::size_t readableBytes() const noexcept;

    // Returns currently reserved writable bytes (no new allocation).
    std::size_t writableBytes() const noexcept;

    // Best-effort read:
    // Transfers up to n bytes from this into dst.
    // Bytes are removed from this and appended to dst.
    // Returns number of bytes transferred.
    std::size_t read(ByteBuf& dst, std::size_t n) noexcept;

    // Best-effort peek:
    // Copies up to n bytes into dst, no state change.
    std::size_t peek(void* dst, std::size_t n) const noexcept;

    // Appends raw bytes to the tail; grows the buffer.
    // Returns false on allocation failure.
    bool write(const void* src, std::size_t n) noexcept;

    // Transfer n bytes from src.
    // Transfers up to n bytes (min(n, src.readableBytes())).
    void write(ByteBuf&& src, std::size_t n) noexcept;

    // Reserve n writable bytes (may allocate new segments).
    // Returns false on allocation failure.
    bool reserve(std::size_t n) noexcept;

    // View of available writable bytes (tailroom) for readv.
    // Does not allocate; caller must reserve() first.
    // <T> can be iovec or a custom scatter/gather type.
    // Valid until the buffer is mutated.
    template<typename T>
    T tailroom() noexcept;

    // View of available readable bytes (headroom) for writev.
    // <T> can be iovec or a custom scatter/gather type.
    // Valid until the buffer is mutated.
    template<typename T>
    T headroom() noexcept;


    // Drop up to n readable bytes from the head.
    // Returns bytes dropped (<= n).
    std::size_t advance(std::size_t n);

    // Make up to n bytes from tailroom readable.
    // Returns bytes committed (<= n).
    std::size_t commit(std::size_t n);
};
```

Semantics:
- read does not move segments. It only adjusts references/slices in both buffers.
- Partial segment transfers create a view slice for the prefix; the remainder
  stays in the source buffer.
- write only appends; it never affects readable bytes already present.
- headroom() is intended for writev; advance() drops bytes after writev.
- tailroom() + commit() is intended for readv to append newly read bytes.

Example:
```cpp
// readv into buffer
buf.reserve(4096);
auto w = buf.tailroom<std::vector<iovec>>();
ssize_t nread = ::readv(fd, w.data(), static_cast<int>(w.size()));
if (nread > 0) { buf.commit(static_cast<std::size_t>(nread)); }

// writev from buffer
auto r = buf.headroom<std::vector<iovec>>();
ssize_t nwritten = ::writev(fd, r.data(), static_cast<int>(r.size()));
if (nwritten > 0) { buf.advance(static_cast<std::size_t>(nwritten)); }
```


EventWatcher callbacks run on the single loop thread and must not be invoked after `unwatch*` returns.


## Events
Events are typed `T` objects deriving from `EventBase<T>`.


```cpp
using type_id_t = const void*;

template <class T>
constexpr type_id_t type_id() noexcept {
    static const int tag{};
    return &tag;
}

struct Event {
    virtual ~Event() = default;
    virtual type_id_t type() const noexcept = 0;
};

template <class Derived>
struct EventBase : Event {
    static constexpr type_id_t static_type() noexcept { return type_id<Derived>(); }
    type_id_t type() const noexcept override { return static_type(); }
};

template <class T, class E>
auto as(E& e) noexcept
    -> std::optional<std::reference_wrapper<std::conditional_t<std::is_const_v<E>, const T, T>>>
{
    using RefT = std::conditional_t<std::is_const_v<E>, const T, T>;
    if (e.type() == EventBase<T>::static_type()) {
        return static_cast<RefT&>(e);
    }
    return std::nullopt;
}
```


Event types:
```cpp
// Transport lifecycle events
struct InboundTransportActive   : EventBase<InboundTransportActive> {};
struct InboundTransportInactive : EventBase<InboundTransportInactive> {};
struct InboundTransportError    : EventBase<InboundTransportError> { Error err; };

// Data
struct InboundBytes  : EventBase<InboundBytes>  { ByteBuf buf; };
struct OutboundBytes : EventBase<OutboundBytes> { ByteBuf buf; };

// Flow control
struct InboundSuspend  : EventBase<InboundSuspend> {};
struct InboundResume   : EventBase<InboundResume> {};
struct OutboundSuspend : EventBase<OutboundSuspend> {};
struct OutboundResume  : EventBase<OutboundResume> {};
```


Event objects are borrowed during propagation; handlers must not store references
or pointers beyond the current callback chain.


## Conveyor
```cpp
class Transport;
class Conveyor;
class StageContext;



/*
 * Type-erased pipeline handler.
 * Inbound flows head->tail; outbound flows tail->head.
 * Ordering on close: InboundTransportError (if any) -> InboundTransportInactive -> onRemoved.
 *
 * [HEAD := transport] -> [S1] -> [S2] -> ... -> [TAIL := handler]
 *
 * Stages are plain types; Conveyor wraps them in an internal adapter.
 * A stage may implement any subset of:
 *   void onAdded(StageContext&);
 *   void onRemoved(StageContext&);
 *   void onInbound(StageContext&, InboundTransportActive&);
 *   void onInbound(StageContext&, InboundTransportInactive&);
 *   void onInbound(StageContext&, InboundTransportError&);
 *   void onInbound(StageContext&, InboundBytes&);
 *   void onInbound(StageContext&, Event&); // catch-all
 *   void onOutbound(StageContext&, OutboundBytes&);
 *   void onOutbound(StageContext&, OutboundClose&);
 */
struct Stage {
    virtual ~Stage() = default;


    // Called when the stage is inserted into the pipeline.
    virtual void onAdded(StageContext& ctx) {}


    // Called when the stage is removed or the pipeline is destroyed.
    //
    // On close `onRemoved` is called after InboundTransportInactive.
    virtual void onRemoved(StageContext& ctx) {}


    // Inbound entrypoint for lifecycle/data/user events.
    virtual void onInboundEvent(StageContext& ctx, Event& evt) = 0;


    // Outbound entrypoint for data/close events.
    virtual void onOutboundEvent(StageContext& ctx, Event& evt) = 0;
};


// Detection helpers (sketch; other traits follow the same pattern).
template <class T, class = void>
struct has_onAdded : std::false_type {};
template <class T>
struct has_onAdded<T, std::void_t<decltype(std::declval<T&>().onAdded(
    std::declval<StageContext&>()))>> : std::true_type {};


template <class T, class = void>
struct has_onRemoved : std::false_type {};
template <class T>
struct has_onRemoved<T, std::void_t<decltype(std::declval<T&>().onRemoved(
    std::declval<StageContext&>()))>> : std::true_type {};


template <class T, class E, class = void>
struct has_onInbound : std::false_type {};
template <class T, class E>
struct has_onInbound<T, E, std::void_t<decltype(std::declval<T&>().onInbound(
    std::declval<StageContext&>(), std::declval<E&>()))>> : std::true_type {};


template <class T, class E, class = void>
struct has_onOutbound : std::false_type {};
template <class T, class E>
struct has_onOutbound<T, E, std::void_t<decltype(std::declval<T&>().onOutbound(
    std::declval<StageContext&>(), std::declval<E&>()))>> : std::true_type {};


/*
 * Internal adapter that type-erases a stage and dispatches typed handlers.
 * The adapter prefers specific overloads and falls back to the catch-all.
 */
template <class T>
class StageAdapter final : public Stage {
public:
    template <class... Args>
    explicit StageAdapter(Args&&... args) : impl_(std::forward<Args>(args)...) {}


    void onAdded(StageContext& ctx) override {
        if constexpr (has_onAdded<T>::value) {
            impl_.onAdded(ctx);
        }
    }


    void onRemoved(StageContext& ctx) override {
        if constexpr (has_onRemoved<T>::value) {
            impl_.onRemoved(ctx);
        }
    }


    void onInboundEvent(StageContext& ctx, Event& evt) override {
        if (dispatchInbound<InboundTransportActive>(ctx, evt)) return;
        if (dispatchInbound<InboundTransportInactive>(ctx, evt)) return;
        if (dispatchInbound<InboundTransportError>(ctx, evt)) return;
        if (dispatchInbound<InboundBytes>(ctx, evt)) return;
        if (dispatchInbound<Event>(ctx, evt)) return; // catch-all
        ctx.fireInbound(evt);
    }

    void onOutboundEvent(StageContext& ctx, Event& evt) override {
        if (dispatchOutbound<OutboundBytes>(ctx, evt)) return;
        if (dispatchOutbound<OutboundClose>(ctx, evt)) return;
        ctx.fireOutbound(evt);
    }


private:
    template <class E>
    bool dispatchInbound(StageContext& ctx, Event& evt) {
        if (auto e = as<E>(evt)) {
            if constexpr (has_onInbound<T, E>::value) {
                impl_.onInbound(ctx, e->get());
            } else if constexpr (has_onInbound<T, Event>::value) {
                impl_.onInbound(ctx, evt);
            } else {
                ctx.fireInbound(evt);
            }
            return true;
        }
        return false;
    }


    template <class E>
    bool dispatchOutbound(StageContext& ctx, Event& evt) {
        if (auto e = as<E>(evt)) {
            if constexpr (has_onOutbound<T, E>::value) {
                impl_.onOutbound(ctx, e->get());
            } else {
                ctx.fireOutbound(evt);
            }
            return true;
        }
        return false;
    }


    T impl_;
};



/*
 * Per-stage context used for propagation and access to the pipeline.
 * Propagates inbound to next and outbound to previous in the chain.
 * The context is also the anchor for mid-pipeline mutations.
 */
class StageContext {
public:
    // Returns the owning transport.
    Transport& transport() noexcept;


    // Returns the owning pipeline.
    Conveyor& pipeline() noexcept;


    // Propagates a typed inbound event to the next stage (toward tail).
    // Used for lifecycle, data, and user-defined events.
    // The event must outlive the current callback chain.
    //
    // Route: current stage -> ... -> TAIL
    void fireInbound(Event& evt);


    // Propagates a typed outbound event to the previous stage (toward head).
    // The event must outlive the current callback chain.
    // Route: current stage -> ... -> HEAD
    void fireOutbound(Event& evt);
};
```


User-defined events are emitted via `fireInbound`. Handlers that care can
implement `onInbound(StageContext&, Event&)` and downcast with `as<T>()`.


```cpp
/*
 * Per-connection pipeline that owns stages and drives event ordering.
 * Inbound starts at head (transport side); outbound starts at tail (app side).
 */
class Conveyor {
public:
    // Creates a pipeline bound to the transport.
    explicit Conveyor(Transport& t);


    // Appends a stage at the tail (application side).
    // Requires: must not be called during a Stage callback.
    template <class T, class... Args>
    Conveyor& addLast(Args&&... args);


    // Inserts a stage before the first stage matching the predicate.
    // Requires: must not be called during a Stage callback.
    // Ensures: new stage receives onAdded before any further events.
    // Returns false if no stage matches.
    template <class T, class... Args>
    bool addBefore(F<bool(const Stage&)> pred, Args&&... args);


    // Inserts a stage after the first stage matching the predicate.
    // Requires: must not be called during a Stage callback.
    // Ensures: new stage receives onAdded before any further events.
    // Returns false if no stage matches.
    template <class T, class... Args>
    bool addAfter(F<bool(const Stage&)> pred, Args&&... args);


    // Replaces or removes each stage matching the predicate.
    // Mapper receives each matched stage and returns replacement (or nullptr to remove).
    // Requires: must not be called during a Stage callback.
    // Ensures: old stages receive onRemoved; new stages (if any) receive onAdded.
    // Returns count of stages matched.
    std::size_t map(F<std::unique_ptr<Stage>(Stage&)> mapper, F<bool(const Stage&)> pred);


    // Fires a typed inbound event from the head.
    void fireInbound(Event& evt);


    // Starts outbound propagation from the tail.
    void fireOutbound(Event& evt);
};
```


## Transport
```cpp
/*
 * Encapsulates a logical connection between a pair of endpoints.
 */
class ITransport {
public:
    virtual ~ITransport() = default;


    // Returns the transport pipeline.
    virtual Conveyor& pipeline() noexcept = 0;


    // Move bytes for transmission.
    //
    // This call is non blocking. 
    //
    // Returns negative if outbound queue is full or positive := number of bytes written.
    // If operation completes successfully returns number of bytes written; bytes are
    // removed from the head of `buf` via advance().
    // If operation fails, returns negative value; buffer is left untouched.
    //
    // If operation fails with backpressure event (buffer full) `onWritableStateChanged` is called with `false` flag.
    // And once buffer is freed `onWritableStateChanged` is called with `true` flag. 
    //
    virtual int write(ByteBuf& buf) = 0;


    // Ask to empty buffers.
    //
    // The operation completes when `onFlushCompleted` is invoked.
    virtual void flush() = 0;


    // Invoked once buffered data is no longer owned by transport;
    // e.g. for BSD socket it is invoked once data is moved to the kernel.
    virtual void onFlushCompleted(F<void()>) = 0;
    
    // Invoked when transport is ready to accept writes after backpressure signal;
    // i.e. after `write` returned `false` on prior attempts.
    //
    // It is called exactly once for a sequence of state transition; 
    // i.e. write queue limit hit the `onWritableStateChanged(false)` called first (exactly once)
    // then once buffer pressure is released `onWritableStateChanged(true)` is called (exactly once).
    virtual void onWritableStateChanged(F<void(bool /*writable*/)> cb) = 0;
    
    // Signals transport to enable or disable receiving.
    // If `false` flag passed, suspends receiving.
    // If `true` flag passed, resumes.
    virtual void readableStateChanged(bool /*readable*/) = 0;


    // Initiates an orderly shutdown via the pipeline.
    // If outbound close is initiated during a callback, the current event continues
    // propagating, but no new inbound/outbound events are delivered after the
    // transport becomes inactive.
    //
    // If arg is 0 normal shutdown, if -errno error.
    virtual void shutdown(int) = 0;


    // Invoked when transport shutdown completes;
    // i.e. transport is inactive before this is called. 
    // Callback accepts int to signal error (or 0 if success).
    virtual void onShutdownCompleted(F<void(int)>) = 0;



    // Returns identity of remote.
    // E.g. for TCP it is a C-string IP:PORT, yet the format is implementation defined.
    virtual const char* otherEndpoint() const = 0;


    // Returns identity of host.
    // E.g. for TCP it is a C-string IP:PORT, yet the format is implementation defined.
    virtual const char* selfEndpoint() const = 0;
};
```


```cpp
/*
 * Owns the I/O backend, event-loop hooks, and backpressure state.
 * All methods are called on the transport's event-loop thread.
 */
class AbstractEventLoopTransport : public ITransport  {
public:
    virtual ~AbstractEventLoopTransport() = default;


    // Binds watcher and constructs the pipeline.
    explicit AbstractEventLoopTransport(int fd, EventWatcher& ew);


    // Event-loop readable callback; reads and fires inbound data.
    void onReadable();


    // Event-loop writable callback; flushes outbound data.
    void onWritable();
    
    // Event-loop callback for connect completion.
    void onConnectWritable();


    // Returns the underlying file descriptor.
    int fd() const noexcept;


protected:
    // Appends bytes to dst and increases readableBytes().
    // Implementations may use readv into newly allocated pages.
    // Returns:
    //   >0 bytes read
    //    0 EOF
    //   <0 error as -errno (including -EAGAIN/-EWOULDBLOCK for would-block)
    virtual int readSome(ByteBuf& dst) = 0;


    // Writes bytes from src headroom and advances it by number of bytes written.
    // Implementations may use writev/sendmsg with iovec spans.
    // Returns:
    //   >0 bytes written
    //   <0 error as -errno (including -EAGAIN/-EWOULDBLOCK for would-block)
    virtual int writeSome(ByteBuf& src) = 0;
};
```


## Flow Control


Flow control events manage backpressure signals and travel opposite direction to data flow:


```cpp
struct FlowControlStage {
    StageContext* ctx_ = nullptr;


    void onAdded(StageContext& ctx) {
        ctx_ = &ctx;
        ctx.transport().onWritableStateChanged([this](bool writable) {
            if (writable) {
                OutboundResume evt;
                ctx_->fireInbound(evt);
            } else {
                OutboundSuspend evt;
                ctx_->fireInbound(evt);
            }
        });
    }


    void onOutbound(StageContext& ctx, InboundSuspend&) {
        ctx.transport().readableStateChanged(false);
    }

    void onOutbound(StageContext& ctx, InboundResume&) {
        ctx.transport().readableStateChanged(true);
    }


    void onOutbound(StageContext& ctx, OutboundBytes& e) {
        ctx.fireOutbound(e);
    }
};
```


## Dialer & Listener
```cpp


/*
 * Dialer for async outbound connections.
 * Builds a per-connection pipeline and returns a connecting Transport.
 */
struct IDialer {
    virtual ~IDialer() = default;


    // Invoked when connection can not be established
    // e.g. remote inactive, no memory.
    // 
    // 
    virtual void onFailure(F<void(int /*errno*/)> cb) = 0;


    // Invoked when connection established successfully.
    //
    // Not thread safe.
    virtual void onConnected(F<void(std::unique_ptr<ITransport>)>&& cb) = 0;


    // Initiate connection.
    //
    // Can be called exactly once.
    // Does not block caller thread.
    //
    // Not thread-safe.
    virtual void dial() = 0;
};


/*
 * Listener for accepting inbound connections and emitting transports.
 * Builds a per-connection pipeline for each accepted Transport.
 */
struct IListener {
    virtual ~IListener() = default;


    // Called once unrecoverable error occurs (e.g. EBADF, EPERM).
    // 
    // The listener is now in a broken state and `shutdown` will be called next.
    // Thread-safe.
    virtual void onFailure(F<void(int /*errno*/)>) = 0;


    // Called once new connection can not be created (e.g. ENFILE, ENOMEM).
    //
    // This is usually transient error and already accepted connections can operate.
    // Not thread-safe.
    virtual void onError(F<void(int /*errno*/)>) = 0;

    // Called once after successful bind.
    //
    // Provides the bound address (implementation defined, e.g. IP:PORT for TCP).
    // Not thread-safe.
    virtual void onStarted(F<void(const std::string& /*address*/)> cb) = 0;


    // For each accepted connection, constructs a Transport in `Active` state,
    // initializes the pipeline via `ConveyorInit`, and invokes `onAccepted`.
    //
    // Not thread-safe.
    virtual void onAccepted(F<void(std::unique_ptr<ITransport>)>&& cb) = 0;


    // Invoked on shutdown event.
    //
    // `onAccepted` will not be called after `onShutdown`.
    //
    // Not thread-safe.
    virtual void onShutdown(F<void()>&& cb) = 0;


    // Binds listener and start accepting connections.
    //
    // Block caller thread until `shutdown`.
    // Can be called exactly once
    virtual void listenAndWait() = 0;


    // Stops accepting new connections and invoke `onShutdown`.
    //
    // Thread safe.
    virtual void shutdown() = 0;
};
```


```cpp
using ConveyorInit = F<void(Conveyor&)>;


ConveyorInit initClient = [](Conveyor& p) {
    p.addLast<FlowControlStage>()
     .addLast<FramingStage>()
     .addLast<AppStage>();
};


// Dialer
IDialer& dialer = /* ... */;
dialer.onFailure([](int err) {
    // Unrecoverable error, e.g. system resource exhaustion.
});
dialer.onConnected([&](std::unique_ptr<ITransport> transport) {
    initClient(transport->pipeline());
    // store transport handle
});
dialer.dial();
```


```cpp
using ConveyorInit = F<void(Conveyor&)>;


ConveyorInit initServer = [](Conveyor& p) {
    p.addLast<FlowControlStage>()
     .addLast<FramingStage>()
     .addLast<AppStage>();
};


// Listener
IListener& listener = /* ... */;
listener.onFailure([](int err) {
    // Unrecoverable error (EBADF, EPERM).
    // Listener is not functional and shutdown event will be invoked after this.
});
listener.onError([](int err) {
    // Transient error (ENFILE, ENOMEM), existing connections unaffected.
});
listener.onStarted([](const std::string& address) {
    // Listener bound to address.
});
listener.onAccepted([&](std::unique_ptr<ITransport> transport) {
    initServer(transport->pipeline());
    // store transport handle
});
listener.onShutdown([]() {
    // Listener closed.
    // This is invoked after `onFailure` if any.
});
listener.listenAndWait();  // Blocks until `shutdown` invoked from another thread.
```



## Examples
```cpp
struct FramingStage {
    ByteBuf current_frame;


    void onInbound(StageContext& ctx, InboundBytes& e) {
        const std::size_t incoming = e.buf.readableBytes();
        if (incoming > 0) {
            current_frame.write(std::move(e.buf), incoming);
        }

        for (;;) {
            std::uint32_t len = 0;
            if (current_frame.peek(&len, sizeof(len)) != sizeof(len)) {
                break;
            }

            if (current_frame.readableBytes() < sizeof(len) + len) {
                break;
            }

            current_frame.advance(sizeof(len));
            ByteBuf frame;
            current_frame.read(frame, len);
            InboundBytes out{std::move(frame)};
            ctx.fireInbound(out);
        }
    }


    void onOutbound(StageContext& ctx, OutboundBytes& e) {
        ByteBuf frame = std::move(e.buf);
        ByteBuf out;
        std::uint32_t len = static_cast<uint32_t>(frame.readableBytes());
        out.write(&len, sizeof(len));
        out.write(std::move(frame), frame.readableBytes());
        OutboundBytes outEvt{std::move(out)};
        ctx.fireOutbound(outEvt);
    }
};


struct AppStage {
    void onInbound(StageContext& ctx, InboundBytes& e) {
        // Handle request, maybe emit response.
        OutboundBytes out{std::move(e.buf)};
        ctx.fireOutbound(out);
    }
};


ConveyorInit init = [](Conveyor& p) {
    p.addLast<FlowControlStage>()
     .addLast<FramingStage>()
     .addLast<AppStage>();
};
```


### IdleStage (user-triggered events)
```cpp
struct IdleEvent : EventBase<IdleEvent> {
    enum Type { ReadIdle, WriteIdle } type;
};


struct IdleStage {
    EventWatcher* ew_;
    int timerFd_ = -1;
    std::chrono::seconds timeout_;
    StageContext* ctx_ = nullptr;


    IdleStage(EventWatcher& ew, std::chrono::seconds timeout)
        : ew_(&ew), timeout_(timeout) {}


    void onAdded(StageContext& ctx) {
        ctx_ = &ctx;
        timerFd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
        armTimer();
        ew_->watchRead(timerFd_, [this]() {
            uint64_t expirations;
            ::read(timerFd_, &expirations, sizeof(expirations));
            IdleEvent evt{IdleEvent::ReadIdle};
            ctx_->fireInbound(evt);
            armTimer();
        });
    }


    void onInbound(StageContext& ctx, InboundBytes& e) {
        armTimer();
        ctx.fireInbound(e);
    }


    void onRemoved(StageContext& ctx) {
        ew_->unwatchRead(timerFd_);
        ::close(timerFd_);
    }


private:
    void armTimer() {
        itimerspec spec{{0, 0}, {timeout_.count(), 0}};
        timerfd_settime(timerFd_, 0, &spec, nullptr);
    }
};


struct HeartbeatAppStage {
    void onInbound(StageContext& ctx, Event& evt) {
        if (auto idle = as<IdleEvent>(evt)) {
            if (idle->get().type == IdleEvent::ReadIdle) {
                OutboundClose close{0};
                ctx.fireOutbound(close);
            }
            return;
        }
        if (auto data = as<InboundBytes>(evt)) {
            OutboundBytes out{std::move(data->get().buf)};
            ctx.fireOutbound(out);
            return;
        }
        ctx.fireInbound(evt);
    }
};


ConveyorInit initWithIdle = [&ew](Conveyor& p) {
    p.addLast<FlowControlStage>()
     .addLast<IdleStage>(ew, std::chrono::seconds{30})
     .addLast<FramingStage>()
     .addLast<HeartbeatAppStage>();
};
```


## Server


                 W1: [@@] 
-> Q: [@@@@] ->  W2: [@]


IO vs Worker

IO

- Read + Write + Frame

Worker

- Parsing
- Handling
- 

Worker Queue

Timeout 

Queue timeout

Deadline


Max connections

Load shedding






TODO:

[x] Interfaces

[x] TCP stack

[*] Stupid buffer

[*] Conveyor

[*] Protocol

[*] Zero-copy buffer
