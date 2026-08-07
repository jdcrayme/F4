// f4-io/include/f4/io/cursor.hpp
//
// f4::io::Cursor — a sequential little-endian byte reader with a sticky
// error flag, used by the binary parsers in f4-world-convert and
// f4-terrain.
//
// This header consolidates 6 prior anonymous-namespace Cursor structs:
//   * f4-world-convert/src/unit_decoder.cpp        (sticky-flag, the design base)
//   * f4-world-convert/src/theater_data.cpp        (richest API: s8/s16/s32,
//                                                  read_bytes, eof, vector ctor)
//   * f4-world-convert/src/campaign_decoder.cpp    (fixed_string)
//   * f4-world-convert/src/objective_decoder.cpp   (canonical i16/i32/u8/...)
//   * f4-world-convert/src/team_decoder.cpp        (subset of objective)
//   * f4-terrain/src/terrain_data.cpp              (subset, different prefix)
//
// The unified API is a SUPERSET of all 6: every name ever exposed is
// provided here, so consumers can switch with `using f4::io::Cursor;`
// and no call-site changes (beyond adding post-parse error checks).
//
// OOB contract: a sticky `error` flag is set on the first read/skip that
// would overrun the buffer; subsequent reads become no-ops (return 0,
// don't advance `p`). Callers that previously threw on OOB must check
// `c.error` after their parse loop and throw — see /home/z/my-project/work/io_survey.md
// section 1.7 for the design rationale (a single type with sticky-flag
// everywhere is simpler than an ErrorPolicy template, and the only
// non-throwing caller — unit_decoder's subclass dispatch — already uses
// this exact model).
//
// Endianness: reads use std::memcpy into the host's native representation.
// All F4 binary files are little-endian on disk; on little-endian hosts
// (x86/x86_64, ARM little-endian, the only platforms F4 currently
// targets) the bytes round-trip correctly. A future big-endian port
// would need byte-swap helpers — out of scope for this consolidation.
//
// Zero f4-* dependencies. Standard library only.

#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace f4::io {

struct Cursor {
    const uint8_t* p = nullptr;
    const uint8_t* end = nullptr;
    bool error = false;   // sticky; set on any OOB read or skip, never cleared

    // ---- Constructors --------------------------------------------------
    //
    // Two constructors to cover both call patterns in the wild:
    //   * direct pointer pair (sub-slice of a larger buffer)
    //   * whole std::vector<uint8_t> (the common "load whole file" case)
    //
    // The default member initializers for `p`/`end`/`error` keep the
    // default constructor (and aggregate-style usage) safe — a default-
    // constructed Cursor sits at eof() with remaining()==0 and no error.
    Cursor(const uint8_t* p_, const uint8_t* end_) noexcept
        : p(p_), end(end_), error(false) {}

    explicit Cursor(const std::vector<uint8_t>& buf) noexcept
        : p(buf.data()), end(buf.data() + buf.size()), error(false) {}

    Cursor() noexcept = default;

    // L7 FIX: Cursor is non-copyable and non-movable. Copying would create
    // two cursors sharing the same buffer, both advancing independently —
    // a logic error in general. Moving is blocked by const members.
    //
    // For trial-parse patterns (e.g. subclass dispatch), use save/restore
    // instead of copying the Cursor. See Position below.
    Cursor(const Cursor&) = delete;
    Cursor& operator=(const Cursor&) = delete;

    // ---- Save/Restore position ----------------------------------------
    //
    // Allows trial-parse patterns to snapshot the cursor state, attempt
    // a parse, and either commit (leave cursor advanced) or rollback
    // (restore to saved position). This is the non-copyable alternative
    // to "Cursor trial = original; parse(trial); if (fail) discard trial;".
    //
    // Usage in a try-parse pattern:
    //
    //     auto saved = c.save();
    //     parse(c, out);              // advances c
    //     if (!c.error && valid) {
    //         // commit — c is already advanced
    //     } else {
    //         c.restore(saved);       // rollback to pre-parse state
    //     }
    struct Position {
        const uint8_t* p;
        bool error;
    };

    [[nodiscard]] Position save() const noexcept { return Position{p, error}; }
    void restore(const Position& pos) noexcept { p = pos.p; error = pos.error; }

    // ---- Bulk reads / skips --------------------------------------------

    // Read `n` bytes into `dst`. On OOB (or after a prior error), sets the
    // sticky flag and returns without writing to `dst` or advancing `p`.
    void read(void* dst, std::size_t n) {
        if (error || p + n > end) { error = true; return; }
        std::memcpy(dst, p, n);
        p += n;
    }

