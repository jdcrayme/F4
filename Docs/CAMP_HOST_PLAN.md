# Campaign Host — the Engine Contract (CAMP)

> **Status**: Draft v1 — CAMP-HOST-1 shipped (f4-campaign-api + the
> EngineSessionHost adapter + `campaignd`); CAMP-HOST-2 shipped (the
> typed event stream over f4-messaging, the JSONL journal, the replay
> identity); CAMP-HOST-3 shipped (the world viewer refactored onto the
> contract — the runner relocated out of the engine, the `threat`
> query landed additively, the golden identity intact); CAMP-CMD-1
> shipped (`roe_set` riding the P7 fire-control path, the
> command journal + its tick-exact replay, the `roe_changed` event
> publishing); CAMP-CMD-2 shipped (`flight_retask` /
> `flight_abort` / `objective_priority` — the v1 command surface
> completes, the abort scrubs books and routes RTB, the objective
> priority write feeds every tasking score, the replay identity
> extends to the full intervention set); CAMP-ATM-1 shipped (the
> ACTION tables — objective-damage-driven CAS/BARCAP/
> SEADSTRIKE filings, the SWEEP station lines, the tanker waypoint,
> the `action_filed` event family — the ninth — plus `actions_filed`
> in the ledger and on the QC line); CAMP-INIT-1 shipped with this
> patch (create-from-parameters — the scenario pack →
> `CampaignInitializer` → fresh `.cam` via the Task-70 encoder stack,
> byte-identity by construction, the generated save decodes in the
> existing reader, the C5 24-hour harness passes on a generated war,
> small/medium/large generated fixtures, the G1 two-war-pair bed);
> CAMP-SCALE-1 shipped (the Tier-3 full-data pass — the complete
> UCD/VCD/WCD tables as `f4.theater.tables/1` JSON, the runtime
> `TheaterTables` reader, the VCD countermeasure supply chain, the
> gated pilot-skill flow, the uncapped-fleet certificate);
> CAMP-DOM-1 shipped (victory scoring — the books' projection: the
> `verdict` query + the `verdict` event family (the tenth), the
> territorial census weighted by the objective's own priority byte
> against the session's opening owner, the ledger's team rows riding,
> the band territorial-only with a tie for the lead being no lead).
> CAMP-DOM-2 shipped (supply depth — the per-objective pool: the team
> strategic stocks (`.tea` supply_avail/fuel_avail, now parsed and
> adapter-exposed) regenerate their held objectives' clamped
> supply/fuel stocks, battalions DRAW from the nearest own-held
> objective within the supply radius and are CUT OFF beyond it, the
> `last_repair` cadence heals features at a supply-gated rate and
> books the `objective_repaired` family (the eleventh), the strategic
> reserve (`replacements_avail`) refills consumed reinforcement
> budgets, the `objectives` query serves the live mirror (the DOM-1
> seam closes), and every knob defaults OFF — the goldens stand);
> CAMP-DOM-3 shipped with this patch (personnel — the reference's
> `AssignPilots()`: every filed flight draws its CREW from the
> squadron's decoded pilot roster (lead = the front-third scan,
> wingmen backward from the tail, a squadron that cannot crew is
> skipped), the per-role effectiveness table (`.uni` rating[16]) decays
> 25% per assignment and re-prices FindBestAir, the personnel books
> book assignment/loss/recovery, the pilot event trio (the twelfth
> through fourteenth families) rides the stream, the `squadrons` query
> serves the personnel face, and every knob defaults OFF — the
> goldens stand); CAMP-DOM-4 shipped with this patch (airbase
> scheduling — the reference's FindTakeoffSlot depth beyond FID's
> airfield-ops windows: the slot grid slides with the clock (a moving
> epoch — past blocks fall off, the 160-minute horizon stops
> silencing late filings), FindBestAir's gate applies the reference's
> own previous-block rule and books its denials, a scrubbed flight's
> still-future slot releases, a horizon refusal books instead of
> staying silent, the `slot_denied` event family (the fifteenth) and
> the `airfields` query serve the scheduling face, and the intents
> carry the scheduled takeoff so the sim's ops window arms against
> the SLOT — the FIDELITY_TIERS §7 delivery-latency divergence
> closes. Also restored here: the DOM-3 personnel test files the
> previous commit missed (they were untracked — the tree they
> shipped in could not build). Every knob defaults OFF — the
> goldens stand); CAMP-DOM-5 shipped with this patch (naval — the
> wrap-then-decide: the upstream NavalTaskingManager is a 15-byte
> flag shell, so the naval face maps onto the ATM pipeline's
> REQUEST VOCABULARY — the anti-ship family (AMIS_ASHIP) files at
> the enemy's task forces through the ranked pool
> rank_taskforce_targets owns (own-shore distance, wire-order
> ties), the targeted filings route like strikes and publish on the
> SAME mission_filed event (no new family), the per-target filing
> books ride the additive `taskforces` query, and the "how deep"
> record names what a deeper naval tranche would take (movement,
> threat painting, carrier airbases, task groups — each its own
> tranche). Every knob defaults OFF — the goldens stand).
> Every other tranche below is an acceptance contract, not a claim.

The campaign engine is the product. Every user experience — the world viewer
today, a map-first strategy UI, a war-room dashboard, a scripted AI observer,
a future 3D host — is a **client** of one engine. This document defines the
contract that makes the engine embeddable, the reference host that proves it,
and the tranche plan that lands the contract without breaking a single golden.

---

## 1. The problem

The engine is real but its interface is **implicit**. Five call sites already
speak private dialects of the same engine:

| Call site | Dialect |
|---|---|
| WorldState→JSON emitter (Task 62) | one-way snapshot dumps |
| World-viewer campaign session (V-CAMP) | direct in-process calls + time controls |
| `campaign_qc` | harness flags and exit codes |
| The campaign diary / result ledger | stats lines and MD5-identity books |
| Scenario keys (`combat.countermeasures`, `strategy_layer`, …) | init-time toggles |

Every future UX would re-wire some subset of these. The root cause (one
sentence): **the engine exposes capabilities, not a contract** — there is no
versioned, pure-data boundary that says what a host may ask, command, and
observe.

The precedent exists in-house: `f4-flight-api` is a tiny header-only boundary
library (`i_aircraft_state.hpp`, `i_pilot_input_sink.hpp`, `pilot_input.hpp`)
that the flight model implements and hosts consume. The campaign gets the
same treatment at one level up.

## 2. Principles

1. **Pure data at the boundary.** The contract carries typed structs with
   canonical, byte-stable JSON encodings (the `CampaignResultLedger::to_json`
   discipline, generalized). No engine types, no entity handles, no
   callbacks-with-state leak across.
2. **The engine is not real-time.** The contract advances ticks
   (`step(n)`); pacing, animation clocks, and wall-clock smoothing are host
   concerns. This is what makes 60× acceleration, pause, and headless
   regression the same code path (the FID certificates already prove it).
3. **Observation is free; intervention is journaled.** Queries and event
   subscriptions never mutate. Commands are appended to a command journal
   with their apply-tick, and replay is (save, seed, journal) → identical
   identity fingerprint.
4. **Golden identity by default.** A session with no client attached behaves
   byte-identically to today (the P6 countermeasures lesson: the gate is the
   story). Every feature arms by use, not by default.
5. **The viewer is just a client.** If the world viewer can be refactored
   onto the contract (HOST-3), the contract is done. Until then it is not.
6. **One subsystem, one plan.** This doc owns the host surface only.
   Campaign domain depth (ATM, ground war, weather) stays in its own plans;
   where this doc absorbs a named queue item, §10 records the handoff.

## 3. The four surfaces

The contract is four surfaces. Everything below exists in embryo; the tranches
formalize, they do not invent.

### 3.1 Lifecycle

```cpp
// f4-campaign-api/include/f4/campaign/api/session.hpp
namespace f4::campaign::api {

struct SessionConfig {
    std::string save_path;      // the .cam (or world JSON) to decode
    std::uint64_t seed;         // the session RNG root (defaults: save's seed)
    FidelityPolicy policy;      // §4
};

struct IdentityFingerprint {
    std::uint64_t seed;
    std::string save_hash;      // sha256 of the decoded inputs
    std::string ledger_md5;     // the C5/C6 identity, on demand
    std::uint32_t protocol_version;
};

class ICampaignSession {
public:
    virtual ~ICampaignSession() = default;

    virtual IdentityFingerprint identity() const = 0;

    // Deterministic advancement. Commands submitted before a step apply at
    // that step's first tick boundary, in submission order (§5).
    virtual void step(std::uint32_t ticks) = 0;
    virtual void set_time_scale(double scale) = 0;   // pacing hint only
    virtual SaveResult save(std::string_view path) = 0;  // the CampaignSaver path
};
}
```

The implementation is an adapter in `f4-simulation` (which already links
f4-campaign, f4-entities, and the world): `CampaignSession` composes
`Campaign`, the `EntityWorld`, and the existing save/ledger machinery. No
engine internals move.

### 3.2 Queries (host → engine, read-only)

Versioned DTOs with byte-stable JSON. The v1 query set maps one-to-one onto
things the engine already produces:

