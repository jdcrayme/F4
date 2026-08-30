// f4-weapons/include/f4/weapons/f4_weapons.hpp
//
// Umbrella header for the f4-weapons library.
//
// f4-weapons is the weapons & effects core of the F4 combat chain
// (Docs/COMBAT_CHAIN_PLAN.md, Milestone M1):
//
//   weapon_types / weapon_class_table — what the weapons ARE
//   weapon_store                      — what a loaded entity CARRIES
//   missile / missile_battery         — what happens when a missile FIRES
//   gun                               — what happens when a gun FIRES
//   damage                            — what happens when something is HIT
//   messages                          — the bus events all of it publishes
//
// Dependencies: f4-geo, f4-math, f4-entities, f4-messaging. Engine-agnostic
// (no rendering, no flight-model coupling). C++20.

#pragma once

#include <f4/weapons/weapon_types.hpp>
#include <f4/weapons/weapon_class_table.hpp>
#include <f4/weapons/weapon_store.hpp>
#include <f4/weapons/damage.hpp>
#include <f4/weapons/missile.hpp>
#include <f4/weapons/missile_battery.hpp>
#include <f4/weapons/gun.hpp>
#include <f4/weapons/messages.hpp>
