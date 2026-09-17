// f4-campaign-api/include/f4/campaign/api/f4_campaign_api.hpp
//
// Umbrella header — f4-campaign-api, the campaign engine's host contract
// (Docs/CAMP_HOST_PLAN.md §3). The four surfaces:
//
//   lifecycle  — session.hpp   (ICampaignSession: identity/step/pause/save)
//   queries    — session.hpp + dto.hpp (versioned DTOs, byte-stable JSON)
//   commands   — commands.hpp  (typed CommandIntent + typed CommandAck)
//   events     — events.hpp    (the v1 event vocabulary; HOST-2 wires it)
//
// plus protocol.hpp, the line dispatch both `campaignd` (the reference
// host) and the tests drive.
//
// DEPENDENCY RULE (the library's whole point): f4-campaign-api depends on
// f4-json and the standard library ONLY. No engine types, no entity
// handles, no f4-geo/f4-units — a host in any language that speaks the
// JSON wire can implement a client, and the C++ host does not drag the
// simulation into its link closure just to name the contract. Positions on
// the wire are plain numbers in the sim's own frame (ENU feet); the
// adapters own the conversion to the engine's strong types.

#pragma once

#include <f4/campaign/api/commands.hpp>
#include <f4/campaign/api/dto.hpp>
#include <f4/campaign/api/events.hpp>
#include <f4/campaign/api/identity.hpp>
#include <f4/campaign/api/protocol.hpp>
#include <f4/campaign/api/session.hpp>
