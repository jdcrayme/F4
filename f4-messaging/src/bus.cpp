// f4-messaging/src/bus.cpp
//
// Today f4-messaging is header-only — all of MessageBus and MessageQueue
// is templated, and the non-template bits (the type_index hash) live in
// the standard library. This .cpp exists so the library has a targettable
// translation unit for future ABI surfaces (e.g. a shared trace recorder
// that hooks into publish_deferred for cross-thread debugging), and so
// `add_library(f4-messaging STATIC src/bus.cpp)` follows the same pattern
// as f4-entities / f4-install rather than being a INTERFACE library.
//
// Leaving it as an empty TU is intentional. When the first non-template
// function lands, it goes here.

namespace f4::messaging {
// (intentionally empty — see file header)
} // namespace f4::messaging