| Query | Source (already landed) | Payload shape |
|---|---|---|
| `theater` | f4-world decode structs | objectives, links, datum, bounds |
| `objectives` | WorldState `fstatus` (C1 write-back) | per-objective state, damage, team |
| `flights` | FID tiered view (FID-VIEW-1) | aggregates by default; live entities inside focus (§4) |
| `routes` | RouteBuilder output (C3) | waypoints, profiles, station contracts — **v1.1 (additive; not in HOST-1)** |
| `threat` | threat map (C3) | viewer_team, cell_grid, cells, both density bands — **v1.1 additive, LANDED in HOST-3** |
| `books` | `CampaignResultLedger` (C1/C2) | `{"ledger_json": "…"}` — the byte-stable ledger as one escaped string (its own writer is pretty-printed; one client decode = exact ledger bytes) |
| `tasking` | the ATM pipeline state (C4/P7) | requests, packages, `next_tasking_sec` |
| `weather` | Weather v1 (Task 73) | condition, twilight band, daylight factor — **v1.1 (additive)** |
| `time` | session clock | tick, sim seconds, cycle countdown |
| `verdict` | the books' projection (DOM-1) | t (relative), threshold, band, leader, per-team census + ledger rows — **v1.1 additive, LANDED in DOM-1** |
| `squadrons` | the personnel face (DOM-3) | one row per squadron: identity, tasking availability, the roster's run deltas, the ratings — **v1.1 additive, LANDED in DOM-3** |
| `airfields` | the schedule books (DOM-4) | one row per booked airbase: the 32-block grid as 64 hex chars, the anchor (`epoch_min`), the set-bit count, the denial books — **v1.1 additive, LANDED in DOM-4** |
| `taskforces` | the naval face (DOM-5) | one row per task force in the WORLD's own wire state (domain-4 units, wire order — present with or without the arm): the VU pair, the owner slot, the sea subtype + the class table's name, position, the wire's own destination, the supply byte, and this run's anti-ship filing book — **v1.1 additive, LANDED in DOM-5** |

```json
{"v":1, "op":"query", "q":"flights", "team":0}
```

The response DTOs live in `f4-campaign-api` as structs + `to_json`/`from_json`
so a C++ host and a JSON host see the same wire. Goldens pin a canned
TestCamp session's responses byte-for-byte.

### 3.3 Commands (host → engine, journaled)

A closed `CommandIntent` variant. V1 is deliberately minimal — RoE and
retasking — because P7 already proved the command→engine path end to end
(the `roe_check` byte rides seed → request → flight → intent → fire
controls).

| Intent | Semantics | Upstream mapping |
|---|---|---|
| `roe_set {scope, roe}` | scope = team / mission / flight; roe = 0 free, 1 tight (BVR suppressed), 2 hold | the P7 RoE carry + the ATM queue's RoE doctrine |
| `flight_retask {flight, mission, target}` | replan from current position (NavigationModule replans; TOT recomputed) | upstream ATO frago changes |
| `flight_abort {flight}` | abort profile → RTB; the package's books close | upstream mission scrub |
| `objective_priority {objective, weight}` | feeds the tasking score | `GetPriority` terms |
| `focus {pos, radius_nm}` | the fidelity focus region (§4) | generalizes the camera bubble |
| `select_deagg {aggregate}` | FID's click-deagg, as a command | FID-1's viewer click |

```json
{"v":1, "op":"command", "intent":"roe_set",
 "scope":{"kind":"team", "team":1}, "roe":2}
```

