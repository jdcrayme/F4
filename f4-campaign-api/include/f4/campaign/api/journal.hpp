// f4-campaign-api/include/f4/campaign/api/journal.hpp
//
// The event journal (CAMP_HOST_PLAN.md §5/§8 CAMP-HOST-2): the
// engine-rate, COMPLETE record of one session's event stream, as
// append-only JSONL. The journal and the wire are two sinks of the same
// bus stream; the journal never filters — replay assertions are only as
// honest as the stream they read.
//
// The file is THREE kinds of lines, in order:
//
//   {"v":1,"journal":1,"identity":{...}}        ← the session-start
//                                                 fingerprint (hello's)
//   {"ev":"tasking_cycle","t":3600,...}         ← the pinned event
//   {...}                                       ← encoders, one per line
//   {"journal_end":{...}}                       ← the session-end
//                                                 fingerprint
//
// Byte discipline: the verifier does NOT parse JSON — it compares the
// golden file's lines against the freshly encoded ones byte-for-byte
// (the house's byte-identity rule; a parser would forgive a drift the
// bytes would catch). Replay identity (plan §5): the same
// (save, seed, journal) reproduces every line INCLUDING both identity
// lines — the closing line carries the ledger fingerprint, so "the
// journal replay reproduces the ledger MD5" is checkable by diffing
// two files, and `campaignd --verify-journal` turns a divergence into
// exit 23.
//
// std only (the f4-json + std dependency rule holds — the encoders ride
// f4-json, the files ride <fstream>).

#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <utility>

#include <f4/campaign/api/events.hpp>
#include <f4/campaign/api/identity.hpp>
#include <f4/json/writer.hpp>

namespace f4::campaign::api {

// --- the line builders (public for the byte goldens) ---------------------

/// The header line: the journal dialect + the session-start identity.
inline void encode_journal_header(f4::json::Writer& w,
                                  const IdentityFingerprint& at_start) {
    w.raw("{\"v\":1,\"journal\":1,\"identity\":");
    encode_identity(w, at_start);
    w.put('}');
}

/// The closing line: the session-end identity (the ledger fingerprint
/// at EOF — the replay's target value).
inline void encode_journal_end(f4::json::Writer& w,
                               const IdentityFingerprint& at_end) {
    w.raw("{\"journal_end\":");
    encode_identity(w, at_end);
    w.put('}');
}

// --- the writer -----------------------------------------------------------

class EventJournalWriter {
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
            encode_journal_header(w, at_start);
        });
        return out_->good();
    }

    /// Append one event as its line. IO failures mark the writer bad
    /// (ok() false) — the reference host checks at close.
    void append(const CampaignEvent& e) {
        if (out_ == nullptr) return;
        write_line([&e](f4::json::Writer& w) { encode(w, e); });
    }

    /// Append the closing identity line and flush. The writer stays
    /// usable only for ok()/detail() after this.
    bool close(const IdentityFingerprint& at_end, std::string* error = nullptr) {
        if (out_ == nullptr) {
            if (error != nullptr && detail_.empty()) {
                *error = "journal not open";
            }
            return false;
        }
        write_line([&](f4::json::Writer& w) {
            encode_journal_end(w, at_end);
        });
        out_->flush();
        const bool good = out_->good() && !failed_;
        if (!good && error != nullptr) {
            *error = "journal write failed: " + detail_;
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

// --- the verifier ---------------------------------------------------------

/// Streams a golden journal against a LIVE run: open() asserts the
/// header, expect() each event line in order, close() the closing
/// identity — and that the golden had nothing left over. Every
/// comparison is byte-exact; the first divergence names its line.
class EventJournalVerifier {
public:
    bool open(const std::string& path, const IdentityFingerprint& at_start,
              std::string* error = nullptr) {
        in_.open(path, std::ios::binary);
        if (!in_.good()) {
            return fail("cannot open " + path, error);
        }
        // expect_line counts: the header is golden line 1
        return expect_line([&](f4::json::Writer& w) {
            encode_journal_header(w, at_start);
        }, error);
    }

    /// Assert the next golden line is exactly this event.
    bool expect(const CampaignEvent& e, std::string* error = nullptr) {
        return expect_line([&e](f4::json::Writer& w) { encode(w, e); },
                           error);
    }

    /// Assert the closing identity — and that the golden ends here (a
    /// golden with lines remaining after journal_end is itself drift:
    /// the live run produced FEWER events than the record).
    bool close(const IdentityFingerprint& at_end, std::string* error = nullptr) {
        if (!expect_line([&](f4::json::Writer& w) {
                encode_journal_end(w, at_end);
            }, error)) {
            return false;
        }
        std::string extra;
        if (std::getline(in_, extra)) {
            return fail("golden continues past journal_end at line " +
                            std::to_string(line_ + 1),
                        error);
        }
        return true;
    }

    /// The 1-based golden line of the last comparison (the drift site).
    [[nodiscard]] std::uint64_t line() const noexcept { return line_; }
    [[nodiscard]] const std::string& detail() const noexcept {
        return detail_;
    }

private:
    template <typename Builder>
    bool expect_line(Builder&& build, std::string* error) {
        std::string actual;
        if (!std::getline(in_, actual)) {
            return fail("golden ended early at line " +
                            std::to_string(line_ + 1),
                        error);
        }
        ++line_;
        f4::json::Writer w;
        build(w);   // the line WITHOUT its newline (getline strips it)
        if (w.str() != actual) {
            return fail("line " + std::to_string(line_) +
                            " diverges\n  want: " + w.str() +
                            "\n  got:  " + actual,
                        error);
        }
        return true;
    }

    bool fail(const std::string& what, std::string* error) {
        detail_ = what;
        if (error != nullptr) *error = what;
        return false;
    }

    std::ifstream in_;
    std::uint64_t line_{0};
    std::string detail_;
};

} // namespace f4::campaign::api
