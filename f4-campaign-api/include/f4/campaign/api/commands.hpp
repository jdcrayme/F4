// f4-campaign-api/include/f4/campaign/api/commands.hpp
//
// Surface 3 — typed commands (CAMP_HOST_PLAN.md §3.3). A closed
// CommandIntent variant; refusals are typed data (CommandAck), never
// exceptions across the boundary — a UX surfaces "can't retask: flight is
// in merge" as UI, and a scripted golden pins the refusal byte-for-byte.
//
// V1's command set is deliberately minimal and HONEST about the engine:
//   focus / clear_focus / select_deagg / select_reagg — the FID machinery
//     wearing its contract hat (plan §4; the engine implements these
//     TODAY: set_view_bubble / force_(de|re)aggregate_flight).
//   roe_set / flight_retask / flight_abort / objective_priority — the
//     CAMP-CMD queue. The v1 adapter REFUSES these with
//     Refusal::NotImplemented and the task ID in the detail: the wire is
//     stable now so CAMP-CMD-1/2 land without a protocol bump.
//
// Command timing (plan §5): a submitted command applies at the NEXT tick
// boundary (the next step()), in submission order; the ack carries the
// campaign time it will apply at. V1's engine-side commands (the FID
// family) apply immediately — they are presentation-adjacent doctrine,
// not war state — and say so in the ack's detail.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <f4/json/reader.hpp>
#include <f4/json/writer.hpp>

namespace f4::campaign::api {

// --- RoE vocabulary (the P7 wire bytes) ---------------------------------
//
// 0 = free (the pre-P7 default), 1 = weapons TIGHT (BVR suppressed),
// 2 = weapons HOLD (everything held). These are the engine's own
// fire-control gate values — the contract passes them through verbatim.
enum class RoeLevel : std::uint8_t { Free = 0, Tight = 1, Hold = 2 };

enum class RoEScopeKind : std::uint8_t { Team = 0, Mission = 1, Flight = 2 };

struct RoEScope {
    RoEScopeKind kind{RoEScopeKind::Team};
    std::uint8_t team{0};          ///< Team kind: the team slot
    std::uint8_t mission{0};       ///< Mission kind: the mission wire byte
    std::uint32_t flight{0};       ///< Flight kind: the flight VU id
};

// --- the closed intent variant ------------------------------------------

struct CommandIntent {
    enum class Kind : std::uint8_t {
        RoeSet,
        FlightRetask,
        FlightAbort,
        ObjectivePriority,
        Focus,
        ClearFocus,
        SelectDeagg,
        SelectReagg,
    };

    Kind kind{};

    // RoeSet
    RoEScope scope{};
    RoeLevel roe{RoeLevel::Free};

    // FlightRetask
    std::uint32_t flight{0};             ///< the flight VU id
    std::uint8_t mission_byte{0};        ///< the new mission
    std::uint32_t target_objective_id{0};///< the new target (0 = none)

    // FlightAbort
    // (uses `flight`)

    // ObjectivePriority
    std::uint32_t objective_id{0};
    std::int32_t weight{0};

    // Focus (sim-frame position, ENU feet)
    double x{0.0};
    double y{0.0};
    double z{0.0};
    double radius_ft{0.0};
};

// --- the typed acknowledgement ------------------------------------------

struct CommandAck {
    enum class Status : std::uint8_t { Applied, Refused };
    enum class Refusal : std::uint8_t {
        None = 0,
        NotImplemented,   ///< queued behind a named tranche (the detail says which)
        UnknownFlight,    ///< the flight VU id is not in the session's roster
        InvalidArgument,  ///< well-formed wire, nonsensical value
    };

