// f4-json/include/f4/json/scope.hpp
//
// Comma-managed scope helpers over Writer. The Writer itself stays a
// structure-free string builder (see writer.hpp); these helpers own the
// one thing every hand-rolled emitter got wrong differently: the member
// and element separators. Two guard shapes cover the house styles:
//
//   Members — a run of object members. The separator (", " in record
//             objects, ",\n    " in nested pretty blocks) is emitted
//             before every member except the first.
//   Array   — a pretty array: head, per-element prefix (first vs rest),
//             and an emptiness-aware close ("\n  ]" with elements, "]"
//             without).
//
// The byte layouts these reproduce are pinned by the emitters' tests
// (result_ledger's greps assert exact fragments like "\"attributed\":
// false" and "\"fstatus\": \"0c\"") — the guards exist so the NEXT
// emitter starts from the discipline instead of re-deriving it.

#pragma once

#include <concepts>
#include <string_view>
#include <type_traits>

#include "writer.hpp"

namespace f4::json {

/// A run of object members with a managed separator. `first_emitted`
/// supports records whose opening brace carries the first member inline
/// (the "{\"t_ms\": " idiom).
class Members {
public:
    Members(Writer& w, std::string_view sep, bool first_emitted = false)
        : w_(w), sep_(sep), first_(!first_emitted) {}

    /// Compact integral member: "key":123 (the totals/record idiom).
    template <typename T>
        requires std::is_integral_v<T>
    void key(std::string_view key, T value) {
        sep();
        w_.string(key);
        w_.put(':');
        w_.number(value);
    }

    /// Compact double member: "key":1.5.
    void key(std::string_view key, double value) {
        sep();
        w_.string(key);
        w_.put(':');
        w_.number(value);
    }

    /// Spaced string member: "key": "value" (the human-facing idiom —
    /// names, hex bitmaps, attributions).
    void key(std::string_view key, std::string_view value) {
        sep();
        w_.string(key);
        w_.put(": ");
        w_.string(value);
    }

    /// Spaced boolean member: "key": true.
    void key(std::string_view key, bool value) {
        sep();
        w_.string(key);
        w_.put(": ");
        w_.put(value ? "true" : "false");
    }

    /// Emit just the separator — for members assembled by hand (raw
    /// opens, inline arrays).
    void sep() {
        if (first_) {
            first_ = false;
        } else {
            w_.raw(sep_);
        }
    }

private:
    Writer& w_;
    std::string_view sep_;
    bool first_;
};

/// A pretty array: emits `head` on construction, `first_elem` before
/// the first element (or `next_elem` before the rest), and closes with
/// `close_nonempty`/`close_empty` depending on whether anything was
/// emitted. element() returns a comma-space Members run for the
/// element's object body.
class Array {
public:
    Array(Writer& w, std::string_view head, std::string_view first_elem,
          std::string_view next_elem, std::string_view close_nonempty,
          std::string_view close_empty)
        : w_(w), first_elem_(first_elem), next_elem_(next_elem),
          close_nonempty_(close_nonempty), close_empty_(close_empty) {
        w_.raw(head);
    }

    ~Array() { w_.raw(any_ ? close_nonempty_ : close_empty_); }

    Array(const Array&) = delete;
    Array& operator=(const Array&) = delete;

    /// Prefix for the next element (first vs rest).
    void element_prefix() {
        w_.raw(any_ ? next_elem_ : first_elem_);
        any_ = true;
    }

    /// Prefix + a comma-space member run for the element's object.
    Members element() {
        element_prefix();
        return Members(w_, ", ");
    }

    /// Prefix + an explicit raw element opening (the "{\"t_ms\": "
    /// idiom — the first member rides the opening); the returned run's
    /// first separator is already consumed.
    Members element_opened(std::string_view raw_open) {
        element_prefix();
        w_.raw(raw_open);
        return Members(w_, ", ", /*first_emitted=*/true);
    }

private:
    Writer& w_;
    std::string_view first_elem_;
    std::string_view next_elem_;
    std::string_view close_nonempty_;
    std::string_view close_empty_;
    bool any_ = false;
};

} // namespace f4::json
