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


```cpp
/*
 * Allocator concept for ByteBuf storage.
 * The allocator is a concrete type supplied by the application.
 * TByteBuf is parameterized by a global allocator instance.
 */
struct TByteBufAlloc {
    // Allocate contiguous memory segment and returns first readable byte.
    // If memory of size `n` can't be allocated returns nullptr.
    void* alloc(std::size_t n, std::size_t align);


    // Free contiguous memory segment previously allocated with `alloc`.
    void free(void* p, std::size_t n, std::size_t align);
};


// Global allocator instance used by ByteBuf.
extern TByteBufAlloc ZeroCopyAlloc;
  
/*
 * Scatter/gather view of a buffer (POSIX iovec, see <sys/uio.h>).
 * Used by to<IoVecView>() and from<IoVecView>().
 * Valid until the ByteBuf is mutated or destroyed.
 */
using IoVecView = std::vector<iovec>;


/*
 * ByteBuf follows folly::IOBuf.
 *
 * Representation:
 * - Storage is a pool of fixed-size pages; pages are reused and never resized.
 * - Buffers are chains of page slices (offset/length) hidden from callers.
 * - Each page has a shared control block with refcount and is returned to the
 *   pool when no buffers reference it.
 * - Alloc provides allocation for pages; TByteBuf never calls ::operator new
 *   directly and never reallocates a page.
 * - New pages are allocated via the buffer's allocator; each page records the
 *   allocator that will receive it on release.
 * - append(buf) links pages as-is; pages keep their original allocator.
 *
 * Allocation model:
 * - Operations that need more space add pages to the chain.
 * - The only allocation that can occur is when the page pool is empty.
 *
 * API model:
 * - The public API exposes only logical operations and does not expose pages.
 * - to<IoVecView>() exposes a scatter/gather view; from<IoVecView>() copies into pages.
 * - Buffers grow only at the tail; prepend is not supported.
 *   To prepend, create a new buffer for the header and append the payload.
 * - offset()/seek()/read() provide a cursor for copy-based parsing.
 * - append does not move the cursor; call seek(size()) to position at tail.
 * - reserve(capacity) preallocates pages.
 *
 * Complexity:
 * - append(buf) is O(1) (linking).
 * - to<IoVecView>()/slice are zero-copy; read/from<IoVecView>()/append of raw bytes are O(n).
 *
 * Copy semantics:
 * - Copy shares storage (refcount++) and copies view range (start + size).
 * - The allocator association is preserved.
 *
 * Move semantics:
 * - Move transfers the handle (refcount unchanged).
 *
 */


template <TByteBufAlloc& TByteBufAllocImpl>
class TByteBuf {
public:
    // Creates an empty handle with no storage.
    // Invariant: size() == 0.
    TByteBuf() = default;


    // Returns total readable bytes across the chain.
    std::size_t size() const noexcept;


    // Returns true when size() is 0.
    bool empty() const noexcept;


    // Returns the current cursor offset from the beginning.
    // Invariant: 0 <= offset() <= size().
    std::size_t offset() const noexcept;


    // Sets the cursor offset from the beginning.
    // Requires: offset <= size().
    void seek(std::size_t offset);


    // Copies n raw bytes from the cursor into dst and advances the cursor.
    // Ensures: returns bytes read or -errno on error.
    int readBinary(void* dst, std::size_t n) noexcept;


    // Reads a portable-encoded integer from the cursor into out.
    // Ensures: returns false if insufficient data.
    bool readUI32(std::uint32_t& out) noexcept;
    bool readI32(std::int32_t& out) noexcept;
    bool readUI64(std::uint64_t& out) noexcept;
    bool readI64(std::int64_t& out) noexcept;


    // Converts this buffer to a view of type T.
    // For example iovec[] for zero-copy IO.
    template <class T>
    T to() const noexcept;


    // Returns a zero-copy view of [offset, offset + len).
    // Requires: offset + len <= size().
    // Ensures: returned slice has offset() == 0.
    TByteBuf<TByteBufAllocImpl> slice(std::size_t offset, std::size_t len) const;


    // Replaces this buffer contents from a view of type T.
    // Ensures: returns false and makes no changes on allocation failure.
    template <class T>
    bool from(const T& view) noexcept;


    // Preallocates pages to provide at least capacity bytes.
    // Ensures: returns false and makes no changes on allocation failure.
    bool reserve(std::size_t capacity) noexcept;


    // Copies n raw bytes from src to the end of the buffer.
    // Ensures: returns false on allocation failure.
    bool writeBinary(const void* src, std::size_t n) noexcept;


    // Writes a portable-encoded integer to the end of the buffer.
    // Ensures: returns false on allocation failure.
    bool writeUI32(std::uint32_t val) noexcept;
    bool writeI32(std::int32_t val) noexcept;
    bool writeUI64(std::uint64_t val) noexcept;
    bool writeI64(std::int64_t val) noexcept;


    // Appends another buffer to the back (zero-copy).
    // Ensures: other becomes empty.
    // Ensures: offset() advances by other.size().
    void append(TByteBuf<TByteBufAllocImpl>&& other) noexcept;
};



using ByteBuf = TByteBuf<ZeroCopyAlloc>;
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
    // Returns negative if outbound queue is full or positive := number of bytes written; 
    // If operation completes successfully returns number of bytes written; buffer `offset` is moved by `number` bytes forward. 
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
    // Appends bytes to dst and increases size().
    // Implementations may use readv into newly allocated pages.
    // Returns:
    //   >0 bytes read
    //    0 EOF
    //   <0 error as -errno (including -EAGAIN/-EWOULDBLOCK for would-block)
    virtual int readSome(ByteBuf& dst) = 0;


    // Writes bytes from src starting at offset(), advancing the cursor by the
    // number of bytes written.
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
        current_frame.append(std::move(e.buf));
        current_frame.seek(0);


        for (;;) {
            std::uint32_t len = 0;
            if (!current_frame.readUI32(len)) {
                current_frame.seek(0);
                break;
            }


            if (current_frame.size() - current_frame.offset() < len) {
                current_frame.seek(0);
                break;
            }


            ByteBuf frame = current_frame.slice(current_frame.offset(), len);
            std::size_t next = current_frame.offset() + len;
            current_frame = current_frame.slice(next, current_frame.size() - next);
            InboundBytes out{std::move(frame)};
            ctx.fireInbound(out);
        }
    }


    void onOutbound(StageContext& ctx, OutboundBytes& e) {
        ByteBuf frame = std::move(e.buf);
        ByteBuf out;
        out.writeUI32(static_cast<uint32_t>(frame.size()));
        out.append(std::move(frame));
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