    // Alias for read() — the theater_data Cursor named the bulk read
    // `read_bytes`; preserve both names so neither call site needs to change.
    void read_bytes(uint8_t* dst, std::size_t n) { read(dst, n); }

    // Advance `p` by `n` without reading. Bounds-checked identically to
    // read() — sets the sticky flag and leaves `p` unchanged on OOB.
    void skip(std::size_t n) {
        if (error || p + n > end) { error = true; return; }
        p += n;
    }

    // ---- Typed little-endian readers -----------------------------------
    //
    // Every name ever used by the 6 prior Cursors is provided:
    //   u8/s8/i8, u16/s16/i16, u32/s32/i32, f32.
    // The `i*` spellings are aliases for the `s*` ones (the same int16_t
    // return type); both sets work without preference.
    //
    // On OOB, sets the sticky flag, returns 0, and leaves `p` unchanged.

    uint8_t  u8 () { uint8_t  v = 0; read(&v, 1); return v; }
    int8_t   s8 () { int8_t   v = 0; read(&v, 1); return v; }
    int8_t   i8 () { return s8(); }
    uint16_t u16() { uint16_t v = 0; read(&v, 2); return v; }
    int16_t  s16() { int16_t  v = 0; read(&v, 2); return v; }
    int16_t  i16() { return s16(); }
    uint32_t u32() { uint32_t v = 0; read(&v, 4); return v; }
    int32_t  s32() { int32_t  v = 0; read(&v, 4); return v; }
    int32_t  i32() { return s32(); }
    float    f32() { float    v = 0; read(&v, 4); return v; }

    // ---- Fixed-width NUL-terminated string -----------------------------
    //
    // Reads exactly `n` bytes (advancing `p` by `n`), scans for the first
    // NUL within those bytes, and returns the prefix as a std::string.
    // Matches the campaign_decoder.cpp `fixed_string` semantics: the field
    // is a fixed-width char array, NUL-terminated within it; trailing NULs
    // (and any bytes after the first NUL) are NOT included in the result.
    //
    // On OOB, sets the sticky flag and returns an empty string.
    std::string fixed_string(std::size_t n) {
        if (error || p + n > end) { error = true; return {}; }
        std::size_t len = 0;
        while (len < n && p[len] != 0) ++len;
        std::string s(reinterpret_cast<const char*>(p), len);
        p += n;
        return s;
    }

    // ---- State ---------------------------------------------------------

    // True once `p` reaches `end`. Cheap (no read).
    [[nodiscard]] bool eof() const noexcept { return p >= end; }

    // Bytes still unread. Returns 0 when eof() (or when p > end after an
    // OOB — callers should check `error` first).
    // H9 FIX: Returns 0 instead of SIZE_MAX when p > end (unsigned
    // underflow). Previously `static_cast<size_t>(end - p)` would wrap
    // to ~2^64 when p > end after an OOB read, which could be used as
    // a loop bound causing catastrophic iteration.
    [[nodiscard]] std::size_t remaining() const noexcept {
        return (p <= end) ? static_cast<std::size_t>(end - p) : 0;
    }

    // ---- Sticky-error helper ------------------------------------------
    //
    // Throws std::runtime_error if the sticky error flag is set, including
    // the caller's `context` string in the message. This is the recommended
    // way to convert the silent sticky-flag into an observable exception
    // at the end of a parse block, instead of forcing every caller to
    // write `if (c.error) throw std::runtime_error(...)` by hand.
    //
    // Use after each logical block of reads:
    //
    //     Cursor c{buf.data(), buf.data() + buf.size()};
    //     h.field_a = c.i32();
    //     h.field_b = c.i32();
    //     h.name    = c.fixed_string(NAME_LEN);
    //     c.check_and_throw("header: payload truncated");  // converts sticky to throw
    //
    // If `error` is not set, this is a no-op.
    //
    // Rationale: the original Cursor design (worklog.md:1504) chose
    // sticky-flag over throw-on-OOB to surface real bugs in subclass
    // dispatch paths where exceptions would be caught and swallowed. But
    // the consequence was that any caller who forgot to check `error`
    // would silently produce zeroed records. check_and_throw() gives
    // those callers a one-line idiomatic check, and the existing throw-
    // style call sites (campaign_decoder, objective_decoder, etc.) can
    // be progressively migrated to use it for consistency.
    void check_and_throw(const char* context) const {
        if (error) {
            throw std::runtime_error(std::string(context));
        }
    }

    // Convenience overload for string literals — avoids the explicit
    // std::string construction at every call site.
    void check_and_throw(const std::string& context) const {
        if (error) {
            throw std::runtime_error(context);
        }
    }
};

} // namespace f4::io
