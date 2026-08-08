// f4-messaging/include/f4/messaging/bus.hpp
//
// Type-safe message bus with explicit thread boundaries.
//
// Replaces FreeFalcon's 76+ FalconEvent subclasses (each with #pragma pack(1)
// DATA_BLOCKs and manual Encode/Decode, routed via switch(FalconMsgID)) with
// three small primitives:
//
//   - Messages are plain structs. No inheritance, no pack pragmas, no manual
//     serialization. If you can copy a struct, you can send it.
//   - A MessageBus is a type-indexed handler table. Subscribe by type,
//     publish by type. Type safety comes from std::type_index + a thin
//     type-erased wrapper — no casts in user code.
//   - Cross-thread traffic goes through publish_deferred() + flush_pending().
//     The publishing thread enqueues work; the owning thread drains it. This
//     is the §9.3 pattern: each subsystem owns a bus, each tick starts with
//     flush_pending().
//
// Why not lock-free? The original design target is 60 Hz sim + 1 Hz campaign.
// A 1 Hz campaign tick producing a few dozen mission assignments per second
// does not need a lock-free queue; it needs correctness. A mutex-protected
// std::vector<std::function> is correct, easy to reason about, and shows up
// as ~0% in profiles. Lock-free is deferred until a profiler says otherwise.
//
// C1 FIX: Replaced recursive_mutex + vector-copy-on-publish with
// shared_mutex + copy-on-write (shared_ptr<const vector>). publish() now
// takes a shared lock (concurrent reads) and reads a shared_ptr without
// allocating. subscribe()/unsubscribe() take exclusive locks and swap in
// a new vector. Reentrant publish is handled by deferring the inner
// publish to a thread-local reentry list, which is drained after the
// outer publish completes. This eliminates the per-publish vector copy
// and the ~2x overhead of recursive_mutex on Linux/glibc.
//
// Dependencies: standard library only. C++20.

#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <queue>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace f4::messaging {

// ============================================================================
// MessageBus — type-indexed handler dispatch with cross-thread queuing.
//
// Threading model:
//   - subscribe() and unsubscribe() take an exclusive lock on handlers_mutex_.
//     Wire up subscriptions on a single thread at startup.
//   - publish() takes a shared lock (concurrent reads). Multiple threads
//     may call publish() simultaneously as long as no subscribe/unsubscribe
//     is happening concurrently for the same Msg type.
//   - publish_deferred() IS thread-safe by design — that's the whole point.
//     Multiple producer threads can enqueue concurrently.
//   - flush_pending() must be called from the owning (consumer) thread.
//
// Reentrant publish safety:
//   A handler called from publish() may itself call publish() on the same
//   bus (e.g. a stall handler publishing a warning). Since publish() now
//   uses a shared lock (not recursive), reentrant publish would deadlock
//   on the shared_mutex. Instead, reentrant calls are deferred to a
//   thread-local reentry list that is drained after the outer publish
//   completes. This is safe because: (1) the shared lock is held during
//   handler dispatch, so no subscribe/unsubscribe can interleave; (2) the
//   deferred messages see a consistent handler snapshot.
//
// Handler storage:
//   Handlers are stored as shared_ptr<const vector<HandlerFn>> keyed by
//   std::type_index(typeid(Msg)). subscribe()/unsubscribe() create a new
//   vector and swap the shared_ptr (copy-on-write). publish() reads the
//   current shared_ptr under a shared lock — zero allocation per publish.
//   The type-erased call wraps a std::function<void(const Msg&)> so user
//   code stays typed.
// ============================================================================
class MessageBus {
public:
    using HandlerFn = std::function<void(const void*)>;

    MessageBus() = default;
    ~MessageBus() = default;

    MessageBus(const MessageBus&) = delete;
    MessageBus& operator=(const MessageBus&) = delete;
    // Move operations deleted: shared_mutex is not movable, and the
    // thread-local reentry list references `this` which would be stale
    // after move.
    MessageBus(MessageBus&&) = delete;
    MessageBus& operator=(MessageBus&&) = delete;