CAMP-CMD-1 as-built (roe_set): the scope joins the session's doctrine
store — ONE level per scope (a re-set REPLACES that scope's level),
and an aircraft's EFFECTIVE level is the tightest of its carried P7
byte and every matching scope (a wider scope's hold is a ceiling;
loosening happens at the scope that tightened). The write goes through
`Simulation::set_flight_roe` — the FULL recompute from the doctrine
baseline, so a command can LOWER as well as tighten (the old
`apply_flight_roe` was a tighten-only ratchet; it remains for the
spawn cadence's original vocabulary). Zero-values are non-targets
(team 0 / mission 0) and a flight scope must name a roster flight —
refusal is data (InvalidArgument / UnknownFlight). Every applied
roe_set publishes `roe_changed` (the pinned encoder, the scope echoed
verbatim).

CAMP-CMD-2 as-built (the retask/abort/priority writes): the session is
the write's composition point and the shapes are explicit. A retask
rebuilds the route through the session's own RouteBuilder (home
airbase → target, threat-aware), SPLICES at the new plan's ingress
point (the kWpfIp waypoint; the takeoff leg is discarded), and heads
with the flight's CURRENT position; TOT = now + the cruise-speed
travel estimate and mission-over = TOT + loiter + return + the doubled
reserve (compose_packages' formula, retask-shaped). The write lands on
every shape: the aggregate row (FlightAggregateEngine::retask — SPEED
mode from the retask point, cursor at index 1; a TIME-mode save flight
retasks INTO speed mode), the stored synthetic intent, a save flight's
world WaypointPlanComponent (a later deagg spawn flies the NEW plan),
the live brains (BrainComponent::retask — Enroute hands the route
straight to the NavigationModule with reset steering; Ground keeps the
swap for the takeoff handoff; Approach/Complete refuse), and the ATM
booking (Campaign::reschedule_flight — the recovery clock follows the
new plan, the takeoff slot survives). An abort of a NOT-LAUNCHED
flight scrubs the aggregate (FlightAggregateEngine::scrub — a distinct
terminal state: tick, the tier triggers, the ops windows, and the air
picture all skip it; a parked live complement folds back and retires)
and an airborne flight flies its RTB leg (route = [current position →
the route's own landing waypoint]); either way the books close NOW —
Campaign::scrub_flight releases the booking's survivors through the
ledger's apply_mission_recovery at the current clock (save-carried
flights have no booking in THIS ledger — operational abort only), and
the session's abort record keeps every future trigger from
resurrecting the sortie. `objective_priority` writes the objective's
priority byte (0..100) in the session's WorldState — the first runtime
write of the field; every scoring site reads it live (the request
target term, the CAP station ranking, the enemy rotation, the legacy
select_target), the `objectives` query echoes it, and the contract
save() persists it. Refused commands mutate nothing; the wire's
typed refusals gain `unknown_objective`.

Refusals are typed (`CommandAck {status: applied | refused(reason), apply_tick}`),
not exceptions across the boundary. Refusal is data: a UX may surface "can't
retask — flight is in merge" as UI.

### 3.4 Events (engine → host, ordered, typed)

The diary generalizes into a typed event stream. The engine emits through
f4-messaging topics (its existing type-safe bus); the journal and the wire
are two sinks of the same stream. The kill event's shape is already fixed by
the C1 ledger — "killer squadron/team/flight, victim squadron/team/flight,
t=356.7 s" — it becomes an event, not just a book entry.

| Event family | Emitter today |
|---|---|
| `mission_filed` / `mission_shared` | C4 tasking + P7 FindSupportFlights (`supports_filed`/`supports_shared`); the naval wrap's anti-ship filings ride the SAME family (DOM-5 — the target id is the task force's VU, no new family) |
| `flight_launched` / `flight_recovered` | spawn path + FID airfield-ops windows |
| `engagement_opened` / `kill` / `loss` | the combat passes + ledger books (C6) |
| `objective_damage` / `objective_captured` | fstatus diff (C1) + GroundWar (G1) |
| `objective_repaired` | the repair cadence's fire — the front healed (CAMP-DOM-2) |
| `reinforcement_delivered` | C2's reinforcement fire |
| `tasking_cycle` | the 7-phase ATM pass + `next_tasking_sec` |
| `action_filed` | the ACTION tables' damage reactions (CAMP-ATM-1) |
| `verdict` | the books' projection changed — band or leader (CAMP-DOM-1) |
| `slot_denied` | the slot grid refused a flight — the pick gate or the horizon (CAMP-DOM-4) |
| `weather_changed` | Task 73's Markov chain |
| `roe_changed` | P7 fire-control gates |

```json
{"v":1, "ev":"kill", "t":356.7,
 "killer":{"sq":214, "team":0, "flight":118},
 "victim":{"sq":317, "team":1, "flight":233},
 "weapon":"aim7"}
```

Delivery rules: events are journaled at engine rate (complete) and delivered
per `step()` return, filtered by the host's subscription (kinds + teams).
No real-time scheduling in the engine — a realtime host subscribes and
flushes on its own clock.

## 4. Fidelity policy is a UX parameter

The FID tier system is the first UX-sensitive engine feature: flights are
aggregates until the camera bubble, an airfield-ops window, or a click
deaggregates them. That is exactly the right shape — it just needs to stop
being viewer-shaped.

- `FidelityPolicy {mode: full | tiered, accel, max_live}` is a
  `SessionConfig` field (maps onto the existing tiered/full flags and the
  `--accel` / `--accel-max-live` certificate knobs).
- The **focus region** (`focus` command) is the generalized camera bubble: a
  map-only UX never sets focus → the war stays aggregate at 60× forever; a
  3D host tracks its camera to it → deagg happens where the user is looking.
  `select_deagg` is FID's click, as a command.
- The airfield-ops window and the event-driven combat deagg triggers
  (FID-5) stay engine-side — they are doctrine, not presentation.

Nothing in FID changes. The policy struct and the focus command are the same
machinery wearing its contract hat.

## 5. Determinism: seed, journal, identity

- Every session has a seed (the save's, unless overridden). The seeded
  determinism the sim already enforces (identical ledger MD5 at 20× and 60×)
  extends across the boundary: **replay(save, seed, command_journal) →
  identical `identity()`**.
- Commands apply at the next tick boundary of the session's own clock, in
  submission order; the ack states the apply-tick. No wall-clock anywhere.
- The EVENT journal is append-only JSONL (one line per event, engine rate,
  unfiltered) and the COMMAND journal (CAMP-CMD-1) is its intervention
  counterpart: one line per APPLIED command — `{"apply_tick":T,"t":S,
  <intent body>}` — where `apply_tick` is the ENGINE TICK INDEX (whole
  sim_dt steps since session start; the host owns the stepping
  accumulator, so it is the one clock a replay reproduces exactly) and
  `t` is the campaign seconds the record's ack carried (audit context).
  Refused commands journal nothing — they mutate nothing. Neither file
  appears for golden runs; both are written only when the host asks
  (`--journal` / `--command-journal`).
- Replay mechanics (CAMP-CMD-1 as-built): the replaying host segments
  its stepping around the journal's pending apply ticks, so each command
  lands at EXACTLY the tick the record applied it at — the replay's step
  CHUNKING is irrelevant (step(200) and 2×step(100) reproduce the same
  war; pinned by tests). At EOF the session's final identity must equal
  the journal's footer — "replay-with-commands reproduces the books" is
  a byte comparison, and `campaignd --replay-commands` turns any
  divergence (header, leftover commands, footer) into exit 23.
- Identity is checkable by any client: the fingerprint (seed, save hash,
  ledger MD5 on demand) is the same MD5 the C5 harness pins.

## 6. Code ownership

| Piece | Home | Depends on |
|---|---|---|
| `f4-campaign-api` (header-only: DTOs, intents, events, session iface) | new top-level lib | f4-json, f4-io, f4-geo, f4-units (neutral, per the ASSET_PIPELINE_SPEC §10 property scheme) |
| `CampaignSession` adapter | `f4-simulation` | f4-campaign-api + the engines it already links |
| `campaignd` (the reference host) | new app target (the `campaign_qc` sibling) | f4-simulation, f4-json |
| Command journal reader/writer | `f4-campaign` (beside `CampaignSaver`) | f4-json, f4-io |
| Typed events + topics | `f4-campaign` (diary generalization) + f4-messaging | f4-messaging |
| Scripted golden client | `scripts/` (drives `campaignd` over stdio) | none (any language) |

## 7. The reference host: `campaignd`

A headless process that speaks the contract over **line-delimited JSON on
stdio** — no sockets in the engine, ever; a realtime UX bridges stdio→socket
on its side of the boundary.

```
campaignd --save TestCamp.cam [--seed N] [--journal war.jsonl] [--policy tiered] [--accel 20]
```

Session flow: on start the host emits a `hello` reply carrying the
`IdentityFingerprint` and protocol version; then it answers one line per
request. `step` advances; queries return DTOs; commands ack; events flush
after each `step` per subscription.

Exit codes (a new namespace — `campaign_qc` owns 6–17):

| Exit | Meaning |
|---|---|
| 0 | green |
| 20 | protocol violation (malformed line, bad `v`) |
| 21 | unknown query |
| 22 | command refused (refusal reason in the last ack) |
| 23 | identity drift (replay fingerprint mismatch) |
| 24 | engine operation failed (save / query) |

## 8. Tranches and acceptance gates

Each tranche is one landed patch series with one line in `CHANGELOG.md`.
Default behavior is byte-identical unless a gate says otherwise.

### CAMP-HOST-1 — the contract + the host (SHIPPED with this patch)
- `f4-campaign-api` scaffold (session iface, v1 query DTOs, command wire,
  event vocabulary, line protocol — header-only, f4-json + std ONLY);
  the `EngineSessionHost` adapter in f4-simulation (wraps the engine's
  own `CampaignSession` — queries serve flight_tiers/intents/ledger/
  WorldState verbatim; commands forward the FID family and REFUSE the
  CAMP-CMD queue with the tranche named); `campaignd` (stdio JSON,
  exit namespace 20/21/22/24/25).
- **Gate (as built)**: with no client attached, engine behavior
  byte-identical; 58 new ctest cases — the contract pinned against a MOCK
  session (no engine in the link) AND end-to-end against the real engine
  session (the tier rig's crafted world); the books query wraps the
  ledger's byte-stable JSON as one escaped string (one client decode =
  the exact ledger bytes — the identity fnv hashes those same bytes);
  the golden client rides the same dispatcher.
- **The step() discipline (pinned by the tests)**: `advance(ticks ×
  sim_dt, override = ticks)` — the engine's accumulator drains whole
  ticks and carries sub-tick rounding residue to the NEXT step, so a
  per-call drain near an exact-second boundary may land one tick short
  (the session's TOTAL time stays exact — the C5 identity harnesses pin
  that). The campaign clock's whole-second fires follow the same
  accumulator and can chunk several seconds into one fire.

### CAMP-HOST-2 — the event stream + journal (SHIPPED with this patch)
- Typed `CampaignEvent` over f4-messaging (ONE bus message type — the
  tagged envelope of the pinned v1 families); the `--journal` JSONL
  writer + the byte-exact verifier; the wire's `subscribe` op and the
  step response's `"events":N` framing.
- **Gate (as built)**: journal replay of the C6 acceptance fight (the
  FID-5 combat rig's head-on merge through the host, armed) reproduces
  the ENTIRE stream byte-for-byte and closes with the same ledger
  fingerprint (`--verify-journal` turns any divergence into exit 23,
  drift outranking the refusal rule at EOF); an empty-journal save is
  byte-identical to an un-journaled one and an eventless journal is
  exactly two lines (the identity header + footer); one golden journal
  line per event family pinned in the contract tests (the engine-backed
  e2e pins tasking_cycle/mission_filed on the kunsan rig and the kill
  pair on the combat rig).
- **As-built notes**: (1) the EMITTER is the session (f4-simulation),
  not f4-campaign — the ledger books stay the war's truth and their
  bytes stay untouched (the identity anchor); events are derived at the
  same call sites that move the books (mission_filed from the intent
  publish, tasking_cycle/reinforcement_delivered from the cadence
  call, objective_captured from the capture log's tail, objective_damage
  from the sink's per-pass collection — the owner needs the session's
  WorldState), so an event stream REQUIRES no engine-internal redesign
  (plan §12) and a no-client session emits nothing (no subscription,
  no buffer, byte-identical). (2) The kill event publishes at the
  RESULT SINK (f4-simulation), where killer team + weapon family live;
  `EntityKilledMessage` carries a `cause` literal ("missile"/"gun") —
  additive, defaulted, no publisher changed but the two batteries.
  (3) weather_changed emits from the scenario session's WeatherSystem
  (a condition-turn observer); campaign sessions build no WeatherSystem
  — the family's golden rides the contract tests until a scenario host
  needs the wire. (4) roe_changed LANDED with CAMP-CMD-1: the publisher
  is the session's roe_set path (apply_roe_command), t = the ladder's
  relative seconds, the scope echoes the command's verbatim — the
  pinned encoder's bytes unchanged. (5) Event `t` is the ENGINE's relative
  seconds (the books' own axis); a host adds the epoch from hello's
  campaign_time_s. (6) The wire is HOST-1-identical for a client that
  never subscribes ("events":0 and zero event lines — features arm by
  use).

### CAMP-HOST-3 — the viewer becomes a client (SHIPPED with this patch)
- World-viewer campaign session refactored onto `ICampaignSession`
  (in-process); the V-CAMP window's reads are QUERIES, its acts are
  typed COMMANDS; the engine session itself is reachable only through
  a quarantined render-plane seam.
- **Gate (as built)**: viewer feature parity — time controls (play/
  pause, the 1x/10x/60x/240x presets, the measured-rate readout, the
  D# HH:MM:SS clock), the flights table (rows from the `flights`
  query; D/R as `select_deagg`/`select_reagg` commands with typed
  refusals surfacing in the status line), the generated-missions
  table (the `tasking` query with the additive `route_waypoints` +
  `flight_role` tail; TOT = the epoch captured from the `time` query
  at adopt + the relative TOT — the engine's own formula), the
  campaign view (the `stats` query's war-status block; Write Result
  JSON via the `books` query; Write Back via the contract's runtime-
  safe `save()`), and the threat overlay (the `threat` query — the
  v1.1-additive name this tranche lands: viewer_team, the cell-grid
  echo, and both density bands, 171×171 on the kunsan war).
  **The deleted-lines count is the proof: 981 deleted / 2327 added** —
  the engine sheds 825 lines of host-side composition
  (campaign_session_runner + its test, moved to the viewer and
  rewritten over the contract), and the viewer's window/canvas code
  drops every direct engine-session call outside the two seams below.
  23 new ctest cases: the relocated runner pinned on a MOCK session
  (no engine in the link — pacing, pause, dilation echo, the FIFO
  frame-pattern starvation pin, lifecycle idempotence), the query
  walks pinned against golden DTO JSON (order-independent,
  additive-tolerant, malformed-reads-as-defaults), the `threat`
  whitelist dispatch, two ThreatView goldens, and four engine-backed
  HOST↔engine PARITY cases on the kunsan rig (flights/tasking/stats/
  threat rows diffed field-for-field against flight_tiers, intents,
  stats, and the route-builder's map).
- **As-built notes**: (1) THE TWO PLANES — the contract governs
  campaign STATE; the live entity graph (per-vehicle transforms,
  models, selection rings — the FID focus bubble's materialized
  roster) stays on the engine's EntityWorld through a named,
  quarantined render-plane seam (the viewer_state.hpp helpers, all
  commenting their `engine()` reach). That is a RENDER concern (plan
  §2.2), not state access; a remote 3D client that needs the roster
  over the wire gets a `vehicles` query in its own additive tranche.
  (2) THE RUNNER LEFT THE ENGINE — pacing is host-side composition
  (§2.2 said so all along); `CampaignClientRunner` drives
  `ICampaignSession::step(ticks)` and owns the wall→tick accumulator
  the engine's advance() used to carry, with the same FIFO FairMutex
  discipline (relocated) and an adaptive tick budget — now CEILING-
  clamped at 4096 ticks, fixing the unbounded-doubling int overflow a
  real war's advance cost always masked. The step-serial gate keeps
  the windows' "refresh once per advance, never per draw" contract
  over JSON round-trips. (3) The factory seam stays: the viewer
  assembles `CampaignSessionOptions` and calls
  `EngineSessionHost::create` — exactly what campaignd does with
  argv; engine config assembly is host work, not client reaching.
  (4) The epoch: a fresh session's first `time` query (paused, zero
  ticks) reads campaign_time_s = the save's epoch; the missions
  table's absolute TOT adds its relative TOT onto that. (5) Data-
  vocabulary headers (mission names, the world-JSON schema the
  static layers parse) remain legitimate host-side resources — the
  boundary quarantines f4-simulation STATE, not display tables; the
  flights table's team names come from the viewer's own world parse.
  (6) Write Back upgrades, on purpose: the old in-memory-only
  writeback button now performs the contract's runtime-safe save
  (write-back + WorldState JSON next to the loaded world) — parity
  or better, and the .cam re-encoder stays the importer's process.

### CAMP-CMD-1 — command journal + RoE doctrine (SHIPPED with this patch)
- `CommandIntent` wire + journal; `roe_set` per team/mission/flight riding
  the P7 fire-control path; RoE-driven threat walls (the 32000 overfly
  denial) as doctrine policy — the ATM plan's "RoE refinement tranche"
  lands here, command-shaped.
- **Gate (as built)**: post-arm fire-control behavior per P7 (1 = BVR
  suppressed, 2 = everything held) — pinned BOTH at the gate level
  (`SetFlightRoe` recomputes 2→1→0 against the armed baseline, gun
  budget untouched) and at the OUTCOME level (the combat rig's t=13
  kill pair: both teams held at t=2.5 s kills nobody — the command
  rides the fire-control path into the books). Replay-with-commands
  reproduces the books: the record's 3-command journal re-applied at
  its recorded ticks regenerates the record's `ledger_fnv` in ONE step
  call, in the record's own pattern, and in 25 × step(4) — chunking is
  irrelevant; a replay cut short leaves its commands pending (the
  reference host exits 23); a released-team tamper diverges the books
  (exit 23 with the fingerprints named); a wrong war fails the header
  at load (23); a malformed journal is 24. No commands → byte-identical
  (the adopt cadence's recompute restores the armed baseline values
  exactly — the C5/C6 identity suites stayed green).
- **As-built notes**: (1) the journal lives in f4-campaign-api
  (`command_journal.hpp` — writer + reader + byte goldens; the
  intent-body encoder in `commands.hpp` gives every intent a canonical
  wire spelling), the SINK lives in the host (`set_command_journal_sink`),
  and the REPLAY segmentation lives in `EngineSessionHost::step` — the
  identity statement runs through the same step() a live client drives,
  no private replay dialect. (2) The deagg inheritance: an FID-5 deagg
  spawn now takes the flight's effective doctrine at spawn (one call —
  the adopt cadence would re-impose it within a second anyway); the
  rigs' zero-roe worlds never see a byte move. (3) `roe_changed` uses
  the scope encoder shared with the journal line (one byte shape, two
  consumers). (4) campaignd composes `--replay-commands` with
  `--verify-journal` — the full assertion (same war, same commands,
  same events) is one flag pair.

### CAMP-CMD-2 — retask / abort / priority (SHIPPED with this patch)
- `flight_retask` (replan from current position, TOT recomputed),
  `flight_abort` (RTB profile, package books close), `objective_priority`
  feeding the tasking score; `GetPriority`'s PO/package terms (the ATM
  queue item) become policy inputs.
- **Gate (as built)**: the M4/M5-style pinned retask — a save flight
  retasked mid-route flies the NEW tasking: the row carries the new
  mission byte and the recomputed mission-over deadline, the aggregate
  closes on the new target (or, when the recomputed TOT arms the FID
  delivery window, the deaggregated aircraft carries the NEW plan with
  the target waypoint aboard its brain — the retask's entity write at
  work), no teleport (the retask position IS the head waypoint). The
  abort gates: a mid-route abort flies the RTB leg home (the row
  reports the additive `aborted` tail; a second abort refuses) and a
  kunsan package's abort closes the books EXACTLY once (the complement
  returns to the pool at the current clock; the booking is gone; no
  release at the old deadline — pinned engine-side too). The priority
  gates: the write echoes through the `objectives` query and the
  generated requests' target term moves with the byte (the ATM unit
  pin: two worlds differing only in the objective's priority score
  differently). The identity statement extends to the full
  intervention set: a kunsan journal carrying retask + priority +
  abort regenerates the record's `ledger_fnv` through ONE step call
  and the record's own pattern; the tampered-flight-id replay exits 23
  with both fingerprints named. The typed refusals (unknown flight /
  unknown objective / an untaskable mission byte / weight 101) are
  pinned on the wire.
- **As-built notes**: (1) the retask route is the session's own
  RouteBuilder build spliced at the INGRESS point — the new plan's
  takeoff leg is discarded and the flight's current position rides as
  the head (RouteWaypoint form for the intent/entity/plan vocabulary,
  WaypointState for the engine); TOT/mission-over use the ATM's own
  estimate arithmetic (straight-line travel at the cruise constant +
  the profile's loiter + the doubled reserve). (2) The ABORT booking
  rule: a save-carried flight's books closed in the save's own
  history — the abort is operational only (no phantom recovery); a
  filed package's booking releases its survivors NOW
  (drawn − booked losses, the recover_completed formula verbatim,
  `AtmStats.flights_scrubbed/aircraft_scrubbed` counted). (3) The
  scrubbed aggregate is a DISTINCT terminal state (not destroyed, not
  arrived) — the flights row's `aborted` tail (the DTO rule: additive,
  at the END, always present) reports both it and the RTB-ing abort;
  the tier triggers, the air picture, and force_deagg skip aborted
  flights forever (a resurrected sortie would fork the war). (4) The
  home airbase resolution order: the ATM booking first (the commit's
  own record — authoritative for every filed flight), then the
  squadron entity / origin stamp (the session's VU maps are the
  crafted-world path; the kunsan-shape worlds carry no VU properties
  into the sim world, so the booking is the reliable anchor).
  (5) `objective_priority` mutates the session's WorldState row — one
  truth; the scoring sites, the query, and the save all read the same
  field, and no new plumbing exists (the decorator/hook alternatives
  were rejected: the field IS the policy input upstream). (6) campaignd
  gains no flags — the commands ride the CMD-1 journal/replay
  machinery verbatim (`--command-journal` / `--replay-commands`), and
  the reference demo records retask + priority + abort and replays it
  clean (exit 0) and tampered (exit 23).

### CAMP-ATM-1 — the ACTION tables (the ATM plan's named queue, now observable)
- Objective-damage-driven contextual CAS/BARCAP/SEAD filings; SWEEP station
  lines; tanker waypoints.
- **Gate (as built)**: the ACTION tables scan the objectives' fstatus
  bitmaps at every strategy-armed `generate_requests` — an OWN objective
  with destroyed features files CAS over it (`kActionDefend`), heavy
  damage (≥ 25% destroyed) adds the garrison BARCAP station, an enemy
  objective at war files SEADSTRIKE against it (`kActionPunish`) —
  into a per-team pending queue (dedup vs the queue and the booked
  flights: a standing garrison does not re-file until its flight
  recovers; capped by `max_pending_action_requests`), drained ahead of
  the ladder walk with a +25 priority bonus (the war's reactions task
  before its routine). The SWEEP family (the contested-air line) gets
  a real enemy-objective target on its own rotation cursor, and under
  the sweep arm the builder flies the LINE — the attack run extends
  through the target along the inbound axis (two turnpoint legs, the
  sweep action byte 22). Tanker stations gain the refuel waypoint (the
  WP_REFUEL turnpoint, backoff grid before the racetrack anchor, the
  fuel-planning slice's consumer-facing marker — mission-byte gated to
  AMIS_TANKER). Every filing books the ledger's action-filing log at
  the Campaign (the one ledger writer; the optional `actions` section
  keeps every disarmed document byte-identical) and publishes as
  `action_filed` — the ninth event family (team-matched to the filing
  side), after the tasking_cycle that generated it. The kunsan QC run
  prints `actions=` alongside the strategy counters; the save's own
  14 damaged objectives drive 96 filings over 8 cycles, and the
  campaignd demo journals the stream (verify clean exit 0, a tampered
  filing names its line at exit 23).
- **As-built notes**: (1) the reference's ACTION tables live in
  aiinput.dat values our sources cannot see — the same documented
  limitation as the racetrack dimensions — so the table is the
  deterministic subset named at `ActionSystemType` (Defend/Punish/
  Sweep), the context byte IS the driving objective's own type byte,
  and the damage read is the fstatus bitmap (2 = destroyed, 1 =
  damaged-present only, 3 = the no-data nibble, ignored; the effective
  count falls back to the bitmap's own capacity when the save carries
  none — the kunsan shape). (2) The ACTION filings BYPASS the team's
  mission-priority table (the reference files what the situation
  demands; the tempo budget and the deconflict gate still bound the
  fleet) and persist while the damage persists — the standing-garrison
  behavior, bounded per cycle by the cap and the pool. (3) The P7
  acceptance line changes BY DESIGN — the +25 ACTION bonus reorders
  the tempo budget (reactions before routine), so the kunsan run's
  support/enemy-CAP counts shifted (`supports=17 shared=7
  enemy_caps=8 actions=96` vs the pre-ACTION `supports=85 shared=115
  enemy_caps=48`; `stations=96` unchanged — the CAP station targeting
  rides generate, not compose). The exit-17 gate (drew aircraft,
  stationed nothing) stands. (4) The seeded backlog requests carry the
  save's own action_type/context as telemetry but are never booked as
  THIS run's filings (the save's history is not news); the SWEEP
  ladder tag (`kActionSweep`) is telemetry too — the ledger log and
  the event family carry only the damage reactions. (5) The `action_
  filed` counter's own booking rides the drained requests at the
  Campaign (`run_tasking_cycle_atm_`) — the ATM's const ledger pointer
  stays read-only, the one-ledger-writer discipline holds; the counter
  (at filing) and the log (at drain) cover the same set. (6) The
  route predicates gained the sweep-line and objective-CAS families
  under the strategy arm (`profile_flies_sweep_line` /
  `profile_flies_objective_cas` — data-driven, never a byte switch);
  the RouteBuilder arms ride the same host config that arms the
  racetracks (session opts / QC flag), so disarmed hosts build
  byte-identical routes. (7) The objective CAS routes to the damaged
  FRIENDLY objective (the defenders are the point) — the builder
  resolves objectives first, the WP_CAS action byte rides unchanged.

### CAMP-INIT-1 — create-from-parameters
- Scenario pack JSON (theater + OOB template + force levels + date/weather +
  seed) → `CampaignInitializer` writes `.cam` via the Task-70 encoder
  (byte-identity by construction); small/medium/large generated wars become
  test fixtures; the G1 two-side war-pair limitation gets its test bed.
- **Gate**: a generated save decodes in the existing reader; the C5 24-hour
  harness passes on a generated save.
- **As-built (shipped)**: (1) The initializer lives importer-side
  (`f4-world-convert`, beside `campaign_saver` — the boundary keeps runtime
  targets clean of the binary parsers; the `campinit` CLI joins the importer
  side list). `ScenarioPack` parses STRICT (the wire protocol's own
  discipline: unknown keys and vocabulary words are named errors; every
  cross-reference — relation slots, squadron bases, battalion sites, link
  indices — validated at the parse). (2) Byte-identity by construction: the
  synthesis is a pure function of pack + class table; the pack's seed lands
  in the `.cmp`'s CreationRand — two builds are byte-identical (in-test AND
  as two independent `campinit` runs, same MD5), a different seed moves the
  bytes. (3) The world JSON is THE EXISTING READER's own projection: build →
  `CamArchive::load_from_memory` (new additive overload; `load(path)`
  delegates) → `to_world_json` — no second projection exists to drift; the
  external `campinit` → `cam2json` chain verified. (4) Fresh-save
  conventions: the passthrough-typed `.evt/.plt/.pst/.wth` ride EMPTY (no
  codec exists for them; nothing in this repo's reader or runtime reads
  them — a donor-copy mode can fill them later without touching the gates);
  the `.obd` is the canonical zero-delta 10-byte form; the maintenance
  anchors start at the opening clock (one full cadence period of grace);
  the camp map is the nearest-objective 2-bit ownership fill (inert for the
  runtime); all 8 team slots are emitted with the stock `XX` placeholder
  rows. (5) Every named team carries korea's own live mission/objtype
  priority profile (a zero row would mean a team that never requests
  anything), one ATM airbase row per squadron home base (32 free schedule
  blocks), and squadrons anchored with non-zero airbase VU ids — the
  primary resolution path (the entity-side positional fallback is dead on
  real data, world_loader.cpp:356-378). (6) Named kinds resolve to
  class-table entity types by first-match over the table's own range (the
  documented rule); the enum's `town` (39) has no entry in Korea's table,
  so packs use `city`. Squadrons in the committed packs pin
  `entity_type 473` (the session-proven F-16 class against the f16.json
  config). (7) Fixtures: small/medium/large/twinwars packs committed under
  `f4-world-convert/tests/fixtures/packs/` and generated at build time (the
  mission_profiles_fixture discipline) into `generated_world_fixtures/` —
  the harness worlds are the DECODE of the generated `.cam`. (8) The gates:
  12 initializer tests (byte identity, the seed, the container manifest,
  cursor-clean decodes ×4, the zero-delta `.obd`, the committed packs, the
  WorldState round-trip, the tasking profile); the C5 24-hour harness
  passes on the generated small war (24 samples × 2 runs, the MD5
  certificate equal); medium/large certify at compressed horizons;
  twinwars certifies with five named teams. (9) The G1 bed: the
  engine-level pin (`GroundWar.SecondWarPairStandsDownWhileTheFirstFights`)
  — the first wire-order pair fights, the second pair's mobile battalions
  never march and never sync to the ledger; the harness-level twinwars run
  certifies with the third sides in the world. The per-team ATM is
  deliberately NOT pair-gated (each team tasks against its own war rows —
  the C4 design); the two-side machine is the C6 air picture + G1 ground
  war (`belligerent_pair`).

