// f4-ai/include/f4/ai/wingradio.hpp
//
// WingRadio — the closed wing-radio vocabulary (AI_IMPLEMENTATION_PLAN.md
// §16 Step 14; FreeFalcon wingradio.cpp's AiMakeRadioResponse, ~867 lines
// of radio prose — v1 lands the MESSAGE VOCABULARY, not the prose).
//
// THE CONTRACT (the plan's Part-III risk table): "Closed enum + text
// renderer in the host; new calls require an enum row + a test, no
// free-form strings." AI code publishes these ENUM ROWS ONLY — the text
// rendering lives in the host (f4-simulation's CombatTranscript), so no
// string formatting ever crosses the AI boundary and the radio log can
// never grow an unreviewed line.
//
// THE FLOW (one-directional command flow — the plan's loop risk):
//
//   lead brain  --OrderRejoin/OrderEngageMyTarget/OrderRTB-->  bus
//   wing brain  --Ack-------------------------------------->  bus
//   FAC brain   --FacTalkOn-------------------------------->  bus
//
// The lead publishes an order when its FlightLeadModule's edge rules
// fire (the per-wingman status echoes the host pushes); the wingman
// publishes exactly one Ack per inbound order on its next update —
// an acknowledgment, never a new command. The FAC publishes its talk-on
// once per mark (the Step-15 FacTalkOnModule's one-shot: the FAC marks
// ONE target in v1 — the reference's re-mark priority loop is the v2
// data tranche); the strike brain consumes it as a targeting hint via
// the host (the Step-14 hint pipe — an informational row, never a
// command, so no ack rides back). The kill/loss half of the
// wing vocabulary rides the EXISTING M4 narration (EntityKilledMessage
// -> "Splash ..." / RadarTrackDroppedMessage -> "Lost the picture on
// ..."), which already attributes kills to shooters — wingradio v1
// adds only the order/ack cycle the transcript was missing.
//
// Messages are plain structs (the f4-messaging convention): no
// inheritance, no pack pragmas, just data.
//
// Dependencies: standard library only. C++20.

#pragma once

#include <cstdint>

namespace f4::ai {

// ============================================================================
// The closed vocabulary
// ============================================================================
enum class WingRadio : std::uint8_t {
    OrderRejoin = 1,          ///< lead -> wingman: form back up ("Rejoin.")
    OrderEngageMyTarget = 2,  ///< lead -> wingman: take the lead's bandit
    OrderRTB = 3,             ///< lead -> wingman: fight's over, go home
    Ack = 4,                  ///< wingman -> lead: order received ("Copy.")
    FacTalkOn = 5,            ///< FAC -> strike flight: the mark ("Talk-on ...")
};

/// The talk-on's closed DESCRIPTION vocabulary (Step 15). The reference
/// facbrain.cpp's target-description priority loop (vehicle / armor /
/// SAM rows picked off the fused track) is the v2 data tranche — v1
/// marks ONE target and every talk-on reads "ground assets". The enum
/// keeps the renderer free-form-free: the host's text for each row is
/// as reviewed as the radio lines themselves.
enum class TalkOnDesc : std::uint8_t {
    None = 0,             ///< not a talk-on row (the other vocabulary rows)
    GroundAssets = 1,     ///< the marked feature: "ground assets"
};

/// One radio row on the bus. `sender_id` speaks, `peer_id` is the
/// addressed aircraft (the lead names the wingman; the ack names the
/// lead; the talk-on names the strike flight), `target_id` is the
/// subject bandit of an engage order or the marked entity of a talk-on
/// (0 for the other rows), `time_s` is the HOST clock stamp (the brain
/// reads BrainComponent::host_time() — the host owns time, the same
/// rule the sensor/weapon message stamps follow).
///
/// The talk-on payload (Step 15) rides the same struct as plain data
/// rows — the f4-messaging convention (no inheritance, no variant):
/// `target_bra_deg` is the Bearing of the mark FROM the addressed
/// flight (the radio convention: degrees true, 0 = north, 090 = east),
/// `target_range_ft` the range from the same point, and `target_desc`
/// the closed description row. Zero for every other row.
struct WingRadioMessage {
    std::uint64_t sender_id{0};
    std::uint64_t peer_id{0};
    std::uint64_t target_id{0};
    WingRadio event{WingRadio::Ack};
    double time_s{0.0};
    double target_bra_deg{0.0};
    double target_range_ft{0.0};
    TalkOnDesc target_desc{TalkOnDesc::None};
};

/// The delivery envelope for an order the host carries from the lead's
/// FlightLeadModule to a wingman brain (the host applies it after
/// update_all; the wingman acks + applies on its next update). Same
/// fields as the message minus the sender (the lead brain fills its
/// own id when it acks).
struct FlightOrder {
    std::uint64_t lead_id{0};
    std::uint64_t target_id{0};
    WingRadio event{WingRadio::Ack};
};

} // namespace f4::ai
