// f4-campaign-api/include/f4/campaign/api/command_journal.hpp
//
// The COMMAND journal (CAMP_HOST_PLAN.md §2.3/§5, CAMP-CMD-1): the
// complete record of one session's applied interventions, as append-only
// JSONL. "Observation is free; intervention is journaled" — every APPLIED
// command lands here with the engine tick it applied at, and replay is
// (save, seed, command journal) → the same identity fingerprint. Refused
// commands are NOT journaled: they mutate nothing, so replaying them
// could not change the books — the refusal rode the wire as data at
// record time and the journal keeps the DETERMINISM record, not the
// audit log.
//
// The file is THREE kinds of lines, in order:
//
//   {"v":1,"cmdjournal":1,"identity":{...}}   ← the session-start
//                                               fingerprint (hello's)
//   {"apply_tick":120,"t":38574480,"intent":"roe_set","scope":{...},
//    "roe":2}                                 ← one line per APPLIED
//                                               command, in apply order
//   {"journal_end":{...}}                     ← the session-end
//                                               fingerprint
//
// The tick axis: `apply_tick` is the ENGINE TICK INDEX — whole sim_dt
// steps since session start — not campaign seconds. The host owns the
// stepping accumulator, so its tick count is the one clock a replay can
// reproduce EXACTLY (campaign seconds truncate sub-second ticks; the
// fire-control state a command meets lives at tick granularity). `t` is
// the campaign seconds the record's ack carried — audit context only,
// never used by replay.
//
// Replay semantics: a line applies when the session's tick index reaches
// its apply_tick — the host steps in segments around the pending ticks,
// so the replay reproduces the record's command placement regardless of
// how the replay's step requests were chunked. The footer's identity is
// the assertion target: the replayed session's final fingerprint must
// equal it (ledger_fnv = the books; "replay reproduces the books" is a
// byte comparison, exit 23 in the reference host).
//
// Byte discipline: the header is compared BYTE-FOR-BYTE at open (the
// event journal's rule — a parser would forgive a drift the bytes
// catch); the entry lines are PARSED (a replay consumes them) with the
// canonical key order enforced and ticks required non-decreasing.
//
// std only (f4-json + std, the contract library's dependency rule).

#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <f4/campaign/api/commands.hpp>
#include <f4/campaign/api/identity.hpp>
#include <f4/json/reader.hpp>
#include <f4/json/writer.hpp>

namespace f4::campaign::api {

// --- the line builders (public for the byte goldens) ---------------------

/// One journaled command: where it applied (tick index + campaign
/// seconds) and what applied.
struct CommandJournalEntry {
    std::uint64_t apply_tick{0};   ///< engine tick index at apply
    std::int64_t campaign_time_s{0};  ///< the record's ack apply_tick
    CommandIntent intent{};
};

/// The header line: the journal dialect + the session-start identity.
inline void encode_command_journal_header(
    f4::json::Writer& w, const IdentityFingerprint& at_start) {
    w.raw("{\"v\":1,\"cmdjournal\":1,\"identity\":");
    encode_identity(w, at_start);
    w.put('}');
}

/// One command line: {"apply_tick":T,"t":S,<body>} (the body's encoder
/// in commands.hpp writes the intent name + payload keys).
inline void encode_command_journal_line(
    f4::json::Writer& w, const CommandJournalEntry& e) {
    w.raw("{\"apply_tick\":");
    w.number(static_cast<std::uint64_t>(e.apply_tick));
    w.raw(",\"t\":");
    w.number(static_cast<long long>(e.campaign_time_s));
    w.put(',');
    encode_command_body(w, e.intent);
    w.put('}');
}

/// The closing line: the session-end identity (the replay's target).
inline void encode_command_journal_end(f4::json::Writer& w,
                                       const IdentityFingerprint& at_end) {
    w.raw("{\"journal_end\":");
    encode_identity(w, at_end);
    w.put('}');
}

// --- the writer -----------------------------------------------------------

class CommandJournalWriter {
public:
    /// Open + write the header. Fails (false + `error`) when the file
    /// cannot be opened — the reference host maps that to exit 24.
    bool open(const std::string& path, const IdentityFingerprint& at_start,
              std::string* error = nullptr) {
        out_ = std::make_unique<std::ofstream>(path, std::ios::binary);
        if (!out_->good()) {
            detail_ = "cannot open " + path;
            if (error != nullptr) *error = detail_;
            out_.reset();
            return false;
        }
        detail_ = path;
        write_line([&](f4::json::Writer& w) {
            encode_command_journal_header(w, at_start);
        });
        return out_->good();
    }