### CAMP-SCALE-1 — Tier-3 full data + the scale certificate
- Full UCD/PLT_PARK conversion (the queue item four plans share); the
  strategy war over the uncapped fleet.
- **Gate**: `campaign_qc --accel` certificate on the full fleet (exits
  15/16 stand); tiered sustained rate meets the plan's target; VCD
  countermeasure counts and pilot data flow from the converted tables.
- **As-built (LANDED)**: the conversion is a surface + the flows behind
  it, so the real 296-row export is DATA, not a second code pass. The
  producer: `emit_tables_json` (f4-world-convert) + `cam2json
  --emit-tables` write the complete UCD/VCD/WCD tables as one
  `f4.theater.tables/1` JSON document (every record, every field — the
  round-trip test scales the 8-row real fixture to a 296-row table and
  pins the shape). The consumer: f4-world's `TheaterTables` loads the
  same document (the F4_SIDE boundary stays JSON-only). Two flows land:
  (1) the VCD countermeasure counts — the class-table → VCD → WCD chain
  (`resolve_countermeasures`; the WCD's name IS the dispenser identity,
  case-insensitive; the WCD has no type enum) stamps
  `CountermeasureSupplyComponent` at spawn and the arm path spends the
  counts instead of the documented 30/15 — SENSORS_COUNTERMEASURES
  PLAN's named Tier-3 item; (2) the pilot-skill flow — gated
  (`pilot_skill_flow` / `--pilot-skill`, the countermeasures gate's own
  lesson: data that re-prices fights lands behind a switch) — the
  squadron's converted pilot roster picks its best available pilot
  (highest skill nibble, then rating, then lowest id) and sets the
  spawned brain's SensorFusion cadence (0-2 Recruit / 3-5 Rookie / 6-7
  Veteran / 8-9 Ace — documented monotone map; no roster = the Veteran
  default). PLT_PARK closed as a test: the PD walk is type-agnostic
  (B1's question), so theaters carrying parking lists flow to the
  ground layouts and the bridge prefers them (Korea's PD carries none —
  the parking there stays synthetic, unchanged). The uncapped fleet:
  `campaign_qc --max-flights 0` now means UNCAPPED (the war mode's
  48-flight default kept when the flag is absent); `--theater-tables` +
  `--pilot-skill` arm the flows in the certificate. Absent tables or
  rosters leave every pre-SCALE run byte-identical.

### CAMP-DOM-1 — victory scoring (the books' projection)
- Upstream tracks victory points in the campaign; F4 has books but no
  verdict. Victory events + a `verdict` query; any UX can say something
  about the war.
- **Gate (as built)**: a `verdict` query answers with the per-team
  census + ledger rows byte-stably (goldens); when the front moves (a
  capture in the stream) a `verdict` event follows, and the query
  answers with the SAME coarse state that event carried; a war at rest
  publishes nothing. Landed on the generated small war's live front
  (the C5 bed), the quiet HostRig's exact bytes, and the pure model's
  10 pins.
- **As-built notes**:
  (1) The verdict is a READ-ONLY projection — no new engine truth, no
  accumulation, no save-format change: the territorial half walks the
  LIVE owner of every objective (the ground war's mirror when one runs
  — the engine's live truth; the WorldState otherwise) against the
  session's OPENING owner (snapshotted at construction, step 5b — the
  same run scope the ledger's books keep), weighted by each objective's
  own priority byte; the attrition half is the ledger's own team rows
  (captures, air losses, ground losses, battalions destroyed, the
  aircraft pool's existence view), reported not folded. (2) The band is
  territorial ONLY: stalemate / advantage / decisive — the leader's net
  gained priority ≥ `kDecisiveSwing` (100, one top-priority objective's
  worth) is decisive; a tie for the lead is no lead; books alone never
  move the band, so a capture is the only mover today and the event is
  as sparse as the front moving. (3) The `verdict` event is the
  coarse-state CHANGE signal (band, leader slot, leader swing — the
  full rows stay on the query); the pump diffs every whole-second
  (advance()'s block, after the damage events), starts
  stalemate/no-lead, and never publishes a synthetic opening event.
  (4) The DTO (`VerdictView` at dto.hpp's tail — the threat precedent)
  carries `t` on the engine's RELATIVE axis (the events'; the books it
  sums are run-scoped) and the band word is the producer's (`band_name`
  — an unset DTO rides empty, the DTO never invents vocabulary). The
  `.cmp` header's `te_victory_points` rides as `threshold` context (0 =
  the save sets none; no terminal semantics claimed — korea carries 0).
  (5) The census: ledger team rows ∪ belligerent slots ∪
  territory-holding slots (slot 0 = the neutral control value, never a
  row of its own; a named slot-0 team keeps its books row), sorted by
  slot. (6) The pure model lives in f4-campaign (`war_verdict.hpp/.cpp`,
  `compute_theater_verdict` over `VerdictObjective` arrays + the
  ledger) — no WorldState detail dependency; the session builds the
  live view from the mirror-or-WorldState and the host maps it onto the
  DTO (the stats() pattern). (7) Known seam, documented not fixed: the
  `objectives` query serves the WorldState's owner rows (the write-back
  lands at save), while the verdict reads the live mirror — the two
  agree at save boundaries; a live objectives view is a later tranche's
  seam (DOM-2 touches the same nerve). (8) `campaign_qc` and
  `campaignd` are untouched (QC drives the engine directly; the host's
  unknown-query exit 21 and exit namespaces stand); the protocol
  whitelist gains `verdict` additively, `kProtocolVersion` stays 1.

### CAMP-DOM-2 — supply depth (the per-objective pool)

AS BUILT (the C2 known-gap's ground face + the repair tranche in one
series). The chain: the team's strategic stock (`.tea` TeamClass
`supply_avail`/`fuel_avail` u16 @52/@54 — decoded and emitted by
world_json all along, parsed into `WorldState::TeamState` and exposed
on `ITeamSource` since DOM-2) regenerates its held objectives' stocks
each resupply fire, battalions DRAW from the nearest own-held
objective within the line-of-supply radius, and the `last_repair`
cadence (the third `.cmp` maintenance timer — bridged since C2,
consumed since DOM-2) spends that stock healing features. Notes:

