// f4-ai/src/fac_talk_on_module.cpp
//
// FacTalkOnModule — the talk-on decision (see the header for the
// contract). The rule is deliberately small: one edge, one latch, one
// message. All the interesting context (orbit, mark liveness, the
// addressee's position) is resolved by the brain/host and passed in —
// the module stays pure and the unit tests need no world.

#include "f4/ai/modules/fac_talk_on_module.hpp"

#include <cmath>

namespace f4::ai::modules {

void FacTalkOnModule::set_mark(std::uint64_t target_id) {
    if (latched_) return;  // v1 marks ONE target — reset() is the way back
    mark_id_ = target_id;
}

void FacTalkOnModule::report_addressee(const AddresseeEcho& echo) {
    addressee_ = echo;
    addressee_seen_ = true;
}

std::optional<WingRadioMessage> FacTalkOnModule::update(
        std::uint64_t fac_id, const MarkPicture& mark,
        const TalkOnInput& in, double host_time_s) {
    // The addressee picture is THIS tick's by construction: every
    // update consumes whatever the host pushed since the last update,
    // gate alignment or not — a push that goes unused (short of the
    // station, dead mark) can never put a stale position on a later
    // radio line. The latch is checked first: a published mark is done
    // (v1's one talk-on), and latching also consumes the picture.
    const bool latched_before = latched_;
    const bool have_addressee =
        addressee_seen_ && addressee_.alive && addressee_.entity_id != 0;
    addressee_seen_ = false;
    if (latched_before || mark_id_ == 0) return std::nullopt;

    // The mark must be alive — a dead mark is never talked on (the
    // corpse rule, mark side).
    if (!mark.alive) return std::nullopt;

    // The orbit must be established — a talk-on from short of the
    // station is a lie the reference never tells.
    if (!in.on_station) return std::nullopt;

    // Somebody to talk on: the host's addressee picture for THIS tick
    // (a live assigned strike flight).
    if (!have_addressee) return std::nullopt;

    // --- The BRA: from the ADDRESSEE to the mark (the radio convention:
    // bearing degrees true, 0 = north, 090 = east; range the horizontal
    // ground distance). ENU: x = east, y = north.
    const double dx = mark.position.x - addressee_.position.x;
    const double dy = mark.position.y - addressee_.position.y;
    const double range_ft = std::sqrt(dx * dx + dy * dy);
    double bra_deg = std::atan2(dx, dy) * (180.0 / 3.1415926535897932346);
    if (bra_deg < 0.0) bra_deg += 360.0;

    WingRadioMessage m{};
    m.sender_id = fac_id;
    m.peer_id = addressee_.entity_id;
    m.target_id = mark_id_;
    m.event = WingRadio::FacTalkOn;
    m.time_s = host_time_s;
    m.target_bra_deg = bra_deg;
    m.target_range_ft = range_ft;
    m.target_desc = TalkOnDesc::GroundAssets;

    latched_ = true;
    return m;
}

void FacTalkOnModule::reset() {
    mark_id_ = 0;
    addressee_ = AddresseeEcho{};
    addressee_seen_ = false;
    latched_ = false;
}

} // namespace f4::ai::modules
