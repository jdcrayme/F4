// f4-messaging/include/f4/messaging/f4_messaging.hpp
//
// Umbrella header for f4-messaging — the type-safe message bus and queue
// primitives. Include this to get the full public API.
//
// Components:
//   f4::messaging::MessageBus       — type-indexed handler dispatch with
//                                     same-thread publish() and cross-thread
//                                     publish_deferred() + flush_pending()
//   f4::messaging::MessageQueue<Msg>— thread-safe SPSC queue for the
//                                     "one consumer" pattern
//   f4::messaging::send_to          — cross-bus deferred delivery helper
//
// Zero dependencies. C++20. Header-only (the templates are the API; the
// implementation is a few mutex-protected std::collections).

#pragma once

#include <f4/messaging/bus.hpp>
