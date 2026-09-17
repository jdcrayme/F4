// f4-campaign-api/include/f4/campaign/api/session.hpp
//
// Surface 1 — the lifecycle interface (CAMP_HOST_PLAN.md §3.1), plus the
// query plumbing it shares with §3.2 (ICampaignSession::query).
//
// THE ENGINE IS NOT REAL-TIME (plan §2.2): the contract advances whole
// sim ticks via step(); pacing (wall clocks, animation, sleep) is the
// host's business. `set_time_scale` carries the host's pacing PRESENTION
// for the `time` query to echo — it never changes how many sim seconds a
// tick is (the fixed-dt discipline forbids dt scaling; FID certificates
// depend on it).
//
// Implementations: f4-simulation's EngineSessionHost (wraps the engine's
// CampaignSession — the V-CAMP object), and the protocol tests' mock.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <f4/campaign/api/events.hpp>
#include <f4/campaign/api/identity.hpp>

namespace f4::campaign::api {

// The wire's fidelity vocabulary. Mirrors the engine's FidelityPolicy
// {FullFidelity, Tiered} as DATA (the contract carries bytes, not engine
// enums); the adapter maps it. The plan §4: fidelity policy is a UX
// parameter — a map-only host stays Tiered forever, a 3D host deaggs
// around its focus.
enum class FidelityMode : std::uint8_t { Full = 0, Tiered = 1 };

[[nodiscard]] constexpr std::string_view
to_string(FidelityMode m) noexcept {
    return m == FidelityMode::Tiered ? "tiered" : "full";
}

// --- lifecycle results -------------------------------------------------

struct StepResult {
    /// True when the engine's per-advance tick cap hit — time dilated
    /// (the debt is dropped, never queued; the caller may surface it).
    bool dilated{false};
};

struct SaveResult {
    bool ok{false};
    std::uint64_t bytes{0};
    /// The path written (or the failure reason when !ok).
    std::string detail;
};

// --- queries (§3.2) -----------------------------------------------------

/// A named query with its filter parameters. `team` filters flights /
/// objectives / tasking by team slot (-1 = every team); `limit` caps the
/// returned rows (0 = no cap — the whole theater).
struct QuerySpec {
    std::string name;
    int team{-1};
    std::size_t limit{0};
};

/// The query's DATA OBJECT as compact JSON (the DTO encoders' output —
/// byte-stable, fixed key order). The protocol embeds it verbatim under
/// "data"; the session never pretty-prints, never adds keys ad hoc.
struct QueryResult {
    bool ok{false};
    std::string data_json;
    std::string detail; ///< the failure reason when !ok
};

// --- the lifecycle interface --------------------------------------------

class ICampaignSession {
public:
    virtual ~ICampaignSession() = default;

    /// The replay identity (§5): stable for the same (save, seed,
    /// journal). Cheap enough to call per response; goldens pin it.
    [[nodiscard]] virtual IdentityFingerprint identity() const = 0;

    /// Advance exactly `ticks` fixed-dt sim ticks. Commands submitted
    /// since the last step apply at this step's first tick boundary
    /// (§5). Returns the dilation flag (StepResult).
    virtual StepResult step(std::uint32_t ticks) = 0;

    /// The host's pacing presentation (see the class comment).
    virtual void set_time_scale(double scale) = 0;

    /// Pause/resume the drain (a paused session still answers queries
    /// and still deaggregates under focus — the V-3DLIVE rule).
    virtual void set_paused(bool on) = 0;

    /// The runtime-safe save: write-back + WorldState JSON. The .cam
    /// assembly stays the importer's process (json2cam --reencode-all) —
    /// the F4_SIDE boundary keeps it out of the runtime link closure.
    virtual SaveResult save(std::string_view path) = 0;

    /// A v1 query (the protocol validates the name against the version's
    /// whitelist first; unknown names never reach the session).
    [[nodiscard]] virtual QueryResult query(const QuerySpec& spec) = 0;

    /// A typed command (§3.3). Refusal is data — never an exception
    /// across the boundary.
    virtual struct CommandAck submit(const struct CommandIntent& intent) = 0;

    // --- events (§3.4, CAMP-HOST-2) ---------------------------------------

    /// Arm the event stream. The filter gates what the session BUFFERS;
    /// a session that is never armed subscribes to nothing and buffers
    /// nothing — the golden-identity rule (features arm by use, plan
    /// §2.4). Calling again replaces the filter (the bus subscription is
    /// installed once, on the first arm).
    virtual void set_event_filter(const EventFilter& filter) = 0;

    /// The events observed since the last drain (the last step that
    /// returned them, or the arm point), filtered by the CURRENT filter
    /// at the time each event fired. Draining clears. The protocol's
    /// step op calls this after every step and emits each event as its
    /// own line (the "events":N framing).
    [[nodiscard]] virtual std::vector<CampaignEvent> drain_events() = 0;
};

} // namespace f4::campaign::api
