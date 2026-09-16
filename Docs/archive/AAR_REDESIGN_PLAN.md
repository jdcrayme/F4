# AAR Redesign — Real-Aircraft Tanker + Full USAF Procedure

> **Status**: Active redesign. Supersedes the Tranche D ScriptedTanker +
> 5-state RefuelModule from `LANDING_PRECISION_FORMATION_AAR_PLAN.md`.
>
> **Goal**: The tanker is a real aircraft (own flight model + own brain
> flying its own route), and the receiver flies the full USAF boom AAR
> procedure end-to-end: rendezvous → pre-contact → (tanker clears
> contact) → contact → hold → (receiver requests disconnect) → back
> out to pre-contact → (tanker reports fuel + clears departure) →
> descend 1000 ft below tanker → resume flight-planned route.

---

## 1. The USAF boom AAR procedure (the spec)

From the USAF references (AFI 11-2KC-135, the boom operator AMA, the
KC-135 procedure page) + the user's description:

| # | Phase | Who acts | Radio call / geometry |
|---|---|---|---|
| 1 | **Rendezvous** | Receiver | Joins the tanker on its refuel track. The tanker flies straight-and-level (or a racetrack anchor pattern). |
| 2 | **Pre-contact** | Receiver | Stabilizes **behind and below** the tanker — the "observation position". Calls **"Precontact"** when stabilized. |
| 3 | **Clear to contact** | Tanker | The boom operator/pilot clears: **"Cleared to contact"**. The receiver is cleared to move forward into the contact position. |
| 4 | **Closure to contact** | Receiver | Closes **slowly** (~1 ft/s) into the contact position — directly behind the boom receptacle, level with the tanker, wings level. |
| 5 | **Contact / Refueling** | Both | The boom latches. The receiver holds **formation** — keeps the boom in the envelope. Fuel transfers. |
| 6 | **Disconnect request** | Receiver | The receiver calls **"Disconnect"** when refueling is complete. |
| 7 | **Back to pre-contact** | Receiver | Falls back to the pre-contact position (slightly behind/below) — a stabilization step. |
| 8 | **Fuel report + departure clearance** | Tanker | The tanker reports the fuel transferred + clears departure: **"Cleared to depart"**. |
| 9 | **Departure** | Receiver | Descends to **1000 ft below** the tanker's altitude for vertical separation, then resumes own navigation (flight-planned route). |

---

## 2. The architecture

### 2.1 The tanker is a real aircraft

The tanker is spawned as a second `ScenarioAircraft` entry with
`"tanker": true`. The Simulation spawns it exactly like any other
aircraft (`TransformComponent` + `FlightModelComponent` +
`VisualModelComponent` + `BrainComponent`), with its own route (straight-
and-level for the refuel window; a racetrack pattern for realism — start
with straight-and-level).

The `ScriptedTanker` class + the `ScenarioTanker` struct + the
`std::optional<ScriptedTanker> tanker_` member are **deleted**. The
tanker's picture is pushed to the receiver each tick by reading the
tanker's real `TransformComponent` + `FlightModelComponent` — the
exact same pattern `push_wingman_lead_pictures` uses for the lead.

### 2.2 The receiver's RefuelModule — 8-state SM

The current 5-state SM (NoTanker → VectorTo → Waiting → Refueling →
Done) is replaced with an 8-state SM that models the full procedure:

```
NoTanker
  --TankerAssigned-->       Rendezvous       (closing on the tanker's track)

Rendezvous
  --AtPrecontactPos-->      PreContact       (reached the pre-contact observation point)

PreContact
  --ClearToContact-->       ClearedContact   (tanker cleared the receiver to close)
  (entry publishes PrecontactReport)

ClearedContact
  --InContactEnvelope-->    Contact          (receiver moved into the contact envelope)
  (entry publishes ContactRequest — wait for ContactMade)

Contact
  --ContactMade-->          Hold             (boom latched; receiver holds formation)

Hold
  --ReceiverRequestsDisconnect-->  BackingOut   (receiver requests disconnect)
  --ContactLost-->          PreContact       (boom disconnected unexpectedly)

BackingOut
  --AtPrecontactPos-->      PreContact       (backed out to pre-contact position)
  --DisconnectApproved-->   Departing        (tanker approves departure)

Departing
  --1000ftBelowTanker-->     Done            (descended 1000 ft below tanker)
  (entry: brain hands back to NavigationModule with post-refuel route)

Done
  (the AAR rung drops; the brain's ladder falls through to the nav module)
```

### 2.3 The TankerModule — the tanker-side brain

A new `f4-ai/modules/tanker_module.{hpp,cpp}` — the tanker's brain.
It has its own state machine:

```
FlyingTrack                 (flying the refuel track, waiting for a receiver)
  --PrecontactReport-->     ReceiverInPrecontact  (receiver stabilized at precontact)
  --(tanker clears)-->      ClearedContact        (tanker sent ClearToContact)

ReceiverInPrecontact
  --(monitor)-->             ClearedContact        (boom operator clears contact)

ClearedContact
  --ContactMade-->           Refueling             (boom latched)
  --ReceiverDeparted-->      FlyingTrack           (receiver backed out without contact)

Refueling
  --DisconnectRequest-->     Disconnecting         (receiver requested disconnect)
  --ContactLost-->           ReceiverInPrecontact  (boom disconnected unexpectedly)

Disconnecting
  --(tanker reports fuel + clears departure)--> DepartureCleared

DepartureCleared
  --ReceiverDescended-->     FlyingTrack           (receiver departed; back to track)
```

