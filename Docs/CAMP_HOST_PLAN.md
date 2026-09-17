# Campaign Host — the Engine Contract (CAMP)

> **Status**: Draft v1 — CAMP-HOST-1 ships with this patch (f4-campaign-api
> + the EngineSessionHost adapter + `campaignd`; 58 ctest cases green, the
> golden identity intact). Every other tranche below is an acceptance
> contract, not a claim.

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
| `threat` | threat map (C3) | per-cell / per-polygon threat values — **v1.1 (additive)** |
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
- The journal is append-only JSONL (`tick, CommandIntent`), written only
  when the host passes `--journal` — no file appears for golden runs.
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

### CAMP-HOST-2 — the event stream + journal
- Typed `CampaignEvent` over f4-messaging; the diary becomes a consumer, not
  the source; `--journal` JSONL writer.
- **Gate**: journal replay of the C6 acceptance fight reproduces the ledger
  MD5 (exit 23 guards drift); empty-journal saves byte-identical; one golden
  example per event family.

### CAMP-HOST-3 — the viewer becomes a client
- World-viewer campaign session refactored onto `CampaignSession`
  (in-process); direct engine reaching deleted.
- **Gate**: viewer feature parity (time controls, flights table, routes,
  campaign view) pinned by the existing viewer tests; the deleted-lines
  count is the proof.

### CAMP-CMD-1 — command journal + RoE doctrine
- `CommandIntent` wire + journal; `roe_set` per team/mission/flight riding
  the P7 fire-control path; RoE-driven threat walls (the 32000 overfly
  denial) as doctrine policy — the ATM plan's "RoE refinement tranche"
  lands here, command-shaped.
- **Gate**: post-arm fire-control behavior per P7 (1 = BVR suppressed,
  2 = everything held); replay-with-commands reproduces the books; no
  commands → byte-identical.

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