(1) Every knob defaults OFF — `GroundWarConfig::objective_supply`,
`repair_period_sec` (plus the rates: `supply_radius_grid` 10,
`supply_regen_per_fire` 10, `repair_min_supply` 25 — the movement
gate's own threshold echo —, `repair_features_per_fire` 1,
`repair_supply_cost` 5), `CampaignConfig::replacement_stock_flow`,
the session's `ground_objective_supply`/`ground_repair_sec`/
`replacement_stock_flow`, the QC's `--objective-supply`/
`--repair-period`/`--replacement-stock`. The G1 flat refill is the
byte-identical golden path; the F6 gate lesson held a third time.
(2) The seed CLAMPS to the wire's own 0..100 domain — real saves
carry 0xEB uninitialized garbage where the original game never wrote
the fields (kunsan: 235), and a negative `last_repair` clamps to 0.
(3) The fire order inside one resupply tick: REGEN (teams in slot
order, their held objectives in wire order, each take =
min(rate, pool remainder, 100 − stock); fuel mirrors on the fuel
pool — carried, no consumer draws it yet, the wire's battalions
carry no fuel byte), then DRAW (battalions in wire order, nearest
own-held depot by squared grid distance, wire-order ties; take =
min(25 — the flat flow's own amount —, depot stock, headroom); a
battalion beyond every own objective's radius is CUT OFF: nothing,
counted in `cut_off_events` — the encirclement effect the
reference's supply-line targeting exists to create). Fatigue/morale
recover flat in both paths (rest, not logistics).
(4) Repair: anchored on `last_repair`, catch-up-once, the same shape
as resupply/reinforcement. Each fire ADOPTS the ledger's
damage-state faces into the mirror first (wholesale, idempotent —
the records ARE final states), then per belligerent-held objective
with stock ≥ `repair_min_supply`: flips up to
`repair_features_per_fire` lowest-index damaged/destroyed features
to VIS_REPAIRED (1 — the honest state: not rubble, not pristine),
spends `repair_supply_cost` per feature, stamps the objective's own
`last_repair` wire field.
(5) The books: `apply_objective_repair` appends to the repair log
(the `objective_repaired` family's source — the eleventh, all eight
touch-points, the owner-side team gate) AND upserts the damage-state
map with the post-repair face — so the C1 fstatus write-back carries
repairs with zero new machinery. The ground write-back gains the
logistics face: `logistics_dirty` objectives (regen/draw/repair
moved them) write supply/fuel/last_repair; the flag is what keeps a
seeded-but-unmoved mirror from normalizing the save's garbage bytes.
The strategic reserve write-back: spent reserves land in
`teams.replacements_avail` (activity = spent > 0).
(6) The air side's known-gap closure: `apply_reinforcements(t,
stock_flow)` — OFF (default) keeps the C2 shape (budgets consumed,
never replenished); ON refills consumed budgets toward their wire
snapshot out of `replacements_avail` after the delivery pass, slot
order then wire order, the reserve draining as it gives. The reserve
is the war's ultimate aircraft source; the budgets are the squadrons'
order books it keeps full. `TeamLedger` carries the reserve's books
(initial/avail/spent — the artifact's team rows gain the keys only
when the flow moved anything), `SquadronLedger` gains
`reinforce_initial` (the refill target).
(7) THE SEAM CLOSES (DOM-1's as-built note 7): the `objectives`
query overlays the engine's live mirror (owner + supply/fuel/losses/
last_repair/fstatus by VU) when a ground war runs — the kunsan
garbage bytes are the test signal (235 in the quiet query, 100 in
the live one); no ground war → the rows stay the WorldState's own,
byte-identical to the pre-DOM-2 shape. The sink's damaged count
stops calling VIS_REPAIRED damage (a healed feature is not damage —
the repair mirror writes state 1 into the entity face, restored hp
included, so the next damage sync diffs truth).
(8) `campaignd` untouched (no new query; the family rides the
subscribe kinds — `parse_event_kind`'s list, `kProtocolVersion`
stays 1). The QC summary's ground block gains the supply books only
when the flow moved anything (regen/drawn/cut-off/repairs/
features_repaired), the certificate line unchanged.
(9) The certificate: campinit medium war, 0.5 h, the three arms on —
regen 12 / drawn 12 / cut-off 5 (the army marched past its depots),
no repairs (nothing broke in 30 minutes — honest zero), two runs one
MD5 (2e0180b3…), exits stand. The session gates prove the repair
chain end to end: seeded damage (feature 0 destroyed on every
belligerent-held kunsan objective) heals on the cadence, the events
publish one-for-one with the books, the entity face joins
(VIS_REPAIRED + hp restored), both write-backs land.

### CAMP-DOM-3 — personnel (the reference's AssignPilots, the rotation pressure)

AS BUILT (PLT_PARK closed in SCALE-1, the rosters flow — the tranche
unblocked). The chain: every filed flight draws its CREW from the
squadron's decoded pilot roster (the wire's own 48-slot array — the
scan axis the reference itself uses), the crew rides the MissionIntent
and the personnel books, losses consume slots, recoveries credit
sorties, and the write-back applies the run's deltas to the wire
statuses. Notes:

(1) Every knob defaults OFF — `AtmConfig::pilot_assignment`/
`rating_decay`, `CampaignConfig::pilot_assignment`/`rating_decay` (the
ATM inherits both at construction), the session's `pilot_assignment`/
`rating_decay`, the QC's `--pilot-assignment`/`--rating-decay`. The
rosters are ignored beyond the SCALE-1 skill map — the golden
identity, the F6 gate lesson's fourth holding. (2) THE PICK
(AssignPilots' deterministic subset): the lead scans the roster's FRONT
THIRD for the first free slot (the commanders' seats), the wingmen
scan BACKWARD from the tail; fewer free pilots than ships → empty crew
= the flight fails. As a PICK-TIME GATE in FindBestAir (counted in
`crew_denials` when every other gate passed): a squadron that cannot
crew never enters the comparison — the scored walk falls to the
runner-up, the same squadrons fly. The ATM tracks its own pick-time
OUT-SET (`crew_out_`) so two same-squadron flights in one cycle never
double-pick (the ledger books at PUBLISH, after compose built the
whole cycle). Free = status 0, not ledger-dead, not out. (3) THE
DECAY: the squadron's per-role effectiveness table (the `.uni` tail's
rating[ARO_OTHER=16], typed through the world pass — emitted only when
non-zero, presence = data) seeds the live view, else the UCD Scores,
else nothing (the static specialty fallback never decays — the
fixture's own artifact). new = (int)(0.75 × rating) + 1 — integer
truncating, the +1 floors the fixed point at 4 (a rating never decays
to zero; 1..3 are already fixed). The decayed view rides the flight
(`ratings_valid`) and the Campaign syncs the ledger (its write domain
— last-write-wins per squadron, `ratings_fires` the activity);
`rating_()` reads the LIVE view first, so FindBestAir's base score
spreads the sorties across the wing (the rotation pressure the
reference's own docs name). (4) THE BOOKS: `apply_mission_draw`'s
6-arg shape books the assignment log (the `pilot_assigned` family's
source), the flight→crew map, and per-slot OUT deltas; `apply_air_loss`
consumes ONE slot per crewed-flight loss IN PICK ORDER (the lead dies
first — the deterministic subset; the reference tracks per-aircraft
pilots in the flight tail the aggregate books do not carry; dead stays
dead); `apply_mission_recovery` releases the crew, credits each
survivor one sortie (the `pilot_recovered` family's `missions_run` is
the slot's THIS-RUN count — the write-back ADDS to the wire's
history, never replaces). An empty crew books exactly the pre-DOM-3
4-arg shape. (5) THE EVENTS: the pilot trio — `pilot_assigned`/
`pilot_lost`/`pilot_recovered` (the twelfth through fourteenth, all
eight touch-points, the flying team's gate) — published from the
ledger logs' tails by the session's `emit_pilot_events_` (three
independent cursors: the logs grow from three engines). (6) THE
QUERY: `squadrons` joins the whitelist additively (protocol +
`engine_serves_query`, `kProtocolVersion` stays 1): one
`SquadronView` row per squadron in WIRE order — the wire's identity +
counts with the ledger's deltas applied (available = the C2 tasking
pool; pilots_available = wire-available minus dead minus still-out;
ratings = the live table when the decay fired, else the wire's own).
The DTO rule holds: additive, at the END, always present.
(7) THE WRITE-BACK: the personnel face in `apply_to` — dead slots →
status 1 (idempotent), per-pilot `missions_flown` += the run's
credited sorties (clamped i16), the decayed table when
`ratings_fires > 0`; ACTIVITY = losses/sorties/fires — a draws-only
run (outs only) writes NOTHING (the out is transient, the save
carries no phantom states), counted in the new `personnel_written`.
(8) THE BRIDGE SEAM: a crewed flight's LEAD sets the spawned brain's
SensorFusion cadence (`intent.crew[0]` indexes the squadron entity's
own roster) when the SCALE-1 pilot-skill flow is armed — the
whole point of the front-third scan; the wing-wide best-scan only
stands for un-crewed intents. (9) The personnel books ride the
ledger's result document (the `books` query's payload): the totals
answer always (`pilot_assignments`/`pilot_losses`/`pilot_sorties` —
the honest 0 is the arms-off answer), the three logs only when one
exists, the squadron rows carry `run_pilot_losses`/
`run_pilot_sorties`/the decayed `role_ratings` only when the run
moved the roster (the personnel activity counts as THIS-RUN
activity). The WorldState's own save face emits `role_ratings`
presence-based (the write-back's decayed table reaches the saved
world; every pre-DOM-3 state emits byte-identically). (10) The QC
prints `personnel: crews=… denials=… decayed=…` when either arm is
on (both the session and the tasking modes wire the arms);
campaignd untouched (the families ride the subscribe kinds, the
query rides the whitelist). The certificate: campinit medium, 0.5 h,
both arms on — 95 intents → 95 crews (denials 0, the pack carries no
rating tables → decayed 0, the honest zero), deterministic=yes,
two runs one MD5, exits stand; the kunsan tasking QC (8 cycles):
crews=32 denials=57 (the save's non-rostered squadrons deny — the
reference's flight-fails rule as a pick-time gate) decayed=0.

### CAMP-DOM-4 — airbase scheduling (FindTakeoffSlot depth beyond FID's airfield-ops windows)

SHIPPED with this patch. The contract: `FindTakeoffSlot()` depth
beyond FID's airfield-ops windows. The slot grid existed (C4 phase 7:
the 32-block bitmask, the exact/+1/+2/−10 snap, the fudge-block fill,
the booking commit) but it was RUNTIME-INVISIBLE and RUNTIME-MORTAL:
block 0 was pinned to the campaign's start (anything past the
160-minute horizon silently unscheduled — the `-1` path kept the
estimate and told nobody), the pick gate checked one block and skipped
without a counter (the reference checked the block AND the previous
one — a documented deviation), a scrubbed flight held its slot
forever, the filled grid never reached any book, and the sim re-derived
its own takeoff gate (`TOT − 2×ops_window`) because the intent carried
only the post-snap TOT — the two clocks never met (FIDELITY_TIERS §7's
"up to ~2× ops_window after its TOT" delivery-latency note).

(1) THE SLIDE: `AirbaseSchedule::sync(now_min, plan_block_min)` —
the grid's anchor (`epoch_min`, the campaign-minute block 0 maps to)
advances in whole blocks so `now` sits in block 0; past bits fall off
with their time (the seeded wire bits included — those sorties are
flown); a clock jump past the whole grid clears it. The gate syncs to
the cycle's clock (which makes the gate's now-relative block index the
grid's own), phase 7 syncs to the filing's takeoff minute (the epoch
never slides past the request — the backward window stays honest).
Disarmed: epoch stays 0, the campaign-start alignment is the golden
identity. The reference's own scheduleTime anchor was runtime state —
so is this one: the grid is NOT written back to the save (the wire's
32-byte `schedule` seeds the NEXT session at ITS start; a run's
bookings are this-run truth — the same position upstream took).
(2) THE GATE: armed, FindBestAir applies the reference's own rule —
the start block OR the previous one full denies the base (a base
still launching the previous block's queue cannot take this flight) —
and the skip is COUNTED (`AtmStats::schedule_denials`, the
per-schedule `denied()` book) and QUEUED for the ledger. Disarmed:
the single-block skip stays the pre-DOM-4 shape, silently.
(3) THE OVERFLOW: phase 7's `-1` path (the grid saturated within the
±13-minute search window) counts (`slot_overflows`, the per-schedule
`overflowed()` book) and queues — the saturated grid is a BOOKS FACT,
not a silent one. The flight keeps its estimate and still flies (the
reference cancels at 0xFFFFFFFF; the documented deviation stands).
Far-TOT requests whose block sits past the horizon bypass the gate
(the same `block < max_cycles` bound as before) and surface at phase 7
— the slide keeps them IN the grid when armed.
(4) THE RELEASE: a scrubbed flight's STILL-FUTURE slot goes back
(`release()` is fill's exact inverse — the fudge block and the
large-flight minute unmark too); a past slot no-ops (clearing it could
only invite a backward snap into a departed minute). `slot_releases`
counts. Recovery releases nothing (the takeoff minute is past by
then — the slide owns it); the retask keeps its slot (historical, as
documented since CAMP-CMD-2).
(5) THE BOOKS: `CampaignResultLedger::apply_slot_denial(t, team,
airbase, reason)` — the slot-denial log (arrival order = engine
order), the `slot_denied` family's source; the artifact's totals gain
`slot_denials` (always — the honest 0 is the arms-off answer), the
log array only when one exists. The ATM queues (`SlotDenial{team,
airbase_vu, reason}`; reason 0 = the pick gate, 1 = the horizon — the
`SlotDenialReason` vocabulary); the Campaign drains after the cycle
and stamps the clock (the ATM's ledger pointer is read-only — the
ACTION-filing split). Nothing queues disarmed; a disarmed run's
artifact stays byte-identical.
(6) THE EVENT: `slot_denied` — the FIFTEENTH family, all eight
touch-points (struct, Kind, member, name, parse, encode, the envelope
dispatch, the team gate: the DENIED side — the flight that could not
launch is the request's own). `kProtocolVersion` stays 1; campaignd
untouched (the family rides the subscribe kinds).
(7) THE QUERY: `airfields` joins the whitelist additively (protocol +
`engine_serves_query`): one `AirfieldView` row per airbase the ATM
holds a book for, in WIRE order — the 32-block grid as 64 lowercase
hex chars, `epoch_min` (the anchor; 0 = the campaign-start
alignment), `booked` (the set bits), the base's own denial books
(`denied`/`overflowed`). Teamless rows (the grid is the BASE's truth
— every side's flights deconflict against it); the pipeline off = an
empty set (the honest answer, never a stub).
(8) THE SEAM: `MissionIntent` gains `takeoff` (the phase-7 snap's
output, campaign-relative; 0 = never slotted — the legacy ladder, the
save's own flights, a base-less filing; the DTO's `IntentView` gains
the additive tail key). The session's airfield-ops gate arms against
the SLOT when the arm is on: the aggregate's head departure IS the
scheduled minute (`depart = takeoff_abs`, clamped forward) — the
flight materializes one ops window before its slot and rolls ON it;
the TOT-anchored gate stays for slotless flights and disarmed
sessions. The FIDELITY_TIERS §7 divergence closes from the campaign
side without moving the FID machinery.
(9) THE SUMMARY: the ATM block gains `schedule_denials` /
`slot_overflows` / `slot_releases` — only when the arm is on (the
disarmed block stays byte-identical; the pre-DOM-4 overflow path
stayed silent, so a 0 would lie about the shape, not the count). The
QC gains `--airbase-scheduling` (both the session and the tasking
modes wire it), the `scheduling:` echo line, and the
`scheduling: denials=… overflow=… releases=…` counter line when
armed.
(10) THE CERTIFICATE: the C5 24-hour gate green with the arm
compiled in and OFF (the MD5 identity stands — the knob is the
opt-in); the Campaign-level scheduling gate (the saturated USA base
denies into the books, the disarmed same-world runs silent, the
armed run deterministic); the session gate (the slots ride the
intents, the gate arms on them, the airfields query serves the live
grids — kunsan's own ATM base rows slide and book, the overflow
books name the saturation honestly — the event/books parity, the
armed run's determinism).

### CAMP-DOM-5 — naval (the wrap-then-decide)

SHIPPED with this patch. The upstream truth drove the shape:
the NavalTaskingManagerClass is a 15-byte flag shell on the wire
(the .tea's NTM record — the world pass captures it verbatim), and
the reference's actual naval tasking IS the air ATM filing
anti-ship missions at naval targets ("for each carrier: generates
AMIS_ASHIP"; naval-target requests convert to PATROL). So the wrap
maps the naval face onto the ATM pipeline's REQUEST VOCABULARY
instead of building a sibling manager, and the DECIDE half of the
tranche's contract shrinks to the "how deep" record below. Notes:

(1) Every knob defaults OFF — `AtmConfig::naval_tasking`,
`CampaignConfig::naval_tasking` (the ATM inherits it at
construction), the session's `naval_tasking`, the QC's
`--naval-tasking` (both the session/war and the tasking modes wire
it). The golden identity holds a fifth time: disarmed, the naval
ranking walk never runs, the anti-ship requests stay target-less,
the summary carries no naval keys, and every pinned test is
unchanged. (2) THE POOL (`rank_taskforce_targets` in the new
naval_tasking.{hpp,cpp} — the tranche's home, the upstream NTM's
name): the task forces of teams at WAR with the requester (the
symmetric belligerence rule), sea domain 4, the aggregate
TaskForce class, non-empty roster, a real VU — ranked by squared
distance to the requester's nearest OWN-HELD objective ascending
(the fleet off your coast is the fleet you strike first), wire
order breaking ties through the sort key (the
rank_battalion_targets pattern), no own holdings = the honest
INT64_MAX tie (wire order decides). No ledger filter: the
ledger's ground-unit books are battalion-worded and the engine
holds no naval-loss source — a spent task force is a later
tranche's book. Deterministic (squared distance, no sqrt, no RNG).
(3) THE FAMILY SPLIT — the pool's decision is
`mission_is_naval_strike` (mission_type.hpp: the name table is the
vocabulary, kMissionAship's position pinned by static_assert):
AMIS_ASHIP only, today. ASW shares the naval face but hunts
SUBMARINES the wire does not carry (its pool is the honest empty
set); TANK shares the strike SHAPE but hunts armor (the ground
pool's business) — both stay target-less even armed. The SHAPE is
`profile_flies_naval_strike_route` (route_builder.hpp: UNIT +
TPROF_ATTACK + WP_STRIKE — ASW and TANK match the shape and never
get targets; the shape only decides whether a targeted filing
routes like a strike). (4) THE REQUESTS: generate_requests
computes the naval pool per team when armed (the ranking walk
never runs disarmed) and the anti-ship requests rotate across it
through their OWN cursor (`naval_target_cursor_` — the families'
spreads stay decoupled, the house pattern); `AtmStats::
naval_requests` counts the requests GIVEN a task force. (5) THE
UNIT-RESOLUTION SEAM: resolve_target_xy's `allow_units` term is
now the OR of the two arms (`Atm::allow_unit_targets()`) — a
task-force target resolves through the units source exactly the
CAS battalion did; both arms off = the pre-G2 walk byte-identical.
(6) THE ROUTES: the Campaign's compose route condition gains the
naval shape under the arm (the builder resolves the task force's
grid position through `unit_xy_` — any unit VU, no battalion
filter needed); the legacy ladder gains the matching naval rung
(`select_naval_target_`, its own cursor — the two ladders never
share). (7) THE BOOKS: one book, one owner — the ATM's
`book_naval_filing(vu)` at compose (main flights only; escorts
never book) increments `AtmStats::naval_filings` AND the per-
target map (`naval_filings_`, VU-ascending — the query's face);
the Campaign exposes it via `atm_naval_filings()` (null when the
pipeline is off). DELIBERATE: no new ledger log — the filings
already book through the squadron draws (apply_mission_draw), the
events already stream through mission_filed, and a naval log would
double-book the same fact; the honest surface is the stats
counter + the per-target books. (8) THE EVENTS: NO new family —
the anti-ship packages publish on the same `mission_filed` event
every other package rides, the task force's VU riding the target
id (the same field the G2 CAS unit targets ride — the documented
naming compromise). `kProtocolVersion` stays 1; campaignd is
untouched (the query rides the whitelist). (9) THE QUERY:
`taskforces` joins the whitelist additively (protocol +
`engine_serves_query`) — one `TaskForceView` row per task force
in the WORLD's own wire state (domain-4 units, wire order),
present with or without the arm (the objectives row's rule: the
wire owns the facts), overlaid with this run's filing books (the
honest 0 disarmed): the VU pair, the owner slot, the sea subtype
+ the class table's name (`unit_subtype_name`), position, the
wire's own dest_x/dest_y (unconsumed — note 10), the supply byte,
`filings`. spec.team filters on the owner slot. (10) THE "HOW
DEEP" RECORD (the tranche's decide-half): the wrap makes the
naval targets REAL to the tasking pipeline — it does not make
them MOVE. Task-force movement (the wire's dest_x/dest_y is
decoded but never consumed — a naval GroundWar sibling), naval
threat-map painting (the MoveType Naval=6 arrays exist on the
wire; the map paints land AD only), carrier airbases (the
reference's naval airbase scoring), task groups / CVN ops, and a
naval-loss book all stay OUT — each is its own tranche with the
same opt-in contract. The NTM's 15 wire bytes stay captured
verbatim, untouched. (11) THE SUMMARY: the Campaign's ATM block
gains `naval_requests`/`naval_filings` only when the arm is on
(the disarmed block stays byte-identical — the pre-DOM-5
anti-ship path stayed target-less, so a 0 would lie about the
shape, not the count). The QC prints the `naval: requests=…
filings=…` counter line when armed (the tasking mode; the war
mode echoes the arm on its config line). (12) THE CERTIFICATE:
the medium-war 24-hour-class gate with the arm compiled in and ON
(deterministic=yes, two runs one MD5, the honest-zero pools —
campinit packs generate no task forces); the kunsan gates (its 2
task forces are the raw material): the ranker pins (hostility,
wire-order ties, the skips), the family split, the ATM arm
(targeted ASHIP, target-less ASW/TANK, the disarmed and
empty-pool corners), the Campaign books (filings sum
one-for-one, the based-squadron route pin), and the session gates
(the query serves the wire's rows + the books, the filings ride
mission_filed one-for-one, the arm-off golden identity, the
two-run query determinism).

### CAMP-DOM-* — domain tranches (each its own landed series, upstream-mapped)
- ~~**DOM-2 supply depth**~~ — SHIPPED above.
- ~~**DOM-3 personnel**~~ — SHIPPED above.
- ~~**DOM-4 airbase scheduling**~~ — SHIPPED above.
- ~~**DOM-5 naval**~~ — SHIPPED above (the wrap; the "how deep"
  record in the as-built notes names what a deeper naval tranche
  would take).

## 9. What does NOT change

- The `.cam` wire format, the encoder's byte-identity, the save round-trip.
- f4-campaign's internal engines (ledger, tasking, GroundWar, RouteBuilder).
- The FID machinery — the policy struct is its existing knobs, renamed.
- The CMake boundary property scheme; `f4-campaign-api` joins the neutral set.
- f4-flight-api, the recorders, and every existing harness. `campaign_qc`
  keeps its exits; `campaignd` adds a new namespace (20+).
- No UI work, no networking, no real-time pacing in the engine (§2.2).

## 10. Relationship to existing plans

| Plan | Handoff |
|---|---|
| `ATM_STRATEGY_PLAN.md` | its queue's ACTION tables + RoE doctrine + GetPriority terms land as CAMP-ATM-1 / CAMP-CMD-*; the plan keeps domain ownership, this doc owns the surface they ride |
| `FIDELITY_TIERS_PLAN.md` | unchanged; §4 here is its contract hat |
| `CAMPAIGN_LOOP_PLAN.md` | unchanged; the ledger/diary generalize into events (HOST-2) without touching the books |
| `SAVE_WRITE_PLAN.md` | unchanged; `campaignd --save` and CAMP-INIT-1 are its consumers |
| `NO_BINARY_RUNTIME_PLAN.md` / Tier-3 | CAMP-SCALE-1 is the same conversion pass, named once here for sequencing |
| `AI_IMPLEMENTATION_PLAN.md` | NavigationModule replanning serves CAMP-CMD-2's retask semantics |

## 11. Risks & mitigations

| Risk | Mitigation |
|---|---|
| DTO/JSON drift (two truths for one payload) | single source of truth is the C++ struct; `from_json` round-trip tests + byte-stable goldens on every DTO |
| Event flooding at 60× accel | journal is engine-rate (complete); wire delivery is per-`step` and subscription-filtered; coalescing rules ride FID-OPT's staleness discipline |
| Command-timing ambiguity breaking replay | apply-at-next-tick-boundary, submission order, journal records the resolved apply-tick; exit 23 guards replay identity |
| HOST-3 refactor destabilizes the viewer | parity gate on existing viewer tests; the refactor lands behind the parity run, deletions reported |
| Scope creep into UX building | §12; the reference host renders nothing, the scripted client is a test, not a product |
| The contract ossifying early | `protocol_version` in the handshake; DTO versioning is additive-field-only until a UX exists to break it deliberately |

## 12. Non-goals

- No user experience building of any kind — no map UI, no panels, no UX
  polish. Clients other than the reference host and the refactored viewer
  are out of scope.
- No networking in the engine; stdio only; sockets are host-side bridges.
- No multiplayer, no real-time pacing, no sound, no rendering hooks beyond
  the focus command.
- No `.cam` format changes; no engine-internal redesign to fit the contract.

## 13. Paperwork

- Docs/README.md index row (Active plans):
  `| Campaign host (the engine contract) | CAMP_HOST_PLAN.md | Draft v1 — HOST-1 not started. |`
- `CHANGELOG.md` line on landing (one line, per convention):
  `CAMP-HOST-1 — the engine contract + campaignd: f4-campaign-api (session iface, v1 query DTOs), the CampaignSession adapter, stdio host, scripted golden client; golden identity holds.`
- Task IDs are namespaced `CAMP-*` and unique (the §Conventions rule).