    /// Append one APPLIED command as its line. IO failures mark the
    /// writer bad (ok() false) — the reference host checks at close.
    void append(const CommandJournalEntry& e) {
        if (out_ == nullptr) return;
        write_line([&e](f4::json::Writer& w) {
            encode_command_journal_line(w, e);
        });
    }

    /// Append the closing identity line and flush. The writer stays
    /// usable only for ok()/detail() after this.
    bool close(const IdentityFingerprint& at_end, std::string* error = nullptr) {
        if (out_ == nullptr) {
            if (error != nullptr && detail_.empty()) {
                *error = "command journal not open";
            }
            return false;
        }
        write_line([&](f4::json::Writer& w) {
            encode_command_journal_end(w, at_end);
        });
        out_->flush();
        const bool good = out_->good() && !failed_;
        if (!good && error != nullptr) {
            *error = "command journal write failed: " + detail_;
        }
        out_.reset();
        return good;
    }

    [[nodiscard]] bool ok() const noexcept {
        return out_ != nullptr && out_->good() && !failed_;
    }
    [[nodiscard]] const std::string& detail() const noexcept {
        return detail_;
    }

private:
    template <typename Builder>
    void write_line(Builder&& build) {
        if (out_ == nullptr) return;
        f4::json::Writer w;
        build(w);
        w.put('\n');
        const auto& s = w.str();
        out_->write(s.data(), static_cast<std::streamsize>(s.size()));
        if (!out_->good()) failed_ = true;
    }

    std::unique_ptr<std::ofstream> out_;
    bool failed_{false};
    std::string detail_;
};

// --- the reader -----------------------------------------------------------

/// Loads a recorded journal for replay: open() byte-checks the header
/// against the ADOPTING session's start identity (a mismatch is identity
/// drift — the journal is not this war), next() yields the entries in
/// file order, and the footer's identity is captured for the replay's
/// final assertion. Ticks must be non-decreasing (the record's apply
/// order IS the replay's apply order).
class CommandJournalReader {
public:
    enum class State : std::uint8_t {
        Empty,    ///< open() not called yet
        Reading,  ///< entries remain
        Ended,    ///< journal_end consumed (footer_seen() true)
        Failed,   ///< malformed / non-monotonic / truncated
    };

    bool open(const std::string& path,
              const IdentityFingerprint& expected_start,
              std::string* error = nullptr) {
        in_.open(path, std::ios::binary);
        if (!in_.good()) {
            return fail("cannot open " + path, error);
        }
        // byte-exact header check (the event journal's rule)
        std::string actual;
        if (!std::getline(in_, actual)) {
            return fail("command journal is empty", error);
        }
        f4::json::Writer w;
        encode_command_journal_header(w, expected_start);
        if (w.str() != actual) {
            return fail("command journal line 1 is not this session's "
                            "identity\n  want: " +
                            w.str() + "\n  got:  " + actual,
                        error);
        }
        line_ = 1;  // the header consumed golden line 1
        state_ = State::Reading;
        return true;
    }

    /// The next entry, in file order. False means: Ended (the footer was
    /// consumed — the healthy stop) or Failed (check state()/detail()).
    bool next(CommandJournalEntry& e, std::string* error = nullptr) {
        if (state_ != State::Reading) {
            return fail_detail("command journal is not open for reading",
                               error);
        }
        std::string line;
        if (!std::getline(in_, line)) {
            return fail("command journal ended without journal_end at "
                            "line " + std::to_string(line_ + 1),
                        error);
        }
        ++line_;
        // the footer?
        constexpr std::string_view kFooter = R"({"journal_end":)";
        if (line.rfind(kFooter, 0) == 0) {
            if (!parse_footer_(line)) {
                return fail("command journal line " +
                                std::to_string(line_) +
                                " has a malformed journal_end footer",
                            error);
            }
            state_ = State::Ended;
            return false;
        }
        if (!parse_entry_(line, e)) {
            return fail("command journal line " + std::to_string(line_) +
                            " is malformed: " + parse_error_,
                        error);
        }
        if (!entries_.empty() && e.apply_tick < entries_.back().apply_tick) {
            return fail("command journal line " + std::to_string(line_) +
                            " ticks backwards (" +
                            std::to_string(e.apply_tick) + " < " +
                            std::to_string(entries_.back().apply_tick) +
                            ")",
                        error);
        }
        entries_.push_back(e);
        return true;
    }

