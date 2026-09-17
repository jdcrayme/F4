# Campaign Host — the Engine Contract (CAMP)

> **Status**: Draft v1 — CAMP-HOST-1 shipped (f4-campaign-api + the
> EngineSessionHost adapter + `campaignd`); CAMP-HOST-2 shipped (the
> typed event stream over f4-messaging, the JSONL journal, the replay
> identity); CAMP-HOST-3 shipped (the world viewer refactored onto the
> contract — the runner relocated out of the engine, the `threat`
> query landed additively, the golden identity intact); CAMP-CMD-1
> ships with this patch (`roe_set` riding the P7 fire-control path, the
> command journal + its tick-exact replay, the `roe_changed` event
> publishing — 30+ new ctest cases, the golden identity intact). Every
> other tranche below is an acceptance contract, not a claim.

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
| `mission_filed` / `mission_shared` | C4 tasking + P7 FindSupportFlights (`supports_filed`/`supports_shared`) |
| `flight_launched` / `flight_recovered` | spawn path + FID airfield-ops windows |
| `engagement_opened` / `kill` / `loss` | the combat passes + ledger books (C6) |
| `objective_damage` / `objective_captured` | fstatus diff (C1) + GroundWar (G1) |
| `reinforcement_delivered` | C2's reinforcement fire |
| `tasking_cycle` | the 7-phase ATM pass + `next_tasking_sec` |
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

### CAMP-CMD-2 — retask / abort / priority
- `flight_retask` (NavigationModule replans from current position, TOT
  recomputed), `flight_abort` (RTB profile, package books close),
  `objective_priority` feeding the tasking score; `GetPriority`'s
  PO/package terms (the ATM queue item) become policy inputs.
- **Gate**: pinned retask scenarios in the M4/M5 acceptance-harness style
  (a BVR flight retasked to CAS mid-crank flies the new route and its
  books close correctly).

### CAMP-ATM-1 — the ACTION tables (the ATM plan's named queue, now observable)
- Objective-damage-driven contextual CAS/BARCAP/SEAD filings; SWEEP station
  lines; tanker waypoints.
- **Gate**: the ATM plan's TestCamp QC (`--strategy`) plus `action_filed`
  counters in the ledger; the filings visible as events (any UX sees the
  war react).

### CAMP-INIT-1 — create-from-parameters
- Scenario pack JSON (theater + OOB template + force levels + date/weather +
  seed) → `CampaignInitializer` writes `.cam` via the Task-70 encoder
  (byte-identity by construction); small/medium/large generated wars become
  test fixtures; the G1 two-side war-pair limitation gets its test bed.
- **Gate**: a generated save decodes in the existing reader; the C5 24-hour
  harness passes on a generated save.

### CAMP-SCALE-1 — Tier-3 full data + the scale certificate
- Full UCD/PLT_PARK conversion (the queue item four plans share); the
  strategy war over the uncapped fleet.
- **Gate**: `campaign_qc --accel` certificate on the full fleet (exits
  15/16 stand); tiered sustained rate meets the plan's target; VCD
  countermeasure counts and pilot data flow from the converted tables.

### CAMP-DOM-* — domain tranches (each its own landed series, upstream-mapped)
- **DOM-1 victory scoring**: upstream tracks victory points in the campaign;
  F4 has books but no verdict. Victory events + a `verdict` query; any UX
  can say something about the war.
- **DOM-2 supply depth**: upstream interdiction targets supply/fuel lines
  and objectives carry supply; deepen C2's pool into per-objective supply
  feeding reinforcement and repair rates.
- **DOM-3 personnel**: `AssignPilots()`, rating decay, squadron rotation —
  blocked on PLT_PARK (CAMP-SCALE-1).
- **DOM-4 airbase scheduling**: `FindTakeoffSlot()` depth beyond FID's
  airfield-ops windows.
- **DOM-5 naval**: upstream HAS a naval tasking manager — it is very
  minimal, so this is a WRAP-then-DECIDE, not a from-scratch build: map
  the existing manager onto the ATM pipeline's request vocabulary first
  (its filings ride the same events), then decide whether TaskForce-level
  depth (task groups, CVN ops) is worth a tranche of its own. The
  decision record shrinks to "how deep", not "whether".

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