    Status status{Status::Applied};
    Refusal refusal{Refusal::None};
    /// Human-readable context: the tranche for NotImplemented, the
    /// engine's semantics for Applied ("view bubble set"), the reason for
    /// InvalidArgument.
    std::string detail;
    /// The campaign time the command applies at (the next tick boundary;
    /// immediately-applied commands report the current time).
    std::int64_t apply_tick{0};
};

// --- wire encoding (byte-stable; fixed key order) ------------------------

[[nodiscard]] inline std::string_view to_string(CommandAck::Refusal r) noexcept {
    switch (r) {
        case CommandAck::Refusal::NotImplemented:  return "not_implemented";
        case CommandAck::Refusal::UnknownFlight:   return "unknown_flight";
        case CommandAck::Refusal::InvalidArgument: return "invalid_argument";
        case CommandAck::Refusal::None:            break;
    }
    return "none";
}

inline void encode(f4::json::Writer& w, const CommandAck& ack) {
    w.raw("{\"status\":\"");
    w.raw(ack.status == CommandAck::Status::Applied ? "applied" : "refused");
    w.raw("\",\"refusal\":\"");
    w.raw(to_string(ack.refusal));
    w.raw("\",\"detail\":");
    // string() adds the quotes — don't pre-open them
    w.string(ack.detail);
    w.raw(",\"apply_tick\":");
    w.number(static_cast<long long>(ack.apply_tick));
    w.put('}');
}

// --- wire parsing --------------------------------------------------------
//
// Parses a command's fields off the protocol line's Reader. The caller
// has already consumed {"v":1,"op":"command" and the intent key's VALUE
// (the intent NAME is a plain string on the wire:
//
//   {"v":1,"op":"command","intent":"roe_set","scope":{...},"roe":2}
//
// so this function takes the intent NAME and a reader positioned just
// AFTER the intent's string value — i.e. facing the rest of the
// envelope: the intent's sibling keys, then the closing '}'). Unknown
// keys and unknown intents throw (the protocol maps that to the
// malformed error); well-formed-but-wrong values (a negative radius)
// parse and let the session's ack refuse them — the wire and the
// semantics stay separate.

[[nodiscard]] RoeLevel parse_roe_level(long long v);
[[nodiscard]] RoEScope parse_roe_scope(f4::json::Reader& r);
[[nodiscard]] CommandIntent parse_command_body(const std::string& intent,
                                               f4::json::Reader& r);

// --- inline implementations ---------------------------------------------

inline RoeLevel parse_roe_level(const long long v) {
    if (v < 0 || v > 2) {
        throw std::runtime_error("f4-campaign-api: roe must be 0, 1, or 2");
    }
    return static_cast<RoeLevel>(v);
}

inline RoEScope parse_roe_scope(f4::json::Reader& r) {
    RoEScope scope;
    r.expect('{');
    // {"kind":"team"|"mission"|"flight","team":N,"mission":N,"flight":N}
    // — kind first (the canonical order the encoder uses); the remaining
    // keys are optional per kind. read_string() consumes the key itself
    // (skip_ws + expect('"') inside).
    const auto key = r.read_string();
    if (key != "kind") {
        throw std::runtime_error(
            "f4-campaign-api: scope must start with \"kind\"");
    }
    r.expect(':');
    const auto kind = r.read_string();
    if (kind == "team") {
        scope.kind = RoEScopeKind::Team;
    } else if (kind == "mission") {
        scope.kind = RoEScopeKind::Mission;
    } else if (kind == "flight") {
        scope.kind = RoEScopeKind::Flight;
    } else {
        throw std::runtime_error("f4-campaign-api: unknown scope kind " +
                                 kind);
    }
    while (true) {
        if (r.consume('}')) break;
        r.expect(',');
        const auto k2 = r.read_string();
        r.expect(':');
        if (k2 == "team") {
            scope.team = static_cast<std::uint8_t>(r.read_int());
        } else if (k2 == "mission") {
            scope.mission = static_cast<std::uint8_t>(r.read_int());
        } else if (k2 == "flight") {
            scope.flight = static_cast<std::uint32_t>(r.read_int());
        } else {
            throw std::runtime_error("f4-campaign-api: unknown scope key " +
                                     k2);
        }
    }
    return scope;
}

// The per-intent field walkers. Each consumes exactly one "key":value of
// the intent's vocabulary; the shared loop below handles the commas and
// the closing brace.
inline void parse_intent_field(CommandIntent& cmd,
                               const std::string& intent,
                               const std::string& key,
                               f4::json::Reader& r) {
    if (intent == "roe_set") {
        if (key == "scope") {
            cmd.scope = parse_roe_scope(r);
        } else if (key == "roe") {
            cmd.roe = parse_roe_level(r.read_int());
        } else {
            throw std::runtime_error("f4-campaign-api: unknown roe_set key " +
                                     key);
        }
        return;
    }
    if (intent == "flight_retask" || intent == "flight_abort" ||
        intent == "select_deagg" || intent == "select_reagg") {
        if (key == "flight") {
            cmd.flight = static_cast<std::uint32_t>(r.read_int());
        } else if (key == "mission") {
            cmd.mission_byte = static_cast<std::uint8_t>(r.read_int());
        } else if (key == "target") {
            cmd.target_objective_id = static_cast<std::uint32_t>(r.read_int());
        } else {
            throw std::runtime_error("f4-campaign-api: unknown " + intent +
                                     " key " + key);
        }
        return;
    }
    if (intent == "objective_priority") {
        if (key == "objective") {
            cmd.objective_id = static_cast<std::uint32_t>(r.read_int());
        } else if (key == "weight") {
            cmd.weight = static_cast<std::int32_t>(r.read_int());
        } else {
            throw std::runtime_error(
                "f4-campaign-api: unknown objective_priority key " + key);
        }
        return;
    }
    if (intent == "focus") {
        // {"x":X,"y":Y,"z":Z,"radius_ft":R} — sim frame, ENU feet
        if (key == "x") {
            cmd.x = r.read_number();
        } else if (key == "y") {
            cmd.y = r.read_number();
        } else if (key == "z") {
            cmd.z = r.read_number();
        } else if (key == "radius_ft") {
            cmd.radius_ft = r.read_number();
        } else {
            throw std::runtime_error("f4-campaign-api: unknown focus key " +
                                     key);
        }
        return;
    }
    throw std::runtime_error("f4-campaign-api: unknown intent " + intent);
}

inline CommandIntent parse_command_body(const std::string& intent,
                                        f4::json::Reader& r) {
    CommandIntent cmd;

    if (intent == "roe_set") {
        cmd.kind = CommandIntent::Kind::RoeSet;
    } else if (intent == "flight_retask") {
        cmd.kind = CommandIntent::Kind::FlightRetask;
    } else if (intent == "flight_abort") {
        cmd.kind = CommandIntent::Kind::FlightAbort;
    } else if (intent == "objective_priority") {
        cmd.kind = CommandIntent::Kind::ObjectivePriority;
    } else if (intent == "focus") {
        cmd.kind = CommandIntent::Kind::Focus;
    } else if (intent == "clear_focus") {
        cmd.kind = CommandIntent::Kind::ClearFocus;
    } else if (intent == "select_deagg") {
        cmd.kind = CommandIntent::Kind::SelectDeagg;
    } else if (intent == "select_reagg") {
        cmd.kind = CommandIntent::Kind::SelectReagg;
    } else {
        throw std::runtime_error("f4-campaign-api: unknown intent " + intent);
    }

    // The rest of the envelope: ","key":"value" pairs, then '}'. The
    // intent's fields are the envelope's OWN sibling keys (the v1 wire —
    // see the function comment). clear_focus carries no fields.
    bool closed = r.consume('}');
    while (!closed) {
        r.expect(',');
        const auto key = r.read_string();
        r.expect(':');
        parse_intent_field(cmd, intent, key, r);
        closed = r.consume('}');
    }
    return cmd;
}

} // namespace f4::campaign::api