    /// All entries at once (open + drain + the healthy-end check in one
    /// call — the shape a replay host wants). Any Failed state returns
    /// false and names the line.
    [[nodiscard]] static bool load(
        const std::string& path, const IdentityFingerprint& expected_start,
        std::vector<CommandJournalEntry>& out_entries,
        IdentityFingerprint& out_end, std::string* error = nullptr) {
        CommandJournalReader r;
        if (!r.open(path, expected_start, error)) return false;
        CommandJournalEntry e;
        while (r.next(e, error)) out_entries.push_back(e);
        if (r.state_ != State::Ended) {
            if (error != nullptr && error->empty()) {
                *error = r.detail_;
            }
            return false;
        }
        out_end = r.end_identity_;
        return true;
    }

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] bool footer_seen() const noexcept {
        return state_ == State::Ended;
    }
    [[nodiscard]] const IdentityFingerprint& end_identity() const noexcept {
        return end_identity_;
    }
    [[nodiscard]] const std::vector<CommandJournalEntry>&
    entries() const noexcept {
        return entries_;
    }
    [[nodiscard]] const std::string& detail() const noexcept {
        return detail_;
    }

private:
    // {"apply_tick":T,"t":S,"intent":"NAME",<body>} — the envelope keys
    // may ride in any order BEFORE intent; the intent's body closes the
    // line (parse_command_body consumes its keys + the brace).
    bool parse_entry_(const std::string& line, CommandJournalEntry& e) {
        try {
            f4::json::Reader r(line);
            r.expect('{');
            e = CommandJournalEntry{};
            bool have_intent = false;
            if (r.consume('}')) {
                parse_error_ = "empty line";
                return false;
            }
            while (true) {
                const auto key = r.read_string();
                r.expect(':');
                if (key == "apply_tick") {
                    const auto v = r.read_int();
                    if (v < 0) {
                        parse_error_ = "negative apply_tick";
                        return false;
                    }
                    e.apply_tick = static_cast<std::uint64_t>(v);
                } else if (key == "t") {
                    e.campaign_time_s = r.read_int();
                } else if (key == "intent") {
                    const auto intent_name = r.read_string();
                    e.intent = parse_command_body(intent_name, r);
                    have_intent = true;
                    break;  // the body consumed the closing brace
                } else {
                    parse_error_ = "unknown key " + key;
                    return false;
                }
                if (r.consume('}')) break;  // closed before any intent
                r.expect(',');
            }
            if (!have_intent) {
                parse_error_ = "no intent";
                return false;
            }
            return true;
        } catch (const std::exception& ex) {
            parse_error_ = ex.what();
            return false;
        }
    }

    // {"journal_end":{"protocol":P,"campaign_time_s":T,"ledger_fnv":"X"}}
    bool parse_footer_(const std::string& line) {
        try {
            f4::json::Reader r(line);
            r.expect('{');
            const auto key = r.read_string();
            if (key != "journal_end") {
                parse_error_ = "expected journal_end, got " + key;
                return false;
            }
            r.expect(':');
            r.expect('{');
            end_identity_ = IdentityFingerprint{};
            if (!r.consume('}')) {
                while (true) {
                    const auto k2 = r.read_string();
                    r.expect(':');
                    if (k2 == "protocol") {
                        end_identity_.protocol_version =
                            static_cast<std::uint32_t>(r.read_int());
                    } else if (k2 == "campaign_time_s") {
                        end_identity_.campaign_time_s = r.read_int();
                    } else if (k2 == "ledger_fnv") {
                        end_identity_.ledger_fnv = r.read_string();
                    } else {
                        parse_error_ = "unknown identity key " + k2;
                        return false;
                    }
                    if (r.consume('}')) break;
                    r.expect(',');
                }
            }
            return true;
        } catch (const std::exception& ex) {
            parse_error_ = ex.what();
            return false;
        }
    }

    bool fail(const std::string& what, std::string* error) {
        state_ = State::Failed;
        detail_ = what;
        if (error != nullptr) *error = what;
        return false;
    }

    bool fail_detail(const std::string& what, std::string* error) {
        detail_ = what;
        if (error != nullptr) *error = what;
        return false;
    }

    std::ifstream in_;
    State state_{State::Empty};
    std::uint64_t line_{0};
    IdentityFingerprint end_identity_{};
    std::vector<CommandJournalEntry> entries_;
    std::string detail_;
    std::string parse_error_;
};

} // namespace f4::campaign::api
