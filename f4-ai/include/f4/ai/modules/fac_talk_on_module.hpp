// f4-ai/include/f4/ai/modules/fac_talk_on_module.hpp
//
// FacTalkOnModule — the talk-on half of the FAC brain (AI_IMPLEMENTATION_PLAN.md
// §16 Step 15; FreeFalcon facbrain.cpp's target marking + talk-on).
//
// THE ROLE. The FAC (Forward Air Controller) orbits its station over the
// marked area and talks the assigned strike flight onto the mark. The
// orbit itself is the NavigationModule's station hold (the P7 racetrack
// contract — the same mechanics the tankers fly); the sensor fusion and
// defensive ladder are the brain's own; THIS module is only the talk-on
// decision: it turns "on station, mark live, addressee known" into ONE
// FacTalkOn radio row on the closed WingRadio vocabulary, plus the
// message state the host reads back and delivers to the strike brain
// (the Step-14 hint pipe — the same host-mediated push discipline the
// lead's orders ride).
//
// THE PICTURE (engine-agnostic — the same contract as every module):
// the module never touches the world, the bus, or another entity. The
// host:
//   * at spawn     — the brain's mark is set (the scenario's
//                    "mark_feature" resolves to the marked entity's id);
//   * each tick    — report_addressee(echo) with the assigned strike
//                    flight's position (v1: the nearest same-team
//                    non-support aircraft — the host is the FAC's eyes);
//   * each update  — the brain resolves the mark's liveness + position
//                    and passes them in with the orbit state; the module
//                    returns the one message to publish, or nothing.
// The brain (which owns the bus) publishes the message — the transcript
// renders it; the AI never formats text.
//
// THE RULE (v1 marks ONE target):
//
//   TALK-ON — publish exactly once, when ALL of these hold:
//     * the mark is set and alive (a dead mark is never talked on —
//       the corpse rule, seen from the mark side);
//     * the FAC is ON STATION (the nav hold is running — a talk-on
//       from short of the orbit is a lie; the reference talks its
//       marks from the station);
//     * the host reports a live addressee this tick (no addressee,
//       nobody to talk on);
//   and then the latch closes: v1 has no re-mark loop (the reference's
//   target-description priority loop + the re-talk-on on a lost mark
//   are the v2 data tranche). "The strike flight does not yet see" is
//   structurally true in this engine — the fusion ladder is air-only,
//   a ground mark can never arrive through the strike's own sensors
//   first — so v1 needs no seen-guard; the one-shot latch is what
//   keeps the net from re-broadcasting.
//
// THE BRA (Bearing / Range / the closed description row): computed HERE,
// from the addressee's pushed position to the mark's resolved position —
// bearing degrees true (0 = north, 090 = east, the radio convention),
// range the horizontal ground distance in feet. The strike brain does
// NOT need these to prosecute (the hint carries the marked entity's id;
// the aim resolution reads the world) — they are what makes the radio
// line an honest talk-on.
//
// THE GATE lives in the host (the scenario ai block's "flight_lead"
// arms the hint pipe; the per-aircraft "fac" field selects the role):
// an unarmed brain never sets a mark, never receives an addressee,
// never publishes a row — byte-identically the pre-Step-15 world.
//
// Dependencies: f4-ai (WingRadio), f4-geo (WorldPosition). C++20.

#pragma once

#include <cstdint>
#include <optional>

#include <f4/geo/position.hpp>

#include "f4/ai/wingradio.hpp"

namespace f4::ai::modules {

// ============================================================================
// FacTalkOnModule
// ============================================================================
class FacTalkOnModule {
public:
    /// The assigned strike flight's picture — the host pushes one per
    /// tick (v1: the nearest same-team non-support aircraft with a
    /// brain; nobody pushed = nobody to talk on this tick).
    struct AddresseeEcho {
        std::uint64_t entity_id{0};
        geo::WorldPosition position{};  ///< ENU feet (the host's read of
                                        ///< the addressee's transform)
        bool alive{false};  ///< not killed (a corpse is not talked on)
    };

    /// The mark-side picture the brain resolves from the world each
    /// update (the brain is the module's eyes — the module stays pure).
    struct MarkPicture {
        geo::WorldPosition position{};  ///< ENU feet
        bool alive{false};
    };

    /// The orbit/state side the brain reads off itself.
    struct TalkOnInput {
        bool on_station{false};  ///< the NavigationModule's hold is running
    };

    // --- Mark in (host sets at spawn; v1 exactly one) ---------------------
    /// The marked entity (0 = no mark — the module is inert). A re-set
    /// BEFORE publication re-targets the talk-on; after the latch it is
    /// ignored (v1 marks ONE target — reset() is the only way back).
    void set_mark(std::uint64_t target_id);

    // --- Addressee in (host pushes each tick, before the brains run) ------
    /// Upsert this tick's addressee picture. Not pushed = not seen (the
    /// module talks on nobody that tick; the picture does NOT persist —
    /// a stale addressee position would put a stale BRA on the radio).
    void report_addressee(const AddresseeEcho& echo);

    // --- Per-tick decision --------------------------------------------------
    /// The talk-on step: return the ONE FacTalkOn row to publish when
    /// the rule fires (the brain puts it on the bus and delivers it
    /// through the host), nothing otherwise. Consumed — a returned
    /// message latches the module (v1's one mark, one talk-on).
    [[nodiscard]] std::optional<WingRadioMessage>
    update(std::uint64_t fac_id, const MarkPicture& mark,
           const TalkOnInput& in, double host_time_s);

    // --- State reporting (tests + host diagnostics) -------------------------
    /// True once the talk-on has gone out (the one-shot latch).
    [[nodiscard]] bool published() const noexcept { return latched_; }
    /// The marked entity (0 = none).
    [[nodiscard]] std::uint64_t mark_id() const noexcept { return mark_id_; }

    /// Clear everything (host re-task — the only path to a second mark).
    void reset();

private:
    std::uint64_t mark_id_{0};
    AddresseeEcho addressee_{};
    bool addressee_seen_{false};
    bool latched_{false};
};

} // namespace f4::ai::modules