The TankerModule flies the track using the existing `NavigationModule`'s
air-steering (the tanker's `BrainComponent` is configured with
`is_tanker_=true`, which makes the brain's `Phase::Tanker` dispatch
run the TankerModule instead of the takeoff/nav/landing mission
sequencer). The TankerModule monitors the receiver's position (pushed
by the host each tick, mirroring the tanker picture) + manages the
refuel protocol.

### 2.4 The ATC messages — full duplex protocol

The current 7 messages are extended with 5 new ones for the full
duplex conversation:

| Message | Direction | When | Purpose |
|---|---|---|---|
| `RefuelRequest` | Receiver → Tanker | NoTanker entry | "I need a tanker" |
| `TankerAssigned` | Tanker → Receiver | (response) | "Tanker is N, at position P" |
| `PrecontactReport` | Receiver → Tanker | PreContact entry | "I'm at the pre-contact position" |
| `ClearToContact` | Tanker → Receiver | TankerModule clears | "Cleared to close to contact" |
| `ContactRequest` | Receiver → Tanker | ClearedContact entry | "Requesting boom contact" |
| `ContactMade` | Tanker → Receiver | Boom latches | "Contact" |
| `ContactLost` | Tanker → Receiver | Boom disconnects | "Contact lost" (+ reason) |
| `DisconnectRequest` | Receiver → Tanker | Receiver done | "Disconnect" |
| `DisconnectApproved` | Tanker → Receiver | Tanker acks | "Cleared to depart" |
| `FuelTransferred` | Tanker → Receiver | After disconnect | "X lbs received" |
| `RefuelComplete` | (existing, kept) | Receiver fuel target | (kept for compat) |
| `DisconnectMessage` | (existing, kept) | Host-side | (kept for compat) |

### 2.5 The geometry

Two hold points (both offset aft of the tanker along its heading):

| Point | Offset from tanker | Envelope |
|---|---|---|
| **Pre-contact** | 100 ft aft, 30 ft below | ±30 long / ±20 lat / ±30 vert |
| **Contact** | 10 ft aft (boom receptacle), level | ±30 long / ±20 lat / ±200 vert (widened for the spawn transient) |

The receiver flies to pre-contact first, stabilizes, waits for
ClearToContact, then closes to contact.

### 2.6 The "resume route" hand-back

After `Done`, the receiver's AAR rung drops (`refuel_.is_active()`
returns false). The brain's ladder falls through to the nav module.
The RefuelModule's `Done` entry action (or the brain on detecting the
transition) calls `nav_.set_route(post_refuel_route)` +
`nav_.air_steering.reset_integrators()` + `refuel_armed_ = false`. The
post-refuel route is a new field on `MissionPlan`.

---

## 3. Implementation order

1. **ATC messages** — the protocol contract (5 new messages in
   `atc/messages.hpp` + the `StubATC` handlers).
2. **RefuelModule** — the 8-state SM + pre-contact geometry + the
   new control laws for each state (Rendezvous, PreContact, ClearedContact,
   Contact, Hold, BackingOut, Departing).
3. **TankerModule** — the tanker-side SM + track-flying + the protocol
   management (PrecontactReport handling, ClearToContact emission,
   fuel reporting, departure clearance).
4. **BrainComponent** — the `is_tanker_` flag + `TankerModule` member
   + `Phase::Tanker` dispatch + the post-refuel route hand-back.
5. **Scenario structs** — `tanker` bool + per-aircraft `route` on
   `ScenarioAircraft`.
6. **Simulation** — spawn the tanker as a real aircraft, rewrite
   `push_tanker_picture` to read the real FM, delete `ScriptedTanker`.
7. **Scenario file** — two aircraft (tanker + receiver) + the
   receiver's route with a `WP_REFUEL` waypoint + the post-refuel route.
8. **E2E test** — assert the full procedure: rendezvous → pre-contact →
   cleared contact → contact → hold (30s) → disconnect → back to pre-
   contact → fuel report → depart → descend 1000 ft → resume route.

---

## 4. What this replaces

- `f4-ai/include/f4/ai/modules/scripted_tanker.hpp` — **deleted**.
- `ScenarioTanker` struct + the `"tanker"` JSON block — **deleted**.
- `std::optional<ScriptedTanker> tanker_` + `push_tanker_picture` (the
  ScriptedTanker version) — **replaced** with a real-aircraft spawn +
  a `push_tanker_picture` that reads the tanker's FM.
- The 5-state `RefuelState` — **replaced** with the 8-state SM.
- The `TankerConfig` in `stub_atc.hpp` — kept (the stub still needs the
  tanker's initial position for the `TankerAssigned` response), but the
  `tanker_entity_id` is now the REAL tanker entity ID, not a sentinel.

---

*This document is the AAR redesign plan. It supersedes the Tranche D
section of `LANDING_PRECISION_FORMATION_AAR_PLAN.md`.*
