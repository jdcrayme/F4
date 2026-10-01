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
//
// The lead publishes an order when its FlightLeadModule's edge rules
// fire (the per-wingman status echoes the host pushes); the wingman
// publishes exactly one Ack per inbound order on its next update —
// an acknowledgment, never a new command. The kill/loss half of the
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
};

/// One radio row on the bus. `sender_id` speaks, `peer_id` is the
/// addressed aircraft (the lead names the wingman; the ack names the
/// lead), `target_id` is the subject bandit of an engage order (0 for
/// the other rows), `time_s` is the HOST clock stamp (the brain reads
/// BrainComponent::host_time() — the host owns time, the same rule the
/// sensor/weapon message stamps follow).
struct WingRadioMessage {
    std::uint64_t sender_id{0};
    std::uint64_t peer_id{0};
    std::uint64_t target_id{0};
    WingRadio event{WingRadio::Ack};
    double time_s{0.0};
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