    // ------------------------------------------------------------------------
    // subscribe(handler): register a handler for message type Msg.
    // Returns a subscription ID that can be passed to unsubscribe().
    //
    // Multiple handlers per type are supported; they fire in registration
    // order on publish(). This mirrors the original VuMessage fan-out
    // semantics (a damage event might notify both the target's damage
    // model and the campaign's kill tracker).
    //
    // NOT thread-safe relative to publish() / unsubscribe() on the same
    // type. Call during setup, before any publish.
    // ------------------------------------------------------------------------
    template <typename Msg>
    std::size_t subscribe(std::function<void(const Msg&)> handler) {
        auto wrapped = [h = std::move(handler)](const void* raw) {
            h(*static_cast<const Msg*>(raw));
        };
        std::unique_lock lock(handlers_mutex_);
        auto key = std::type_index(typeid(Msg));
        auto it = handlers_.find(key);
        if (it == handlers_.end()) {
            auto vec = std::make_shared<std::vector<HandlerFn>>();
            vec->push_back(std::move(wrapped));
            handlers_[key] = std::move(vec);
            return 0;
        }
        auto& vec = it->second;
        // Copy-on-write: create a new vector with the added handler.
        auto new_vec = std::make_shared<std::vector<HandlerFn>>(*vec);
        std::size_t id = new_vec->size();
        new_vec->push_back(std::move(wrapped));
        it->second = std::move(new_vec);
        return id;
    }

    // Convenience overload for stateless lambdas / function pointers.
    template <typename Msg, typename Callable>
    std::size_t subscribe(Callable&& handler) {
        return subscribe<Msg>(std::function<void(const Msg&)>(
            std::forward<Callable>(handler)));
    }

    // ------------------------------------------------------------------------
    // unsubscribe(id): remove a handler. O(n) in the number of handlers for
    // the type — typically small (1-3). The subscription ID is the index
    // returned by subscribe(). After unsubscribe, the slot is nulled but
    // the IDs of other handlers are unchanged (we don't compact).
    // ------------------------------------------------------------------------
    template <typename Msg>
    void unsubscribe(std::size_t id) {
        std::unique_lock lock(handlers_mutex_);
        auto it = handlers_.find(std::type_index(typeid(Msg)));
        if (it == handlers_.end()) return;
        // Copy-on-write: null the slot in a new vector, then swap.
        auto new_vec = std::make_shared<std::vector<HandlerFn>>(*it->second);
        if (id < new_vec->size()) (*new_vec)[id] = nullptr;
        it->second = std::move(new_vec);
    }

    // ------------------------------------------------------------------------
    // publish(msg): deliver msg to every subscriber of Msg on the calling
    // thread, synchronously. No queueing. This is the hot path — at
    // 60 Hz x N entities, this is what runs every frame.
    //
    // Thread-safety: safe to call concurrently with other publish() calls
    // and with publish_deferred(). NOT safe to call concurrently with
    // subscribe() / unsubscribe() for the same Msg type.
    // ------------------------------------------------------------------------
    template <typename Msg>
    void publish(const Msg& msg) {
        SharedHandlerList snapshot;
        {
            std::shared_lock lock(handlers_mutex_);
            auto it = handlers_.find(std::type_index(typeid(Msg)));
            if (it == handlers_.end()) return;
            snapshot = it->second;  // shared_ptr copy — no vector allocation
        }
        // If we're already inside a publish() on this thread (reentrant
        // publish), defer to avoid shared_mutex deadlock.
        auto& reentry = reentry_list();
        if (!reentry.inside) {
            reentry.inside = true;
            for (const auto& h : *snapshot) {
                if (h) h(&msg);
            }
            // Drain any deferred reentrant publishes.
            while (!reentry.pending.empty()) {
                auto fn = std::move(reentry.pending.front());
                reentry.pending.pop_front();
                fn();
            }
            reentry.inside = false;
        } else {
            // Reentrant: defer until the outer publish drains.
            //
            // CRITICAL: capture msg BY VALUE, not by reference. The
            // reentrant publish() call returns before the drain loop
            // runs (the drain happens in the OUTER publish). If we
            // captured &msg, it would dangle — pointing to the inner
            // publish's stack parameter, destroyed when the inner call
            // returned. This was the root cause of garbage aircraft_id
            // values in the StubATC taxi-clearance round-trip test.
            reentry.pending.push_back([this, snapshot, msg]() {
                for (const auto& h : *snapshot) {
                    if (h) h(&msg);
                }
            });
        }
    }

