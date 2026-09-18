// f4-campaign-api/include/f4/campaign/api/protocol.hpp
//
// The line protocol (CAMP_HOST_PLAN.md §7): one JSON object per line,
// request in, response out. `campaignd` (the reference host) runs this
// loop over stdio; the tests run it against a mock session with no
// process at all — the dispatch is a library function, not a main().
//
// THE ENVELOPE (byte-stable output; fixed key order):
//   {"v":1,"op":<op>,"status":"ok"|"refused"|"error", ...}
//
// v1 ops:
//   hello    → protocol + identity fingerprint
//   step     → advance N fixed-dt ticks          {"ticks":N}
//   pause    → pause/resume the drain            {"on":0|1}
//   query    → time|stats|flights|tasking|books|objectives
//                                                 {"q":"...",["team":N,["limit":N]]}
//   command  → a typed CommandIntent             {"intent":"...",...}
//   save     → runtime-safe save (world JSON)    {"path":"..."}
//   subscribe → arm the event stream (HOST-2)    {"kinds":[...],["teams":[...]]}
//
// Event framing (HOST-2): a step response carries "events":N — the
// number of event lines that FOLLOW it (each line one pinned event
// encoder's bytes: {"ev":"kill","t":...,...}). Line types are
// discriminable by their first keys: responses carry "op", events
// carry "ev". A client that never subscribes sees "events":0 and NO
// extra lines — the HOST-1 wire is unchanged for it (the golden-
// identity rule, now on the wire too). The journal (journal.hpp) is
// the ENGINE-RATE sink and never filters; the wire is what the host
// subscribed to.
//
// Exit-code mapping (the plan §7 table; campaignd's contract with CI):
//   20 protocol violation — malformed line, bad version, unknown op
//   21 unknown query
//   22 command refused   — data, not fatal mid-stream; the reference host
//                          exits 22 at EOF when the LAST command was
//                          refused (a scripted golden fails loudly)
//   23 identity drift    — campaignd --verify-journal's replay
//                          assertion failed (HOST-2; checked at the
//                          process level, not in this dispatcher)
//   24 engine operation failed — save/query/journal refused by the
//                                 engine side
//
// Strictness: UNKNOWN KEYS in the envelope are a protocol violation —
// the v1 dialect rejects them loudly (the house's loud-failure
// discipline; a silent skip would let a client drift for weeks). The
// version key gates everything: a higher v is "bad_version", never a
// silent best-effort parse.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <f4/campaign/api/commands.hpp>
#include <f4/campaign/api/dto.hpp>
#include <f4/campaign/api/identity.hpp>
#include <f4/campaign/api/session.hpp>
#include <f4/json/reader.hpp>
#include <f4/json/writer.hpp>

namespace f4::campaign::api {

inline constexpr std::uint32_t kProtocolVersion = 1;

struct ProtocolOutcome {
    enum class Kind : std::uint8_t { Ok, Refused, ProtocolError };