    // ------------------------------------------------------------------------
    // publish_deferred(msg): enqueue msg for delivery on the owning thread
    // when it next calls flush_pending(). The message is COPIED once into
    // a type-erased callable that, when invoked, looks up the CURRENT
    // handlers for Msg and dispatches to each. This means subscriptions
    // added between enqueue and flush will fire, and unsubscriptions will
    // take effect — the snapshot is taken at flush, not at enqueue.
    //
    // Thread-safe by design — this is the cross-thread primitive. Multiple
    // producer threads can call concurrently.
    // ------------------------------------------------------------------------
    template <typename Msg>
    void publish_deferred(Msg msg) {
        // Capture the message by value (one copy) and bind a callable that
        // re-enters the bus's dispatch path at flush time. The handler
        // lookup is deferred to flush so subscribe/unsubscribe between
        // enqueue and flush actually take effect.
        auto m = std::make_shared<Msg>(std::move(msg));
        std::function<void()> job = [this, m]() {
            this->publish(*m);
        };
        std::lock_guard lock(pending_mutex_);
        pending_.push_back(std::move(job));
    }

    // ------------------------------------------------------------------------
    // flush_pending(): deliver all deferred messages on the calling thread.
    // Swap-then-drain so a handler that itself calls publish_deferred()
    // enqueues into a fresh empty pending_ vector — its deliveries land on
    // the NEXT flush, not mid-iteration. This prevents unbounded recursion
    // when a handler's response is itself a deferred message.
    // ------------------------------------------------------------------------
    void flush_pending() {
        std::vector<std::function<void()>> to_flush;
        {
            std::lock_guard lock(pending_mutex_);
            std::swap(to_flush, pending_);
        }
        for (auto& fn : to_flush) {
            if (fn) fn();
        }
    }

    // ------------------------------------------------------------------------
    // send_to(target, msg): convenience for cross-bus deferred delivery.
    // Equivalent to target.publish_deferred(msg); named after the §9.3
    // spec's send_to() to make intent at call sites obvious
    // ("campaign -> sim" reads better than "sim.publish_deferred(...)").
    // ------------------------------------------------------------------------
    template <typename Msg>
    friend void send_to(MessageBus& target, const Msg& msg) {
        target.publish_deferred(msg);
    }

    // ------------------------------------------------------------------------
    // pending_count(): number of deferred deliveries waiting. Diagnostic
    // only — used by tests and by the sim's "did we drain everything this
    // tick?" check. NOT a synchronization primitive.
    // ------------------------------------------------------------------------
    [[nodiscard]] std::size_t pending_count() const {
        std::lock_guard lock(pending_mutex_);
        return pending_.size();
    }

    // ------------------------------------------------------------------------
    // handler_count<Msg>(): number of subscribed handlers for type Msg.
    // Diagnostic only — used by tests to verify subscribe/unsubscribe.
    // ------------------------------------------------------------------------
    template <typename Msg>
    [[nodiscard]] std::size_t handler_count() const {
        std::shared_lock lock(handlers_mutex_);
        auto it = handlers_.find(std::type_index(typeid(Msg)));
        if (it == handlers_.end()) return 0;
        std::size_t n = 0;
        for (const auto& h : *it->second) if (h) ++n;
        return n;
    }

private:
    using SharedHandlerList = std::shared_ptr<std::vector<HandlerFn>>;

    // Reentrant publish support: thread-local deferred list.
    struct ReentryList {
        bool inside = false;
        std::deque<std::function<void()>> pending;
    };
    static ReentryList& reentry_list() {
        thread_local ReentryList rl;
        return rl;
    }

    mutable std::shared_mutex handlers_mutex_;
    std::unordered_map<std::type_index, SharedHandlerList> handlers_;

    mutable std::mutex pending_mutex_;
    std::vector<std::function<void()>> pending_;
};

// ============================================================================
// MessageQueue<Msg> — thread-safe single-producer/single-consumer queue for
// the "I just want a queue of things to process" pattern. Use this when
// you don't need fan-out (one consumer), as opposed to MessageBus (N
// consumers). Simpler than MessageBus for cases like:
//   - Per-entity command queue (each entity drains its own queue)
//   - Per-thread work queue (one worker thread)
//
// push() is multi-producer safe; drain() is single-consumer (or externally
// serialized). Matches the §9.2 spec verbatim.
// ============================================================================
template <typename Msg>
class MessageQueue {
public:
    void push(Msg msg) {
        std::lock_guard lock(mutex_);
        queue_.push(std::move(msg));
    }

    // Atomically drain the entire queue into a vector. Returns an empty
    // vector if nothing was pending.
    std::vector<Msg> drain() {
        std::vector<Msg> batch;
        std::lock_guard lock(mutex_);
        batch.reserve(queue_.size());
        while (!queue_.empty()) {
            batch.push_back(std::move(queue_.front()));
            queue_.pop();
        }
        return batch;
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

private:
    mutable std::mutex mutex_;
    std::queue<Msg> queue_;
};

} // namespace f4::messaging