    Kind kind{Kind::Ok};
    /// The exit code this outcome maps to in the reference host
    /// (0 for Ok; 22 for a refusal; 20/21/24 per the table above).
    int exit_code{0};
};

// Dispatch one request line; append exactly one response line (with the
// trailing newline) to `out`. Throws nothing.
ProtocolOutcome host_handle(ICampaignSession& session,
                            std::string_view line,
                            std::string& out);

// --- internals -----------------------------------------------------------

namespace detail {

// The identity object — the canonical bytes live in identity.hpp (the
// journal's header/footer lines share them).
inline void encode(f4::json::Writer& w, const IdentityFingerprint& id) {
    encode_identity(w, id);
}

// The uniform error line. `op` is the (possibly empty) op string when the
// envelope was readable enough to name one.
inline void encode_error(f4::json::Writer& w, std::string_view op,
                         std::string_view code, std::string_view detail) {
    w.raw("{\"v\":");
    w.number(static_cast<std::uint64_t>(kProtocolVersion));
    w.raw(",\"op\":\"");
    w.put(f4::json::escape_string(op));
    w.raw("\",\"status\":\"error\",\"error\":{\"code\":\"");
    w.put(f4::json::escape_string(code));
    w.raw("\",\"detail\":\"");
    w.put(f4::json::escape_string(detail));
    w.raw("\"}}");
}

[[nodiscard]] inline ProtocolOutcome
error_line(std::string& out, std::string_view op, std::string_view code,
           std::string_view detail, int exit_code) {
    f4::json::Writer w;
    encode_error(w, op, code, detail);
    out.append(w.str());
    out.push_back('\n');
    ProtocolOutcome o;
    o.kind = ProtocolOutcome::Kind::ProtocolError;
    o.exit_code = exit_code;
    return o;
}

} // namespace detail

inline ProtocolOutcome host_handle(ICampaignSession& session,
                                   const std::string_view line,
                                   std::string& out) {
    using PK = ProtocolOutcome::Kind;

    // --- parse the envelope (strict) -------------------------------------
    std::string op;
    bool have_v = false;
    long long v = 0;
    // op-specific slots
    long long ticks = 0;
    bool pause_on = false;
    std::string query_name;
    long long team = -1;
    long long limit = 0;
    bool have_team = false;
    bool have_limit = false;
    std::string intent_name;
    CommandIntent intent;
    bool have_intent = false;
    std::string save_path;
    std::vector<std::string> kind_names;
    bool have_kinds = false;
    std::vector<long long> teams;

    try {
        f4::json::Reader r(line);
        r.expect('{');
        bool closed = false;
        while (!closed) {
            const auto key = r.read_string();
            r.expect(':');
            if (key == "v") {
                v = r.read_int();
                have_v = true;
            } else if (key == "op") {
                op = r.read_string();
            } else if (key == "ticks") {
                ticks = r.read_int();
            } else if (key == "on") {
                pause_on = r.read_bool();
            } else if (key == "q") {
                query_name = r.read_string();
            } else if (key == "team") {
                team = r.read_int();
                have_team = true;
            } else if (key == "limit") {
                limit = r.read_int();
                have_limit = true;
            } else if (key == "kinds") {
                // ["kill",...] | [] — the v1 event-vocabulary names
                // (plus "all"); validated at dispatch, not here.
                r.expect('[');
                if (!r.consume(']')) {
                    while (true) {
                        kind_names.push_back(r.read_string());
                        if (r.consume(']')) break;
                        r.expect(',');
                    }
                }
                have_kinds = true;
            } else if (key == "teams") {
                // [] = every team (the filter's own default)
                r.expect('[');
                if (!r.consume(']')) {
                    while (true) {
                        teams.push_back(r.read_int());
                        if (r.consume(']')) break;
                        r.expect(',');
                    }
                }
            } else if (key == "intent") {
                intent_name = r.read_string();
                // the body parse consumes ':' + {...} — including the
                // envelope's closing brace
                intent = parse_command_body(intent_name, r);
                have_intent = true;
                closed = true; // parse_command_body consumed the '}'
                break;
            } else if (key == "path") {
                save_path = r.read_string();
            } else {
                throw std::runtime_error("unknown envelope key " + key);
            }
            if (r.consume('}')) closed = true;
            else r.expect(',');
        }
    } catch (const std::exception& e) {
        return detail::error_line(out, op, "malformed", e.what(), 20);
    }

    if (!have_v || v != static_cast<long long>(kProtocolVersion)) {
        return detail::error_line(out, op, "bad_version",
                                  "protocol version mismatch (want v=1)",
                                  20);
    }
    if (op.empty()) {
        return detail::error_line(out, op, "malformed", "missing op", 20);
    }

    // --- dispatch ---------------------------------------------------------

    if (op == "hello") {
        const auto id = session.identity();
        f4::json::Writer w;
        w.raw("{\"v\":1,\"op\":\"hello\",\"status\":\"ok\",\"identity\":");
        detail::encode(w, id);
        w.put('}');
        out.append(w.str());
        out.push_back('\n');
        return {};
    }

    if (op == "step") {
        if (!have_v || ticks < 0) {
            return detail::error_line(out, op, "malformed",
                                      "step needs ticks >= 0", 20);
        }
        const auto res = session.step(static_cast<std::uint32_t>(ticks));
        // HOST-2: the events the step produced, delivered per the step
        // return (the plan §3.4 rule) — N announced in the response, N
        // lines following. An un-armed session drains to zero lines.
        auto events = session.drain_events();
        f4::json::Writer w;
        w.raw("{\"v\":1,\"op\":\"step\",\"status\":\"ok\",\"ticks\":");
        w.number(static_cast<std::uint64_t>(ticks));
        w.raw(",\"dilated\":");
        w.raw(res.dilated ? "1" : "0");
        w.raw(",\"events\":");
        w.number(static_cast<std::uint64_t>(events.size()));
        w.put('}');
        out.append(w.str());
        out.push_back('\n');
        for (const auto& e : events) {
            f4::json::Writer ew;
            encode(ew, e);
            out.append(ew.str());
            out.push_back('\n');
        }
        return {};
    }

    if (op == "pause") {
        session.set_paused(pause_on);
        f4::json::Writer w;
        w.raw("{\"v\":1,\"op\":\"pause\",\"status\":\"ok\",\"on\":");
        w.raw(pause_on ? "1" : "0");
        w.put('}');
        out.append(w.str());
        out.push_back('\n');
        return {};
    }

    if (op == "query") {
        // the v1 whitelist (additive evolution — plan §11). CAMP-HOST-3
        // adds `threat` (the DTO landed additively; see dto.hpp);
        // CAMP-DOM-1 adds `verdict` (the books' projection — dto.hpp);
        // CAMP-DOM-3 adds `squadrons` (the personnel face — dto.hpp).
        const bool known = query_name == "time" || query_name == "stats" ||
                           query_name == "flights" ||
                           query_name == "tasking" ||
                           query_name == "books" ||
                           query_name == "objectives" ||
                           query_name == "threat" ||
                           query_name == "verdict" ||
                           query_name == "squadrons";
        if (!known) {
            return detail::error_line(out, op, "unknown_query",
                                      "no such query: " + query_name, 21);
        }
        QuerySpec spec;
        spec.name = query_name;
        if (have_team) spec.team = static_cast<int>(team);
        if (have_limit && limit > 0) spec.limit =
            static_cast<std::size_t>(limit);
        const auto res = session.query(spec);
        if (!res.ok) {
            return detail::error_line(out, op, "query_failed",
                                      res.detail, 24);
        }
        f4::json::Writer w;
        w.raw("{\"v\":1,\"op\":\"query\",\"q\":\"");
        w.put(f4::json::escape_string(query_name));
        w.raw("\",\"status\":\"ok\",\"data\":");
        w.raw(res.data_json);
        w.put('}');
        out.append(w.str());
        out.push_back('\n');
        return {};
    }

    if (op == "command") {
        if (!have_intent) {
            return detail::error_line(out, op, "malformed",
                                      "command needs an intent", 20);
        }
        const auto ack = session.submit(intent);
        f4::json::Writer w;
        w.raw("{\"v\":1,\"op\":\"command\",\"status\":\"");
        w.raw(ack.status == CommandAck::Status::Applied ? "ok" : "refused");
        w.raw("\",\"ack\":");
        encode(w, ack);
        w.put('}');
        out.append(w.str());
        out.push_back('\n');
        if (ack.status == CommandAck::Status::Refused) {
            ProtocolOutcome o;
            o.kind = PK::Refused;
            o.exit_code = 22;
            return o;
        }
        return {};
    }

    if (op == "subscribe") {
        // HOST-2: arm the event stream. kinds REQUIRED (a subscription
        // that names nothing is a client bug — loud, the strictness
        // rule); "all" short-circuits the kind gate; teams OPTIONAL
        // (default: every team). The response echoes the EFFECTIVE
        // filter in canonical order.
        if (!have_kinds) {
            return detail::error_line(out, op, "malformed",
                                      "subscribe needs kinds", 20);
        }
        EventFilter filter;
        std::vector<std::string> echo_kinds;
        for (const auto& name : kind_names) {
            if (name == "all") {
                filter.all = true;
                echo_kinds.push_back(name);
                continue;
            }
            CampaignEvent::Kind kind;
            if (!parse_event_kind(name, kind)) {
                return detail::error_line(out, op, "malformed",
                                          "unknown event kind: " + name,
                                          20);
            }
            filter.kinds.push_back(kind);
            echo_kinds.push_back(name);
        }
        for (const auto t : teams) {
            if (t < 0 || t > 255) {
                return detail::error_line(out, op, "malformed",
                                          "team slots are 0..255", 20);
            }
            filter.teams.push_back(static_cast<int>(t));
        }
        session.set_event_filter(filter);
        f4::json::Writer w;
        w.raw("{\"v\":1,\"op\":\"subscribe\",\"status\":\"ok\",\"kinds\":[");
        for (std::size_t i = 0; i < echo_kinds.size(); ++i) {
            if (i > 0) w.put(',');
            w.string(echo_kinds[i]);
        }
        w.raw("],\"teams\":[");
        for (std::size_t i = 0; i < filter.teams.size(); ++i) {
            if (i > 0) w.put(',');
            w.number(static_cast<std::uint64_t>(filter.teams[i]));
        }
        w.raw("]}");
        out.append(w.str());
        out.push_back('\n');
        return {};
    }

    if (op == "save") {
        const auto res = session.save(save_path);
        if (!res.ok) {
            return detail::error_line(out, op, "save_failed", res.detail,
                                      24);
        }
        f4::json::Writer w;
        w.raw("{\"v\":1,\"op\":\"save\",\"status\":\"ok\",\"bytes\":");
        w.number(static_cast<std::uint64_t>(res.bytes));
        w.raw(",\"path\":\"");
        w.put(f4::json::escape_string(res.detail));
        w.raw("\"}");
        out.append(w.str());
        out.push_back('\n');
        return {};
    }

    return detail::error_line(out, op, "unknown_op", "no such op: " + op,
                              20);
}

} // namespace f4::campaign::api
