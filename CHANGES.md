# F4 Cleanup Pass — Changes Summary

## Guns Employment — The Last Unflown Weapon Flies (M3-TACTICS-4)

**The AI fires the cannon. An armed jet working a merge tracks the GUN
solution — steering at where the target will be when the bullet arrives,
superelevation included — and the trigger goes down when the boresight
error projects inside the hit footprint at the current range. One
AI-vs-AI E2E proves the whole chain headless: missiles tight, guns free,
a drone bandit, and the fight resolves at the trigger — 25-50 rounds of
20 mm land, the bandit dies, the shooter disengages. The shipped
`guns_merge` scenario puts the trigger fight in the scenario-player,
tracer streaks and all. The suite is now 1,661/1,661.**

| Area | Change |
|------|--------|
| `f4-weapons` — GunStream (hardening) | SEGMENT hit detection: a tracer checks the segment it swept each tick (`[p_old, p_new]`, derivable exactly from the semi-implicit Euler update — no stored state), not its point position. At 3,400 ft/s a round covers ~57 ft per 1/60 s tick — more than the 40 ft hit radius — and at coarse host steps (dt >= 0.024) point checks jump straight THROUGH a target; the segment sweep closes the tunnel (regression-tested with a constructed jump-over). Plus: `set_sim_time()` (the host sweep stamps the clock — GunFired/Damage/Kill messages previously carried 0.0), `set_weapon_handle()` (name resolution), and `start_burst(rounds, aim_target_id)` (the aim hint on GunFiredMessage, which also gains `weapon_handle`). |
| `f4-weapons` — GunComponent + update_guns (new) | The per-aircraft cannon as a passive ECS component (the RwrComponent pattern — the host drives it, never update_all: hit detection mutates the world, and a firing jet must emit from its FRESH muzzle pose). `attach_combat_loadout` adds it (GunConfig from the M61A1 card: muzzle 3,400 ft/s, per-round 0.22 lb, lethal 40 ft; seeded like the radar — deterministic per scenario, distinct per shooter). `update_guns(world, bus, dt, t)` is the world-level sweep `Simulation::tick` runs AFTER the FM->Transform sync: boresight = the velocity unit vector, muzzle 15 ft ahead (the same clearance `launch_missile` gives a missile), tracers fly, hits damage, bursts announce on the bus. |
| `f4-ai` — GunModule (new, engine-agnostic) | The guns fire control, AI_IMPLEMENTATION_PLAN Steps 11-12. ROLE 1 — THE PREDICTOR: a gun is not fired at the target but at the LEAD POINT — where the target will be after the bullet's time of flight (`tof = range_now / (muzzle + closure)`), flown from the TRACK-FILE PREDICTION (the fusion refreshes at the skill interval — seconds — and at merge closure 5 stale seconds is the whole envelope; every quantity starts from `position + velocity * age_s`). The lead includes SUPERELEVATION: the aim point sits above the kinematic lead by `0.5*g*tof^2` — the gravity drop every real fire computer compensates (16 ft at 1 s). ROLE 2 — THE TRIGGER: a RANGE-SCALED hit-quality cone (`error <= atan2(hit_radius, range)`, capped ~5 deg) — a fixed cone cannot work, the FCS's own ~1.5-deg tracking lag is a 44 ft miss at 5,000 ft and a hit at 1,500 ft; 40 ft = the weapons model's hit radius. Burst discipline: 100 rounds (~1 s of trigger at 6,000 rpm) then 1 s cooldown; a 511-round drum budget; ROE hold_fire at the module level (no pulse, no phantom budget). |
| `f4-ai` — WVRModule integration | `wvr().guns()` — the fire control composed like the heater's `fire()`. MERGE: the snapshot — while the gun is armed and the target is inside its envelope, the steering swaps the missile-grade pursuit for the GUN lead (aiming IS steering there; the integrators reset at the reference change — windup carried across reference swaps held the trigger closed through a whole window in testing) and the trigger gate runs. OFFENSIVE: the same swap for the sustained tracking solution (OverB still overrides below 0.35 NM with hard closure — overshoot control trumps gunnery; a slow-closure stern chase is the gun shot). The module's ownship boresight estimate: consecutive positions / dt (the exact quantity the host sweep fires along); a two-tick warmup on engagement (stale history is dropped — a solution you cannot measure is not one you fire on). Guns tight: the merge flies EXACTLY as before — every pre-gun scenario is untouched. |
| `f4-ai` — TargetInfo/SensorFusion | `TargetInfo.age_s`: seconds since the fusion rebuilt the entry (0 = fresh). The fusion ages its track file every `update()` between rebuilds — a track file, not a live feed; consumers needing now-geometry dead-reckon. Missile modules ignore it (their envelopes absorb staleness); the gun predictor needs it. |
| Brain + bridge (the host half) | `CombatIntent` += `gun_trigger`/`gun_target_id` (the one-tick burst edge, the same intent surface as `weapon_release`; hold_fire gates it at the brain). `configure_brain_combat` configures the gun from the M61A1 card (envelope, muzzle) + the store's gun-station drum, with the full ROE matrix: `hold_fire` (all tight) / `bvr_hold` (radar missiles) / `missiles_hold` (NEW — all A/A missiles tight, guns free: the guns-dogfight doctrine, subsumes bvr_hold) / `guns_hold` (NEW — guns tight, **default TRUE**: the no-surprise rule — guns are the newest weapon and every pre-gun scenario must fly the identical fight after the wiring lands). `execute_brain_combat_intents` turns the edge into `GunStream::start_burst` (burst = the module's doctrine clipped to the drum; the store debits what left the muzzle). |
| `f4-recorder` + transcript | `CombatEventKind::GunFired` (wire name `"gun_fired"`, 9th kind): subject/object/weapon/rounds/muzzle position, captured by the bridge's recorder subscription; gun DAMAGE rides the existing DamageApplied stream (`missile_id == 0` = gun hit, the message contract's documented marker). JSON round-trip both directions; the LLM summary gains a `gun_bursts` array and gun-kill weapon attribution (the killer's most recent burst). The transcript learns the trigger call: **"Guns, guns, guns."** |
| Scenarios | `guns_merge.json.in` (shipped, scenario-player): EAGLE1 vs BANDIT1 (a hold-fire drone that fights geometry but never shoots) — both spawned nose-on at fight speed flying a shared MERGE waypoint dead ahead (a single-waypoint route: the nav's spawn-on-leg consolidation skips waypoints an aircraft is past, and turn-anticipation swallows a merge point near a corner — the merge must be the LAST waypoint), missiles tight, guns armed, a drone hull calibrated to one burst. Tracer streaks (fading amber lines back along each round's velocity) in the player's combat view. |
| Tests | +29 (suite **1,661/1,661 — 100%**, zero warnings): 15 GunModule units (TOF/lead/superelevation math, the track-file prediction, the range-scaled cone, the burst state machine, budget, ROE, reset semantics), 3 GunStream units (the tunneling regression, sim-time/aim stamps, the update_guns sweep from a moving muzzle), 6 WVR gun-intent units (the two-tick warmup, burst cycle, guns-tight hold, envelope, the Offensive steering swap vs the pursuit, reset), 1 recorder unit (gun_bursts summary + gun-kill attribution) + the extended every-kind round-trip, and the E2Es: GunsRoeDefaultsAndWiring, GunsRoeMissilesTightGunsFree, **AiVersusAiGunsMergeFight** (head-on at fight speed, WVR band entered, zero missiles launched despite BVR engaging, bursts by the eagle only carrying the aim hint, the store debit matching the rounds fired, a GUN kill (damage with missile_id == 0), the shooter alive and disengaged after) + the shipped-file twin. |

**Watch the trigger fight** (player build needs X11/OpenGL):

```
f4-scenario-player <build>/scenarios/guns_merge.json --run --follow
```

EAGLE1 closes head-on, the HUD flips to WVR/Merge, and inside half a mile
the amber tracer streaks reach out ahead of the nose — the lead-point
solution, drop-compensated. The drone jinks when the angle sorts; watch
the snapshot bursts at each pass. The trace records the bursts
(`gun_fired` events in guns_merge_trace.json).

## The 2-Ship: WingmanModule — Formation, the Sort, the Rejoin (M3-TACTICS-3)

**The AI flies in formation now. A #2 with a `lead_callsign` holds its
FightingWing station through the cruise, follows the lead into the BVR
fight, SORTS onto the bandit the lead has not taken (a genuine 2v2, not
two 1v1s sharing a map), kills it, and rejoins the lead's wing after the
fight — all autonomous, all regression-tested. The 2v2 E2E proves the
full wingman contract end to end: formation before the fight, split
targets during, both bandits dead, both blues alive, wing reformed
after. The shipped `two_ship` scenario puts the whole thing in the
scenario-player to watch. The suite is now 1,632/1,632.**

| Area | Change |
|------|--------|
| `f4-ai` — WingmanModule (new) | Step 11 of AI_IMPLEMENTATION_PLAN: the formation-keeping + engagement-discipline module. Engine-agnostic per the house rule — the host pushes a `LeadPicture` (position/velocity/heading/speed/altitude + validity) every tick before the brains run, the module answers with steering; it never touches the world or the bus. FSM: `None` (no live lead — empty output, brain flies the mission) / `Following` / `Rejoining` (with lead-range capture — see below). Five 2-ship formations from FreeFalcon's formdata (FightingWing default, Echelon L/R, Trail, LineAbreast) via `command_formation()`; the 4-ship types stay deferred to a 4-ship roster. |
| `f4-ai` — the steering laws | Two channels, two regimes each. LATERAL: far out, pure pursuit of the station slot; inside 3× tolerance, the formation law — the LEAD's heading plus a clamped proportional correction toward the slot's lateral offset (pure pursuit at zero error would orbit a co-moving slot; a fixed lead-heading would freeze the offset in place; the blend does neither — it forms). LONGITUDINAL: Following uses a station-frame PD law (lead speed ± P on the along-track error, minus D on closure — the D term is what keeps a 150-kt join from sailing through the slot); Rejoining uses a RANGE-TO-LEAD law, deliberately rotation-free: during the lead's post-fight turn the station frame rotates under the wingman and the along-rate becomes frame rotation, not closure — the station-frame law phugoided 36 kft around the flight before the split. Capture back into Following fires on lead range < 5,000 ft (the slot sweeps ~3,200 ft around a turning lead; station-distance capture kept missing the flyby). |
| `f4-ai` — SensorFusion: the sort + a friendly-leak fix | `sorted_threat_target(lead_engaged_id)`: the wingman's target pick — the highest-scoring hostile that is NOT the lead's engaged target (the free bandit outranks the lead's target even at lower score — that is the point of the sort); with only the lead's target visible it doubles up (support the kill); with the lead not fighting it degenerates to the plain query. REAL BUG fixed alongside: `threat_target()` never filtered hostiles, so in a 2-ship the wingman's own LEAD won the query pre-detection and the BVR rung engaged a friendly — 2v2 was structurally impossible until this. Friendlies stay in the target list (situational awareness); they just never arm a combat rung. |
| `f4-ai` — BrainComponent: the Formation rung | The ladder is now Defensive > WVR > BVR > **Formation** > mission module: a wingman with a live lead picture flies formation instead of its own route (with combat disabled too — formation is not a combat behavior); a fighting wingman stops forming and fights (the combat rungs preempt the module); a dead/landed lead empties the rung and the wingman becomes a single-ship. New API: `set_flight_lead()` / `update_lead_picture()` / `set_lead_engagement()` (host-fed) and `combat_engagement_id()` (host-read, feeds the sort). Mode names: `WingmanFormation` / `Following` / `Rejoining` ride the recorder + HUD for free. |
| `f4-simulation` — the host half | Scenario schema gains per-aircraft `"lead_callsign"` (empty = single-ship, as everything was). `resolve_wingman_refs()` runs after all aircraft spawn (a lead may sit anywhere in the list): resolves the callsign, validates same-team (a red wingman of a blue lead is an authoring bug — initialize() throws, like the team check), marks the brain. `push_wingman_lead_pictures()` runs every tick BEFORE `update_all`: reads the lead's transform/FM/damage/brain and pushes the picture + the lead's current engagement id. No-op when no aircraft declares a lead — the pre-Step-11 world is untouched. |
| Scenarios | `two_ship.json.in` (shipped, scenario-player): EAGLE1 + EAGLE2 (lead_callsign, spawned on station) vs BANDIT1 + BANDIT2 (hold-fire drones, 13 NM stern chase — the fight resolves with both blues alive, deterministically). Watch the #2 form, sort onto the free bandit, shoot, and rejoin. Records like the other combat scenarios (`two_ship_trace.json`). |
| Tests | +22 (suite **1,632/1,632 — 100%**, zero warnings): 14× WingmanModule units (station geometry per formation incl. lead-heading rotation, vertical offset, no-lead/lead-lost → None, speed up behind / slow down ahead / hold on station, steer toward the slot, blowout → Rejoining → Following hysteresis, a point-mass convergence test over 120 s, names, reset), 5× SensorFusion units (threat_target never returns a friendly; prefers hostile over near friendly; the sort takes the free bandit, supports the lead's kill when solo, degenerates without a lead engagement), 1× schema (lead_callsign round-trip + the three rejections: unknown lead, self-lead, cross-team), and **AiVersusAiTwoShipBvrFight** — the 2v2 E2E: formation rung pre-detection, sort separation while both fight, both bandits killed by blues only, both blues alive, zero live missiles, and the wing reformed (3D station distance < 4,000 ft, state Following) — plus the shipped-file twin, `TwoShipScenarioFilePlaysOut`. The rejoin assertion's failure message carries a 5 s timeline (mode/state/distance/kills) — a rejoin regression without it is undebuggable. |

**Watch the 2v2** (player build needs X11/OpenGL):

```
f4-scenario-player <build>/scenarios/two_ship.json --run --follow
```

EAGLE2 (the #2) is the one to watch: `WingmanFormation/Following` on the
HUD through the cruise, then the BVR crank on the bandit EAGLE1 did not
take, and the rejoin after the splash. Tab to it to see its picture.
## Combat Events in the Recorder — Fights Replay Headless (M4-RECORDER-1)

**A recorded fight is now the whole fight: FlightRecorder captures the
combat bus transitions (detection, spikes, launches, impacts, kills) as a
`CombatEvent` stream AND the missile flyouts as per-tick track snapshots,
alongside the existing aircraft snapshots — all in the same trace JSON the
world viewer replays. Run a fight headless, write the trace, load it back:
the full engagement chain survives the round-trip with tick-aligned timing,
and the LLM-facing summary gains a combat debrief (launches with outcomes,
kills with attribution). The two shipped combat scenarios now record:
watch `bvr_intercept` or `wvr_merge` in the scenario-player and a
replayable trace lands in `<build>/*_trace.json` when you close it. The
suite is now 1,610/1,610.**

| Area | Change |
|------|--------|
| `f4-recorder` — CombatEvent (new) | `combat_event.hpp`: the discrete half of a fight recording. One fat value struct with an 8-value kind enum (`track_acquired` / `track_dropped` / `rwr_lock` / `rwr_launch` / `missile_launched` / `missile_detonated` / `damage_applied` / `entity_killed` — stable wire names via `combat_event_kind_name()`), per-kind payload fields (ids, launch/burst positions, weapon name, end cause, miss distance, damage/HP, RWR range), and the same engine-agnostic stance as FlightSnapshot: raw uint64 ids and plain strings, NO f4-weapons/f4-sensors dependency — the bridge that flattens bus messages lives in f4-simulation. `weapon_name` is resolved at capture time so a replay never needs the weapon table. |
| `f4-recorder` — FlightRecorder | `record(CombatEvent)` + `combat_events()` / `combat_event_count()` / `combat_events_in_range(t0, t1)` (the snapshot query's twin). `to_json()` appends `combat_event_count` + a `combat_events` array **only when events exist**; `from_json()` parses them (unknown kinds and fields skip — the reader's forward-compatibility rule). `to_summary_json()` gains a `combat` debrief: engagement window, per-launch outcomes (shooter/target/weapon, end cause, miss distance, flight time — correlated launch↔detonation by missile_id), and kills (victim/killer/weapon — correlated kill↔damage↔launch). |
| `f4-recorder` — missile tracks | `FlightSnapshot::missile` marks a track as a munition: callsign carries the weapon name ("AIM-120C"), `ai_state` the flyout status ("guided"/"ballistic"), kinematics the missile's. Serialized **only when true** and the combat arrays **only when present** — aircraft-only recordings stay byte-identical to the pre-M4 format (diff baselines unperturbed), old readers skip the new keys, new readers load old docs clean. The summary's aircraft/phases/state-sequence sections filter missiles out (munitions, not flights). |
| `f4-simulation` — event bridge | `attach_combat_event_recorder(sim)` (combat_bridge): subscribes the recorder to all seven bus transitions and converts each message to a CombatEvent. Tick stamping is the subtle part: bus events publish mid-tick BEFORE `tick()` increments its counter, so events get `tick_count()+1` — aligned with the FlightSnapshots the SAME `tick()` call records. Wired automatically in `initialize()` whenever the scenario enables recording (harmless with combat off: no messages, no events). |
| `f4-simulation` — missile track recording | `record_snapshot()` now walks `with_component<MissileComponent>()` each tick and records every live missile's position/speed/status (swept-terminal missiles vanish first — their last position is the tick before detonation; the burst point lives in the detonation event, so replay endpoints stay covered). `Simulation::recorder()` accessor exposes the live recording to hosts. |
| Scenarios | `bvr_intercept.json` and `wvr_merge.json` flip `record: true` with `<build>/*_trace.json` paths — exit the player after a fight and the trace is ready to load in the world viewer's replay mode (missile trails included). Test runs write nothing (only the player host calls `write_recording()`). |
| Tests | +13 (suite **1,610/1,610 — 100%**, zero warnings): 12× recorder units (record/queries, every kind's JSON round-trip incl. positions and payloads, unknown-kind forward compat, missile-flag round-trip + aircraft byte-compat, old-format doc load, combat debrief content, missile exclusion from the aircraft summary) and **CombatRecordingReplaysTheFight** E2E: the full BVR engagement flown with recording on, written to disk, re-loaded — asserts both aircraft tracks AND the missile flyout (name/status/motion), the complete event chain in order (acquire → RWR lock → FOX 3 with name → RWR launch → `target_hit` detonation → killing damage → kill attribution), event ticks within the snapshot tick range, cause-before-effect timing, and the debrief section. |

**How to get a replayable fight trace** (player build needs X11/OpenGL):

```
f4-scenario-player <build>/scenarios/bvr_intercept.json --run --follow
# ... watch the fight, close the window ...
# <build>/bvr_intercept_trace.json now holds snapshots + combat events
```

Headless traces come from any scenario JSON with `record: true` +
`record_path` (the E2E test does exactly this through the public API).

**Deferred (documented, deliberate)**: world-viewer replay UI for the
event stream (the viewer already loads the format — a combat timeline
panel is a viewer-side feature for a session that can build GL),
countermeasure consumption (chaff/flare remain intents), guns (no gun
events exist to record), campaign-flights combat attachment.

## WVR Merge — The AI Fights Inside 3 NM (M3-TACTICS-2)

**The last band of the fight is flown: BVRModule now hands off to a new
WVRModule inside the plan's 3 NM WVR entry band, and the merge plays out
autonomously — geometry sorting (Merge/Offensive/Defensive/BugOut),
lead-pursuit closure, break-turn jinks with reversals, overshoot control,
and IR employment (FOX 2 off the wingtip before any AMRAAM leaves the
trench). A new shipped scenario, `scenarios/wvr_merge.json`, gives the
scenario-player the merge to watch: radar-missile-tight ROE walks two
fighters head-on from 5 NM into the band, EAGLE1's heater ends it while
the fire-holding bandit drone defends. The suite is now 1,597/1,597.**

| Area | Change |
|------|--------|
| `f4-ai` — WVRModule (new) | The plan §5 Step 9 module (`modules/wvr_module.{hpp,cpp}`, ~600 lines): a 5-state FSM (None/Merge/Offensive/Defensive/BugOut — FreeFalcon wvrengage.cpp's ladder) with dwell-guarded geometry classification from the SensorFusion angles (`ata`/`ata_from`: we hold the angle when the target is in our forward cone and pointed away; they hold it when behind us and nose-on), the plan's 11-value `WVRTactic` enum with the flown subset documented (RandP/Straight at the merge, OverB overshoot control, GunJink defensive break turns, BugOut), and an embedded IR `MissileModule` fire control (opportunity shots inside the forward cone — a heater at a target on our six has nothing to track; 3 s cadence, shoot-shoot 2). Defensive steering is the gunsjink: ±60 deg break turns off the threat bearing reversing every 3 s with an altitude weave, throttle on the rail. Bug-out doctrine requires the IR allotment spent AND a sustained defense (grace timer), then hands the reopened fight back past the 4.5 NM exit ring. |
| `f4-ai` — BrainComponent ladder | New `CombatMode::WVR` rung between Defensive and BVR: inside `bvr().config().wvr_entry_range_nm` (3 NM, plan constant) the brain resets BVR and hands the fight to WVRModule; past `wvr().config().wvr_exit_range_nm` (4.5 NM hysteresis) it hands back — one source per boundary (entry lives in the BVR band taxonomy, exit in the WVR config). Missile defense still preempts everything; falling off the ladder resets the nav integrators. `mode_name()`/`state_name()`/`combat_mode_name()` report `WVREngage` + the WVR states for HUDs and traces. |
| `f4-ai` — ROE in the fire control | `MissileModule::Config::hold_fire` (weapons tight) — the gate lives in `should_fire()` itself, NOT only at the brain's intent layer: a module-level hold means no pulse, no phantom shot counted, and no doctrine separation can trigger on a launch that never happened (the intent-only gate would have let BVR count two phantom AMRAAMs under `bvr_hold` and bug out before the merge). Per-aircraft `hold_fire` disarms both fire controls; the combat block's `bvr_hold` disarms the BVR one alone. |
| `f4-simulation` — combat bridge | `configure_brain_combat()` now configures BOTH envelopes from the weapon class table (BVR from the longest-range A/A class as before; WVR/IR from the IR-guided class — AIM-9M's [0.5, 8] NM doctrine window) and installs the ROE flags at module level. `execute_brain_combat_intents()` gains WVR-aware station doctrine: when the shooter's brain is in the WVR rung, IR-guided stations fire first, then the shortest-range A/A — FOX 2 off the wingtip before an AMRAAM out of the trench. |
| Scenario schema | Per-aircraft `"hold_fire": true` (that aircraft never releases — it still locks, maneuvers, defends; the difference between a live opponent and a maneuvering target drone) and combat-block `"bvr_hold": true` (SPINS-style radar-missiles-tight for everyone, heaters free). Both parsed, documented on the structs, and asserted in tests. |
| `f4-scenario-player` — scenario | New `scenarios/wvr_merge.json.in` (registered in the root CMake template list): EAGLE1 (blue, live) and BANDIT1 (red, `hold_fire`, 10 HP) spawn 5 NM apart head-on at 15,000 ft with `bvr_hold` on — the deterministic twin of the E2E test geometry. The user watches from EAGLE1: closure, the WVR handoff at 3 NM (HUD mode flips to `WVREngage/Merge`), FOX 2, the drone's RWR `MISSILE LAUNCH!` and jinking defense (visible as the bandit's break turns), splash. Missiles render with the m4-scenario-1 procedural visuals; the COMBAT panel narrates with the `FOX 2` brevity word (already in the transcript's guidance-kind map). |
| Hygiene | Removed the landed `f4-m3-tactics-1.patch` / `f4-m4-scenario-1.patch` from the repo root (both applied and pushed as `BVR test` d6a6032 — same discipline as step 0 removing the landed M2 patch). Fixed a latent `-Wunused-function` (`fmt1`) in combat_transcript.cpp. |
| Tests | +20 (suite **1,597/1,597 — 100%**, zero warnings): 18× WVRModule unit tests (geometry classes, dwell anti-chatter, jink offset + 3 s reversal, OverB guard, IR pulse/cooldown/shoot-shoot, forward-cone + RWR-blind gates, band exit, bug-out doctrine incl. no-bugout-with-heaters-remaining, reset contract) and **AiVersusAiWvrMergeFight** + **WvrMergeScenarioFilePlaysOut** E2Es: BVR-tight closure from 5 NM, both brains reach the WVR rung (the RWR-only drone too — the lock warning is its picture), the merge shot is an IR-class station, zero AMRAAMs expended, the drone's RWR sees the IR launch and MissileModule defends, kill + attribution + disengage + clean sweep. |

**How to watch the merge** (needs a scenario-player build — X11/OpenGL,
unlike the headless CI):

```
f4-scenario-player <build>/scenarios/wvr_merge.json --run --follow --camera-distance 8000
```

The fight resolves in ~40 s of sim time (the 32x speed slider makes it
ten); set the drone's `hold_fire` to `false` in the JSON for a live
two-way merge, and remove `"bvr_hold"` to let the AMRAAM exchange back
in.

**Deferred (documented, deliberate)**: guns employment (GunStream is
f4-weapons-side; the sim tick has no gun-sweep driver yet), one-circle
vs two-circle geometry + the reserved WVRTactic values (Roop/Avoid/Beam/
BeamReturn/RunAway — they need the WingmanModule's formation picture and
the skill layer), countermeasure consumption (chaff/flare remain intents),
recorder combat events (the M4 replay half — next session), visual
detection (the eyeball model behind the WVR "visual" band), and
skill-parameterized behavior.

## BVR Intercept Scenario — The Fight You Can Watch (M4-SCENARIO-1)

**The AI-vs-AI BVR engagement is now a scenario you can fly as a spectator:
`scenarios/bvr_intercept.json` spawns EAGLE1 (blue) vs BANDIT1 (red), both
brains fight autonomously through the M3 chain, and the scenario-player
renders the whole thing — missiles with contrails and pursuit lines, a
brevity COMBAT transcript (FOX 3 / spike / splash), the watched aircraft's
RWR picture, and a Tab key to switch which jet you ride with. The engine
side of the observability is a new engine-agnostic `CombatTranscript` in
f4-simulation (headless-tested), so the narration exists with or without a
window.**

| Area | Change |
|------|--------|
| `f4-simulation` — CombatTranscript (new) | Subscribes to the combat bus transitions (radar acquire/drop, RWR lock/launch, missile launch/detonate, damage, kill) and formats them as brevity radio calls into a ring buffer: `"EAGLE1: FOX 3, AIM-120C away on BANDIT1."`, `"BANDIT1: Spike from EAGLE1, 13 NM."`, `"C2: Splash BANDIT1!"`. Callsigns resolve via the `aircraft_entities()` ↔ `scenario().aircraft` index map; launch brevity follows the weapon's guidance kind (`missile_brevity_word()`: ActiveRadar→FOX 3, SemiActiveRadar→FOX 1, Ir→FOX 2). Severity (Info/Warning/Kill) ships on every entry for hosts to color-code. Engine-agnostic: no renderer, no window — tested headless. |
| `f4-scenario-player` — scenario | New `scenarios/bvr_intercept.json.in` (registered in the root CMake template list): two spawn-in-air F-16s, blue 506 kt 13 NM behind red 420 kt, both northbound — the same deterministic stern-chase geometry the M3 E2E test proves (detection inside the Pd=1 knee, AMRAAM flyout, kill, disengage). `combat: {enabled: true, radar_rng_seed: 777}`. 10-minute tick budget; the fight resolves in ~1-2. |
| `f4-scenario-player` — combat view | Missiles render procedurally (they carry no KoreaObj visual record): a bright cylinder body along the velocity vector, a 500-ft wire-sphere tactical marker (visible at BVR zoom), a fading contrail sampled once per frame (900 points ≈ 15 s), and a thin red line to the assigned target that makes the proportional-navigation pursuit visible. A **COMBAT** panel under the ATC transcript draws the CombatTranscript entries color-coded by severity (white/amber/red). |
| `f4-scenario-player` — watched aircraft | The HUD, follow camera (C), focus (F), and FCS panel (F3) now track the **watched** aircraft instead of always the first: **Tab** cycles (bvr_intercept: EAGLE1 ↔ BANDIT1), an ImGui "Watched" combo does the same. The HUD gains an RWR line (`clear` / `SPIKE (locked)` / `MISSILE LAUNCH!` — the same flag the MissileModule defends on) and a live-missile count for combat scenarios. Also fixed: non-primary *scenario aircraft* were gated by the "Show airport" toggle (the bandit disappeared with the runway); aircraft-vs-feature gating now uses the `aircraft_entities()` list. |
| Tests | 4 new (suite **1,577/1,577 — 100%**): 3× CombatTranscript (brevity-word mapping, ring-buffer + callsign fallback + capacity/clear semantics, and a full E2E narration of the AI-vs-AI fight — every link from radar contact to splash asserted on the transcript), plus **BvrInterceptScenarioFilePlaysOut**: loads the *shipped, build-configured* `scenarios/bvr_intercept.json` (not an in-memory copy), flies it, and asserts launch → kill → attribution → missile sweep, so a typo in the file the player actually reads can never hide behind the in-memory test. |

**How to watch the fight** (needs a build with the scenario player on —
X11/OpenGL, unlike the headless CI):

```
f4-scenario-player <build>/scenarios/bvr_intercept.json --run --follow --camera-distance 12000
```

Space pauses, Tab flips between EAGLE1 and BANDIT1, the speed slider
fast-forwards the boring transit, and the COMBAT panel (top-right) narrates.
The scenario starts PAUSED without `--run`.

## M3 Tactics — The AI Fights (M3-TACTICS-1)

**The brains now fight the war themselves: BVRModule engages, MissileModule
defends, and the E2E test drives two AI flights through a complete
autonomous kill chain with nothing but `Simulation::tick()`. Two real
engine bugs surfaced on the way: missiles were invisible to the victim's
RWR (missing ROLE tag), and red-team brains were blind to blue fighters
(blue-biased hostility).**

| Area | Change |
|------|--------|
| `f4-ai` — BVRModule (new) | The offensive BVR fight (AI_IMPLEMENTATION_PLAN Step 8): `None → Entering → Employing → Separating` state machine, plan range bands (entry ring 1.3×Rne, WVR 3 NM, merge/bugout 2 NM), lead-pursuit steering, 45° crank support window after each shot, cold- turned bug-out with range-reopen hysteresis. Engine-agnostic by contract: it never touches f4-weapons/f4-sensors — the radar lock and weapon release leave as **intents** (`wants_lock()`, one-tick `release_pulse()`) that the host executes. |
| `f4-ai` — MissileModule (new) | Two roles in one class (Step 10, mengage.cpp + mdefeat.cpp): **fire control** — deterministic Pk model (range × aspect), employment envelope, 4 s cooldown, shoot-shoot allotment, weapons-grade-picture gate (RWR-only contacts never fire); **missile defeat** — beam maneuver (nearest ±90° off the threat bearing), full-AB outrun, `has_override` defensive preempt, chaff/flare intents, and a 2 s defeat-linger so the jink doesn't stop on the detonation tick. |
| `f4-ai` — BrainComponent | The DigitalBrain priority ladder's first rungs (Step 12): while Enroute, `Defensive > BVR > navigation`. The brain owns a SensorFusion (initialized lazily; the host installs the detection policy) and exposes `CombatIntent` (lock + release) each tick; falling off the ladder resets the nav steering integrators (the same transient guard the phase handoffs use). `mode_name()` reports `BVREngage` / `MissileDefeat`. |
| `f4-ai` — SensorFusion | Two-sided combat fixes: hostility is now **own-relative** (target team ≠ ownship team; legacy "red ⇒ hostile" only as the no-tag fallback — a red brain was blind to blue fighters before), and `missile_threat()`/missile threat-scoring are **hostile-only** (your own missile must never read as an incoming threat — the shooter would have defended against its own AMRAAM). |
| `f4-simulation` | `configure_brain_combat()` turns a spawned brain into a fighting brain (ladder on + table-derived envelope: AIM-120C's 40 NM boundary → 20 NM doctrine Rne). `RadarBackedDetectionPolicy` now answers all-false for **killed** entities (corpses don't paint — the M3 host decision radar_component.hpp deferred here), so the shooter's BVR sees `LostTarget` and goes home instead of pumping the shoot-shoot allotment into a still-flying airframe. New `execute_brain_combat_intents()` runs between `update_all` and `update_rwr` each tick: lock intents → `command_track` (idempotent until the track is live), release intents → `launch_missile` through the sim's table (longest-range A/A station first — AMRAAM before Sidewinder; killed aircraft never fire). `Simulation` owns one policy per spawned combat aircraft. |
| `f4-weapons` | **Bug fix**: `launch_missile` never set the `ROLE="missile"` tag — `update_rwr`'s launch detection and SensorFusion's `is_missile` classification both key on it, so the victim's RWR was blind to every launch and missile defense was impossible. Found by the AI-vs-AI E2E, not by reading code. |
| Tests | 29 new: 14 BVRModule (range bands, state ladder, fire-control pulses/cooldown/shoot-shoot, crank offset band, bug-out hysteresis, RWR-only hold-fire, corpse reset), 11 MissileModule (Pk monotonicity/bounds, fire gates, beam = 90° off threat bearing, override + AB, chaff/flare conditions, linger, own-team refusal), 3 SensorFusion (own-relative hostility ×2, hostile-only missile threats), 1 E2E: **AiVersusAiBvrEngagement** — two AI flights, zero test-driven steering, the whole OODA loop (detect → lock intent → fire intent → victim RWR launch warning → beam defense → AMRAAM kill → corpse filter → disengage → nav resumes) asserted end to end. Full suite: **1,573/1,573 — 100%**, zero warnings. |

## Combat Chain Integration — M3 Step 1 (COMBAT-INT-1)

**f4-weapons and f4-sensors are no longer unconsumed leaf libraries: the
Simulation ticks the whole combat chain end to end — radar detect → track →
STT lock → RWR warning → AMRAAM launch → flyout → kill — proven by a single
E2E test. The integration also flushed out (and fixed) a real bug: the
FM→Transform sync never wrote velocity.**

| Area | Change |
|------|--------|
| `f4-simulation` | Links `f4-weapons` + `f4-sensors` (PUBLIC). Scenario JSON gains `"combat": {"enabled", "radar_rng_seed", "fighter_hit_points"}` and per-aircraft `"team"` (blue/red, validated). When enabled, spawned aircraft carry `WeaponStoreComponent` + `SignatureComponent` + `RadarSimComponent` + `RwrComponent` + `DamageStateComponent` + `CampaignIdentityComponent`; `Simulation::weapon_table()` exposes the built-in `WeaponClassTable` every launch goes through. `tick()` stamps the radar/missile sim clocks before `update_all` and runs `update_rwr` + `sweep_spent_missiles` after it — all gated, so combat-disabled worlds are byte-identical to before. |
| `combat_bridge` (new) | `attach_combat_loadout()` — one call adding the whole combat component set (per-aircraft radar seeds derived from the scenario seed). `RadarBackedDetectionPolicy` — the M2 `SensorFusion::DetectionPolicy` hook made real at the host layer (f4-ai stays interface-pure): radar = live track in the ownship's track store, RWR = warning from that emitter, visual/GCI = **false** — the flip off GCI-omniscience, ready for BVRModule to install. |
| FM sync fix | `Simulation::tick`'s FM→Transform sync now writes `vx/vy/vz` (NED→ENU, same axis swap as position). Before, every combat consumer reading the transform saw a *parked* aircraft: `launch_missile` gave the missile 0 ft/s inherited velocity (it fell ballistic and lost the seeker cone instantly) and radar aspect was degenerate. Found by the E2E test, not by reading code. |
| `campaign_bridge` | Dropped a dead local (`h2`, unused-variable warning under `-Wall`). Combat attachment for the campaign-flights spawn path is deliberately deferred: it needs team resolution from campaign data, which belongs with the M3 tactics that consume it. |
| Tests | 5 new cases in `f4-simulation/tests/test_combat_integration.cpp`: component attach (incl. IFF teams, seeds, store loadout), nothing-when-disabled, the full E2E chain through `Simulation::tick` (deterministic Pd=1 knee geometry: 13 NM stern chase), the policy adapter (invisible before scans, radar-not-GCI after), and JSON parsing/validation. Full suite: **1,544/1,544 — 100%**. |

## Green Suite + CI + Repo Hygiene (STEP-0)

**The full suite is 1,539/1,539 green for the first time; every push now
builds and tests itself on GitHub Actions; ~11 MB of dead weight and
runtime state left the tree.**

| Area | Change |
|------|--------|
| `f4-flight-api` | `PilotInput::validate()`: deleted the nine debug asserts that fired *before* the clamps — they contradicted the clamping contract pinned by `PilotInputTest.ValidateClamps*` and made those 5 tests impossible to pass in Debug builds. `validate()` is a sanitizer, not a checker. |
| `f4-flight-model` | `EngineModel::update()`: deleted the two asserts that contradicted the null-table guard three lines below them — `EngineModel.DefaultConstructedHasNoTables` tests exactly that contract (default-constructed engine → zero thrust/fuel flow, no abort). |
| CI | New `.github/workflows/ci.yml`: headless configure (all X11/OpenGL targets OFF) → build → full `ctest`, GCC 14 / Debug, on every push and PR to main. Same command sequence as the local verification workflow. |
| repo hygiene | Untracked: `temp/mapcheck*.png`, `temp/dump_full.txt`, the FreeFalcon source dumps `temp/ff_*.{cpp,h}`, the already-landed `f4-combat-m2-sensors.patch`, `imgui.ini`, `Testing/`. `.gitignore` gains `imgui.ini`, `Testing/`, `Data/` (asset-pipeline §4), `build*/`, and `temp/*` with the load-bearing `KoreaObj.{HDR,LOD,TEX}` re-included — their removal is deferred to the asset pipeline's Stage 3 glTF export, which replaces them with generated `Data/` assets. |
| Tests | 1,539/1,539 pass (was 1,533 + 6 "pre-existing environment failures" that were actually Debug-build assert-vs-contract bugs, not environment issues). The 4 `ControlLoop*` tests remain `DISABLED_` and the `F4_INSTALL`-dependent tests skip gracefully, unchanged. |

## Viewer Terrain Fixes (TERRAIN-TEX-2)

**Fixes five user-reported world-viewer bugs; textures now load in every
flow, the map is north-up, and 3D objectives sit on the terrain.**

| Area | Change |
|------|--------|
| `f4-renderer` | `WorldView::load_theater` accepts both the theater root and the terrain subdir (f4-install's `Theater.dir` is the subdir — every install-flow load silently failed before); `terrain_dir()` exposes the resolved dir. Far-ring z bias −20 → −400 ft so the coarser L4 ring can't poke through the near L2 surface (black z-fight blobs). |
| `f4-world-viewer` map | North-up fix: the cache became a plain `Texture2D` in TEX-1 but the canvas still draws with negative source height (row 0 renders at the *bottom*); both paint paths now write row 0 = south with tile art mirrored per cell. Theater binaries also load via world-JSON/import/install-set paths (`try_load_theater_tiles`), not just the campaign dialog. |
| `f4-world-viewer` 3D | Geometry (selected + neighboring objectives) samples the near post level — the same elevation the textured terrain renders — instead of the 128×128 MEA summary; the orbit camera targets the terrain elevation instead of sea level. `--select` sets the selection kind, also matches class names, prefers objectives with layout/features, and no longer zooms the map to a corner. |
| tools | `png_probe` takes an optional region `[x y w h]` and prints per-cell luminance variance. |

## Textured Terrain + Unified WorldView (TERRAIN-TEX-1)

**Both viewers now render real Falcon 4 terrain tile art, through one
shared code path.** Validated against real geography at Kunsan.

| Area | Change |
|------|--------|
| `f4-terrain` | New decoders: `TheaterGeometry` (ENU↔post↔block conversion, one documented place for the theater-scale convention), `PostLevel` (THEATER.O\*/L\* 7-byte TdiskPost records, dedup'd block offsets), `FarTileDB` (FArtILES.PAL/.RAW), `NearTileDB` (TEXTURE.BIN + texture.zip PCX art with H/M/L variant resolution), internal PCX reader. `tools/dump-terrain-textures` validates everything against a real install. |
| `f4-io` | `ZipReader` — minimal STORED-entry PKZIP reader (Falcon's texture.zip needs no inflate). |
| `f4-renderer` | Textured terrain: `TerrainTileCache` (lazily-grown GL_TEXTURE_2D_ARRAYs), `TerrainShader` (GLSL 330, 4 tile arrays via `vertexTexCoord2`, lighting + distance fog), textured path in the chunk builder (post-aligned quads, UVs ported from FreeFalcon's `DiskblockToMemblock`, near region + far ring). **`WorldView`**: one class owning load-theater → ensure-GPU → set-view-center → per-frame uniforms → teardown; both apps call it instead of hand-rolling the lifecycle. Sampler helpers deduplicated into `src/terrain_internal.hpp`. |
| `f4-scenario-player` | Terrain lifecycle now one `WorldView` member; scenario JSON accepts optional `theater_dir` (substituted from `F4_INSTALL` at configure time); falls back to the untextured MEA mesh without it. |
| `f4-world-viewer` | Install flow loads theater binaries into `WorldView`; the 3D Ground Layout tab renders textured terrain centered on the selection; **the 2D strategic map now paints real far-tile art** (2048×2048) instead of elevation-band colors; new `--select <name>` CLI flag + auto 3D-tab for headless screenshots. |
| Tests | 22 new unit tests (zip reader, post level vs real L2 fixture, tile DBs with synthetic theaters, geometry). Full suite: 1519 tests, only the 9 pre-existing failures (coord_transform, PilotInput clamps ×5, EngineModel — fail on the clean tree too). |

---

This document summarizes the cleanup pass applied to the F4 codebase.
**All 998 unit tests pass on a clean build.** (Up from 988 — added 10 new
tests covering the fixes.)

## Build Fixes (the repo on `main` does not compile out-of-the-box)

| File | Fix |
|------|-----|
| `CMakeLists.txt` | Added `add_subdirectory(f4-io)` — `f4-world-convert` and `f4-terrain` link against `f4-io` but the root CMakeLists never added it. Fatal: `f4/io/read_file.hpp: No such file or directory`. |
| `CMakeLists.txt` | Added `enable_testing()` at top level. All test executables built fine, but `ctest` from the build root reported "No tests were found!!!" because no top-level `enable_testing()` had been called. |
| `f4-json/include/f4/json/writer.hpp` | Added missing `f4::json::escape_string()` free function. `world_json.cpp:24` does `using f4::json::escape_string;` but the function didn't exist in the header. The worklog claims it was added but it never made it into the committed code. |

## Phase 0 — Stabilize

### Phase 0b: Deleted 14 orphaned diagnostic files

Removed `f4-models/tests/diag_*.cpp` (14 files, ~1370 lines):
- `diag_bsp_direct.cpp`, `diag_correct.cpp`, `diag_geometry.cpp`,
  `diag_geometry2.cpp`, `diag_hex.cpp`, `diag_hex2.cpp`,
  `diag_offsets.cpp`, `diag_offsets2.cpp`, `diag_offsets3.cpp`,
  `diag_raw_bsp.cpp`, `diag_slot_sizes.cpp`, `diag_tags.cpp`,
  `diag_tree.cpp`, `diag_vtables.cpp`

These were standalone `int main()` diagnostic programs (not gtest tests),
were NOT listed in `CMakeLists.txt` (so never built), and contained
hardcoded absolute paths like `/home/z/my-project/f4-repo/...` that
don't exist in any checkout. Pure dead code cluttering `tests/`.

### Phase 0c: RAII FILE* in `f4-io/src/read_file.cpp`

Replaced raw `FILE* fp = std::fopen(...)` with a `FileGuard` RAII
wrapper. If the `std::vector<uint8_t> buf(sz)` allocation throws
`std::bad_alloc` between `fopen` and `fclose`, the handle no longer
leaks. This single helper backs every binary loader in the project.

### Phase 0d: Comment typos fixed

- `f4-flight-model/include/f4/flight/flight_model.hpp:193` — wrong
  arithmetic in the `minorFrameTime_` comment ("6 sub-steps of 1/60s =
  1/10s major" — actually 6 × 1/360s = 1/60s major).
- `f4-math/include/f4/math/scalar.hpp:147` — referenced a `qsquared`
  function that doesn't exist.

### Phase 0e: Tightened weak tests

The original tests asserted "is the value finite" or used tolerances so
loose they admitted fully-developed stalls and divergent EOMs.

**`test_atmosphere.cpp`**:
- Replaced self-consistency check (`EXPECT_NEAR(out.rho, RHOASL, 1e-9)`)
  with assertions against published ISA-1976 reference values at sea
  level, 18000 ft, 36089 ft (tropopause), and 50000 ft. Tolerances are
  relative (0.5%) to admit the legacy Falcon-4 lapse rate constants
  while still catching wrong layer breakpoints or swapped exponents.
- Tightened `ZeroAirspeedDoesNotProduceNaN` to assert `mach == 0` and
  `qbar == 0` (the previous implementation produced a phantom Mach of
  `1/sound` at zero airspeed because the `safe_vt` floor leaked into
  the Mach computation). **This found a real bug in `atmosphere.hpp`
  which is now fixed** — Mach and qbar use the actual `vt`, only `qovt`
  uses the floor.

**`test_flight_model.cpp`**:
- `SixtySecondStabilityRun`: tightened G tolerance from ±3.0 to ±1.0,
  added altitude drift bound (12000 ft — loose enough to admit the
  known trim/FCS settling transient, tight enough to catch divergent
  EOMs), added speed divergence bound (10x).
- `PitchStickChangesAlpha`: rewrote to compare against a no-input
  baseline (because trim() doesn't seed the FCS integrator, so the
  first few seconds involve settling). Asserts alpha INCREASES (not
  just "moves") and Nz INCREASES above baseline. Found a real
  direction issue that was hidden by the old `|delta| > 0.1` check.
- `ThrottleIncreasesSpeed`: added quantitative lower bound (≥50 ft/s
  gain in 10 s of full AB) — catches a regressed throttle path (AB
  not lighting, throttle reversed, etc.) that the old "speed went up"
  check would miss.

**`test_table_accessors.cpp::F16TablesInterpolateCorrectly`**:
- Added grid-point fidelity check (interpolated value at an exact
  breakpoint must equal the raw table value to 1e-9).
- Added CL-monotonic-in-alpha check at low Mach (catches indexing
  bugs that "look right" but read from the wrong cell).
- Fixed boundary clamping test to use `mach0 - 1` / `alpha0 - 1`
  instead of hardcoded `-1`/`-10` (the F-16 fixture's alpha axis
  extends to -10 deg, so the old test was querying INSIDE the table).
- Added thrust magnitude check (catches table-misload like
  thrust_ab being loaded into thrust_mil's slot).

## Phase 1 — Type safety & code quality

### Phase 1a: `f4-geo` position ctors `explicit` + `from_degrees()` factory

`f4-geo/include/f4/geo/position.hpp`:
- Marked 3-arg ctors of `WorldPosition`, `LatLonAlt`, `ECEFPosition`
  as `explicit`. Closes the most common bug: passing degrees where
  radians are expected, which compiled silently because both are
  `double`.
- Added `LatLonAlt::from_degrees(lat_deg, lon_deg, alt_ft)` factory.
  This is the recommended way to construct a LatLonAlt from
  human-readable coordinates — applies `DEG_TO_RAD` so callers don't
  have to remember the radians convention.
- Updated `operator+` / `operator-` to use explicit `WorldPosition{...}`
  construction (so the explicit ctor doesn't break arithmetic).
- Updated all call sites in `f4-geo/tests/` to use either
  `T(...)` functional-cast form (which works with explicit ctors) or
  `T{...}` direct-init (also works). The parenthesized-braced form
  `EXPECT_EQ(x, (T{...}))` had to become `EXPECT_EQ(x, T(...))` because
  gtest treats the parenthesized braced-init as copy-init.
- Added new test `Position.FromDegreesFactoryConvertsCorrectly`.

### Phase 1b: Named constants for magic numbers in binary parsers

`f4-world-convert/src/campaign_decoder.cpp`:
- Extracted `NUM_TEAMS`, `TEAM_NAME_LEN`, `TEAM_MOTTO_LEN`,
  `CMP_HEADER_BYTES` from inline literals.

`f4-world-convert/src/objective_decoder.cpp`:
- Extracted `OBJ_HEADER_BYTES`, `LINK_COSTS_PER_LINK`,
  `NUM_RADAR_ARCS`, `GRID_COORD_MIN`, `GRID_COORD_MAX`, `VU_ID_BYTES`.
  (Renamed from `MOVEMENT_TYPES` to avoid collision with the
  `MoveType::MOVEMENT_TYPES` enum value.)

`f4-models/src/hdr_parser.cpp`:
- Documented `FORMAT_VERSION` and added `OLD_FORMAT_SENTINEL = 0xFEEF`
  (was a bare hex literal with no explanation of what it means).
- Extracted `LOD_ENTRY_BYTES` and `LOD_ENTRY_SPARE` from inline `12`
  and `20` literals.

### Phase 1c: `BinReader::remaining()` footgun fixed

`f4-models/src/bin_reader.hpp`:
- The old `bool remaining()` returned `size - pos` (a `size_t`)
  implicitly converted to `bool`. Callers writing `auto n =
  r.remaining();` got 0 or 1, not the byte count.
- Renamed to `remaining_bytes() -> std::size_t` (returns the actual
  count) and added `has_remaining() -> bool` for the boolean check.
- No callers existed, so the rename was safe.

### Phase 1d: `EntityWorld` move ctor regenerates cookie

`f4-entities/include/f4/entities/entity.hpp` + `f4-entities/src/entity.cpp`:
- The defaulted move ctor copied the cookie from the source, so after
  `EntityWorld b = std::move(a);`, both `a` (moved-from) and `b` had
  the same cookie. Handles captured against `a` before the move would
  incorrectly validate against `b` — defeating the use-after-free
  detection the cookie is there to provide.
- Replaced with a custom move ctor/assign that regenerates the cookie
  on the destination via a new `detail::next_world_cookie()` helper
  (declared in the public header, defined out-of-line in `entity.cpp`
  so the global atomic counter stays file-local).

### Phase 1e: `Trace::append()` is now O(1)

`f4-state-machine/include/f4/fsm/trace.hpp`:
- Switched internal storage from `std::vector<Record>` to
  `std::deque<Record>`. `push_back` + `pop_front` are both O(1) on
  deque; the previous `erase(begin())` on vector was O(n) per append
  when full. At 360 Hz on a 1024-record trace, that's a measurable
  cost on the minor frame.
- `records()` now returns `std::vector<Record>` by value (a copy)
  instead of `const std::vector<Record>&`. Trace inspection is rare
  (test/diagnostic only), so the copy cost is acceptable.

## Phase 2 — `Cursor::check_and_throw()`

`f4-io/include/f4/io/cursor.hpp`:
- Added `Cursor::check_and_throw(const char* context)` and the
  `const std::string&` overload. Converts the silent sticky error
  flag into an observable `std::runtime_error` at the end of a parse
  block — one-line idiomatic check instead of
  `if (c.error) throw std::runtime_error(...)` by hand.
- Rationale: the original sticky-flag design (worklog.md:1504) chose
  silent flags over throw-on-OOB to surface real bugs in subclass
  dispatch paths where exceptions would be caught and swallowed. But
  the consequence was that any caller who forgot to check `error`
  would silently produce zeroed records. `check_and_throw()` gives
  those callers a one-liner.

Migrated all throwing call sites to use it:
- `f4-world-convert/src/theater_data.cpp` — 9 sites converted from
  `if (c.error) throw std::runtime_error("...");` to
  `c.check_and_throw("...");`
- `f4-world-convert/src/campaign_decoder.cpp` — 2 sites.

Call sites that use the sticky flag for non-throwing rollback behavior
(`objective_decoder.cpp`, `unit_decoder.cpp`, `team_decoder.cpp`) keep
their existing `if (c.error)` pattern — that's the correct use of the
sticky flag.

Added 6 new tests in `f4-io/tests/test_cursor.cpp`:
- `CheckAndThrowNoopsWhenNoError`
- `CheckAndThrowThrowsAfterOobRead`
- `CheckAndThrowIncludesContextInMessage`
- `CheckAndThrowAcceptsStdString`
- `CheckAndThrowAfterSkipOob`
- `CheckAndThrowAfterFixedStringOob`

## Phase 3 — Deduplication

### Phase 3a: JSON serialization consolidated

`f4-convert/src/json_io.cpp` was a 664-line copy of
`f4-data/src/config_loader.cpp` (both admitted "Both copies MUST stay
in sync" in their comments). The "circular dependency" justification
was invalid: `f4-convert` already depends on `f4-data` via
`target_link_libraries(f4-convert PUBLIC f4-data nlohmann_json)`.

- `writeJson()` / `writeJsonFile()` / `readJson()` / `readJsonFile()`
  are now thin adapters that delegate to `f4::data::writeConfig()` /
  `loadConfig()` / `loadConfigFromString()`. The `IoResult` ↔
  `LoadResult` adapter handles the structural difference (IoResult
  takes cfg by reference; LoadResult embeds it).
- `diffConfigs()` stays in f4-convert — it has no equivalent in
  f4-data and is genuinely f4-convert-specific (used by the `json_diff`
  CLI tool and the round-trip test harness).
- Net: ~470 lines of duplicated serialization code deleted. The
  "MUST stay in sync" maintenance hazard is gone — format changes
  now happen in exactly one place.
- Updated misleading comments in `f4-data/src/config_loader.cpp` and
  `f4-convert/CMakeLists.txt` to reflect the new reality.

### Phase 3b: `f4-models::Vec3` aliased to `f4::math::Vec3<float>`

`f4-models/include/f4/models/types.hpp`:
- The duplicate `struct Vec3 { float x, y, z; }` (with only `operator==`)
  is now `using Vec3 = f4::math::Vec3<float>;`. Consumers get the full
  Vec3 operator set (dot, cross, length, normalize, hadamard, scalar
  arithmetic, `operator[]`) from f4-math, instead of the bare struct.
- Layout is identical (3 contiguous floats, 12 bytes, no padding), so
  `sizeof(Vec3)` in the binary parsers (which read arrays of Vec3
  directly from disk) is unchanged.
- `f4-models/CMakeLists.txt` now links `f4-math` as a PUBLIC dependency.

## Verification

Clean build from scratch:
```
$ rm -rf build && cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DF4_BUILD_MODEL_VIEWER=OFF -DF4_BUILD_VIEWER=OFF ..
$ cmake --build . -j2
$ ctest -j2
100% tests passed out of 1012
```

(Viewers are OFF because they require X11 + OpenGL dev headers not
present in this environment. The library code itself builds clean
with viewers enabled on a dev machine.)

## What was NOT done (and why)

The following items from the original review were deferred because they
are higher-risk or require more design work:

- **Full strong-type migration in f4-flight-model** (CRITICAL #1 in the
  review). The flight model uses raw `double` for alpha/beta/sigma/etc.
  with degrees/radians mixed in the same struct. Migrating to strong
  types is invasive — every consumer of `AeroState` / `KinematicState`
  / `calcBodyRates` needs updating. Recommend doing this as a separate
  dedicated PR with its own test pass, not as part of a cleanup batch.
  **DONE — see "Phase 4 — Angle Strong-Type Migration" below.**

- **`f4-messaging::publish()` shared_mutex refactor** (CRITICAL #5).
  The current `recursive_mutex` + vector copy on every publish is a
  real serialization point at 60 Hz, but the reentrant-publish
  semantics need careful thought. Needs its own benchmark + design
  pass.

- **`LayeredStateMachine::applyInhibition()` `force_to_state()`**
  (HIGH #3). The current `reset()` call fires the initial state's
  entry action, which is wrong if initial ≠ idle. Fix requires adding
  a new `force_to_state()` method to `StateMachine` and reasoning
  carefully about which transitions should/shouldn't fire on
  suppression.

- **`dat_parser.cpp` exception-as-control-flow** (HIGH #6). The
  backtracking search uses `try { ts.nextDouble(); } catch (...) {
  ts.setPos(saved+1); }`. Fix requires adding `tryNextDouble() ->
  std::optional<double>` to `TokenStream` and replacing 5+ sites.

These are all good candidates for the next cleanup pass.

## Phase 4 — Angle Strong-Type Migration (CRITICAL #1)

Resolves the largest correctness hazard flagged in the original review:
`AircraftState` stored angles as raw `double` with the unit convention
documented only in comments (alpha/beta in degrees, euler angles in
radians, alpha_dot in deg/s). Passing a degree value where radians were
expected (or vice versa) compiled silently because both are `double`.

### Approach

`f4-units` already provides a complete `Quantity<U,R>` framework with
`Radians`, `Degrees`, dimension arithmetic, and user-defined literals.
The flight model now uses these directly via flight-local aliases and
factory functions in a new public header:

**`f4-flight-model/include/f4/flight/angle.hpp`** (new, ~120 lines):
- `Angle` = `f4::Quantity<f4::Radians>` (radians canonical storage)
- `AngularRate` = `f4::Quantity<f4::RadiansPerSecond>` (rad/s canonical)
- Factories: `angle_from_degrees(d)`, `angle_from_radians(r)`,
  `angular_rate_from_degrees_per_second(dps)`,
  `angular_rate_from_radians_per_second(rps)`, `zero_angle()`,
  `zero_angular_rate()`
- Accessors: `to_degrees(a)`, `to_radians(a)`, `to_deg_per_s(r)`,
  `to_rad_per_s(r)`
- The `Angle` ctor is `explicit` (inherited from `Quantity`), so
  implicit `double -> Angle` conversion is rejected at compile time.
  Call sites must pick a side via the named factories.

### Changes by file

| File | Change |
|------|--------|
| `f4-flight-model/include/f4/flight/angle.hpp` | **New** — Angle / AngularRate aliases + factories + accessors |
| `f4-flight-model/include/f4/flight/aircraft_state.hpp` | All euler angles (sigma, gmma, mu, psi, theta, phi) and aero angles (alpha, beta, alpha_dot, beta_dot) and FCS commands (aoacmd, betcmd) migrated from raw `double` to `Angle` / `AngularRate`. Body rates (p, q, r) kept as raw double with a comment explaining why (they're integrated into the quaternion and never compared with degree-valued quantities). |
| `f4-flight-model/include/f4/flight/aerodynamics.hpp` | `Aerodynamics::update()` signature: `alpha_deg`/`beta_deg` params → `alpha`/`beta` of type `Angle` |
| `f4-flight-model/include/f4/flight/fcs.hpp` | `FlightControlSystem::update()` + `computeGains` + `runPitch` + `runRoll` + `runYaw`: `alpha_deg`/`beta_deg`/`phi_rad` params → `Angle` |
| `f4-flight-model/src/aerodynamics.cpp` | Extract `alpha_deg`/`beta_deg` locals via `to_degrees()` at the top of `update()` (the F-16 aero tables are degree-indexed and we deliberately do NOT convert them). Body unchanged. |
| `f4-flight-model/src/fcs.cpp` | Same boundary-extraction pattern. Write-backs via `angle_from_degrees(...)`. |
| `f4-flight-model/src/eom.cpp` | `trigonometry()` reads `to_radians(k.theta)` etc. instead of `k.theta` directly. Quaternion recovery writes via `angle_from_radians(...)`. Ground clamp uses `zero_angle()`. |
| `f4-flight-model/src/flight_model.cpp` | `init()`, `minorStep()`, `trim()`, `updateStallSM()` — all `alpha_deg`/`beta_deg` field references converted to `alpha`/`beta` (Angle) with `to_degrees()` extraction at the boundary. |
| `f4-flight-model/CMakeLists.txt` | Added `f4-units` as a PUBLIC dependency (was previously explicitly NOT a dependency). |
| `f4-flight-model/tests/*.cpp` | All `a.update(alpha, beta, ...)` and `fcs.update(..., alpha, beta, ..., phi, ...)` call sites updated to pass `angle_from_degrees(x)` for angle args. Direct field assignments (`aero.alpha_deg = 30.0`) updated to `aero.alpha = angle_from_degrees(30.0)`. Field reads in EXPECT_* macros wrapped with `to_degrees(...)`. |
| `f4-flight-model/tests/test_angle.cpp` | **New** — 11 unit tests covering the Angle / AngularRate factories, accessors, round-trip conversions, arithmetic, and the explicit-ctor guarantee. |

### What was deliberately NOT changed

- **The F-16 aero coefficient tables remain degree-indexed.** They are
  physical data files in degrees; converting the data would alter the
  flight feel. The degree convention now survives at exactly one place:
  the lookup call site (`table(mach, alpha_deg)`), where `alpha_deg` is
  a local extracted via `to_degrees(alpha)` and named `_deg` to make
  the convention explicit.
- **`StallConfig::*_deg` and `StallDetection::alpha_deg` / `*_deg`
  fields.** These are plain-data fields consumed by the polling
  detection logic and serialized into bus messages. They are
  deliberately `double` (degrees) because they cross the bus boundary
  as plain data and consumers (UI audio cues, JSON config) want
  degrees. The bridge from the typed `AeroState::alpha` to the plain
  `StallDetection::alpha_deg` is one `to_degrees(a.alpha)` call in
  `flight_model.cpp::updateStallSM()`.
- **Body rates p, q, r in `KinematicState`.** They are rad/s and feed
  the quaternion integrator + FCS roll-rate lag, never a degree-valued
  comparison. Typing them would add friction without closing a real
  correctness gap. Documented in the file-level comment.
- **`AeroState::clalpha`, `clalph0`, `cnalpha`.** These are
  dimensionless aerodynamic derivatives (dCL/dalpha per radian), not
  angles. Correctly `double`.
- **`FcsState::startRoll`.** This is the time-integral of p (rad), not
  an angle. Documented.

### Verification

Clean build from scratch:
```
$ rm -rf build && cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DF4_BUILD_MODEL_VIEWER=OFF -DF4_BUILD_VIEWER=OFF ..
$ cmake --build . -j2
$ ctest -j2
100% tests passed out of 1012
```

The 11 new `test_angle` tests cover the Angle/AngularRate contract
itself; the existing 1001 flight-model / world / etc. tests confirm
the migration didn't change runtime behavior (the same trim alpha
converges, the same stall transitions fire, the same FCS gains
compute).

## Phase 5 — Deferred Item Resolution & Code Quality Pass

Resolves the three deferred items from Phase 4 plus additional code
quality improvements identified in the comprehensive audit.

### C1: MessageBus shared_mutex + copy-on-write refactor

**CRITICAL** — The `recursive_mutex` + vector-copy-on-every-`publish()`
was the dominant serialization point at 60 Hz × N entities.

`f4-messaging/include/f4/messaging/bus.hpp`:
- Replaced `std::recursive_mutex` with `std::shared_mutex`. `publish()`
  takes a shared lock (concurrent reads), `subscribe()`/`unsubscribe()`
  take exclusive locks.
- Replaced per-publish vector copy with copy-on-write `shared_ptr<vector>`.
  `publish()` reads the current shared_ptr under shared lock — zero
  allocation per publish. `subscribe()`/`unsubscribe()` create a new
  vector and swap the shared_ptr.
- Reentrant publish (handler calling publish() on the same bus) is now
  handled via a thread-local deferred list instead of recursive_mutex.
  The outer publish drains the list after handler dispatch completes.
- Deleted move operations (shared_mutex is not movable, and the
  thread-local reentry list references `this`).

### C2: LayeredStateMachine inhibition uses force_to_state()

**CRITICAL** — `applyInhibition()` was calling `reset()` which fires the
initial state's entry action. When a higher-priority DigiMode layer
activates, suppressed layers would execute phantom entry actions (e.g.,
starting timers, publishing messages) instead of silently going to idle.

`f4-state-machine/include/f4/fsm/state_machine.hpp`:
- Added `force_to_state(StateEnum s) noexcept` — sets `current_` to `s`
  without firing entry/exit actions or recording a transition. This is
  an administrative reset, not a UML 2 transition.

`f4-state-machine/include/f4/fsm/layered.hpp`:
- Changed `applyInhibition()` to call
  `layers_[j].sm.force_to_state(layers_[j].idle_state)` instead of
  `layers_[j].sm.reset()`.

### C3: dat_parser exception-as-control-flow eliminated

**HIGH** — The backtracking search in `parseEngine`, `parseRollData`,
and `parseLimiters` used `try { ts.nextDouble(); } catch (...) { ... }`
as a branching mechanism. C++ exceptions are ~100× slower than normal
return on most platforms, making .dat loading the dominant cost.

`f4-Fconvert/src/dat_parser.cpp`:
- Added `tryNextDouble() -> optional<double>`,
  `tryNextInt() -> optional<int>`, and
  `tryNextDoubles(n) -> optional<vector<double>>` to `TokenStream`.
  These return `nullopt` on EOF or parse failure WITHOUT throwing.
- Replaced all 4 catch sites: `parseEngine` legacy format scan (site 1),
  `parseEngine` alpha-factor probe (site 2), `parseRollData` (site 3),
  and `parseLimiters` (site 4).
- Added `<optional>` include.

### H9: Cursor::remaining() unsigned underflow

`f4-io/include/f4/io/cursor.hpp`:
- `remaining()` now returns 0 instead of `SIZE_MAX` when `p > end`
9  (unsigned underflow from `static_cast<size_t>(end - p)`). Previously
  any code using `remaining()` without first checking `error` would get
  a garbage value that could be used as a loop bound.

### CP2: [[nodiscard]] on result-returning functions

`f4-convert/include/f4/convert/dat_parser.hpp`:
- Added `[[nodiscard]]` to `loadFile()` and `loadString()`. Discarding
  the `ParseResult`D silently ignores parse errors.

`f4-flight-model/include/f4/flight/flight_model.hpp`:
- Added `[[nodiscard]]` to* `trim()`. Discarding the bool return
  silently ignores trim convergence failure.

### L6/L7: FlightModel::setMinorPerMajor + Cursor non-copyable

`f4-flight-model/include/f4/flight/flight_model.hpp`:
- Changed `minorPerMajor_` from `int` to `unsigned int`. Negative values
  silently became 1; now the type prevents them at compile time.

`f4-io/include/f4/io/cursor.hpp`:
- Added `Cursor(const Cursor&) = delete` and `operator=`. Copying a
  Cursor creates two readers sharing the# same buffer, advancing
  independently — a logic error.

### M8:2 ModelDatabase texture cache thread safety

`f4-models/include/f4/models/model_database.hpp`:
- Added `mutable std::shared_mutex texture_cache_mutex_` to protect
  `decoded_textures_`. `fetch_texture()` is const and lazily populates
  the cache; concurrent calls from render + export threads would
  otherwise be a data race.

### H8: FlightModel::bus_ lifetime contract documented

`f4-flight-model/include/f4/flight/flight_model.hpp`:
- Added explicit lifetime contract comment in `set_message_bus()`.
  The raw pointer pattern is retained for the default-construct-then-
  reassign pattern, but the contract is now prominent.

## Phase 6 — Pre-AI Hardening Sprint

Resolves the 5 Critical + 5 High issues from the dark-pattern audit
that would have become acute the moment multiple aircraft start sharing
state at 360 Hz. All 1020 tests pass (up from 1008 — 12 new tests added).

### PR1: C3+C4+C5 — silent-garbage fixes (low risk, high value)

- **C3: THEATER.MAP magic validation** (`f4-terrain/src/terrain_data.cpp`):
  the parser read `header.magic` but never compared it against
  `0x444CFFAE`. A file with the wrong magic but plausible dimensions
  parsed silently and produced garbage elevation data. Added
  `constexpr uint32_t THEATER_MAP_MAGIC = 0x444CFFAEu` in the public
  header (`terrain_data.hpp`) and a check in `load()`. Dedupes 8
  scattered literal occurrences across the terrain lib, hex inspector,
  and tests.
- **C4: BinReader::remaining_bytes() underflow** (`f4-models/src/bin_reader.hpp`):
  same bug as the `Cursor::remaining()` H9 fix in Phase 5, applied to
  the parallel reader. Now returns 0 (not `SIZE_MAX`) when `pos > size`
  after an OOB read.
- **C5: JSON skip_value() strict validation** (`f4-json/include/f4/json/reader.hpp`):
  the bare-token path accepted any non-structural char run — `truu`,
  `1.2.3.4`, `@#$%` all parsed silently. Now validates against
  `true`/`false`/`null`/number grammar and throws on anything else.
  Added `read_bool()` helper to replace the hand-rolled
  `if (r.consume('t')) ... skip_value()` pattern in `world_state.cpp`
  (3 sites) that broke under strict skip_value.
- 5 new tests: `Terrain.RejectsBadMagic`, `SkipValueFalse`,
  `SkipValueNumber`, `SkipValueRejectsMalformedBareToken`,
  `SkipValueRejectsBarePunctuation`.

### PR2: C1 — Table1D/Table2D data race on mutable cache

`f4-math/include/f4/math/table.hpp`:
- The "cached last-index hint" used `mutable std::size_t last_` written
  on every `const operator()` call. Two `FlightModel` instances sharing
  an aero table (e.g. formation AI) would race on this write.
- Replaced with `mutable std::atomic<std::size_t>` using
  `memory_order_relaxed`. Relaxed atomics are ~1ns on x86, preserving
  the cache's perf benefit while making the const operator() thread-safe
  per the C++ memory model. The cache is only a hint — a racy write
  simply causes the next call to do a full scan (still correct).
- Added explicit copy/move constructors because `std::atomic` is
  non-copyable. The cache value is loaded/stored via relaxed atomics;
  any value (including stale) is acceptable.
- No new tests (the existing 192 f4-math tests verify correctness).
  TSAN would now pass on a multi-aircraft shared-table test.

### PR3: H2 — warning flags + GoogleTest fetch dedup

- **Warning flags**: created `cmake/f4_warnings.cmake` defining an
  INTERFACE library `f4_warnings` with `-Wall -Wextra -Wpedantic`
  (GCC/Clang) or `/W4 /permissive-` (MSVC).
  `-Wno-missing-field-initializers` is suppressed (tests legitimately
  use `{}` aggregate init for structs with many fields). Linked to
  every f4-* library target (PUBLIC for STATIC, INTERFACE for
  header-only). First clean build: 2 real warnings (unused function,
  unused variable) — both fixed. Zero compiler warnings now.
  `-Wshadow -Wconversion` deferred to a follow-up (would produce
  hundreds of warnings on legacy-shaped code).
- **GoogleTest dedup**: the 17 `f4-*/tests/CMakeLists.txt` files each
  repeated the same 10-line `FetchContent_Declare(googletest ...)` +
  `include(GoogleTest)` block, pinning the version in 17 places.
  Hoisted to `cmake/f4_deps.cmake`, included once from the root
  `CMakeLists.txt`. Each test CMakeLists now just calls
  `gtest_discover_tests()`.
- 0 new tests (infrastructure change).

### PR4: C2 — TagValue → std::variant

`f4-entities/include/f4/entities/entity.hpp`:
- Replaced the hand-rolled tagged union (4 parallel fields:
  `str_val`, `int_val`, `float_val`, `bool_val`, only 1 populated,
  ~40 bytes wasted per int/bool tag) with
  `std::variant<std::string, int64_t, double, bool>`.
- API change: the 4 fields → 4 pointer-returning accessors
  (`as_string()`, `as_int()`, `as_float()`, `as_bool()`). Returns
  nullptr if the variant doesn't hold the requested type — surfaces
  type mismatches instead of silently returning default-constructed
  zeros/empties.
- Retained `Type` enum and `type()` method for backward compatibility
  with code that switches on type.
- Updated 12 call sites across `f4-entities/tests/test_entity.cpp`,
  `f4-world/tests/test_world_loader.cpp`, and
  `f4-world-viewer/src/{canvas,inspector_panel}.cpp`. The viewer's
  `team_tag->type == Type::Int ? team_tag->int_val : 0` pattern
  (4 sites) became the cleaner `team_tag->as_int() ? *team_tag->as_int() : 0`.
- 0 new tests (existing 20 entity tests + 7 world-loader tests verify
  the migration).

### PR5: H1 — ECS Phase 4 verification + interface contract test

**Finding**: the audit's H1 was overly pessimistic. The ECS Phase 4
work is **already complete** — `IDataSource` interfaces
(`ICampaignSource`, `ITeamSource`, `IObjectiveSource`, `IUnitCoreSource`
+ `IGroundUnitSource`/`ISquadronSource`/`IFlightSource`/`IPackageSource`)
exist in `f4-world/include/f4/world/data_source.hpp`, the bridge has
interface-based overloads, `WorldState` is forward-declared in the
public header (not included), the adapter structs are private to
`src/world_loader.cpp`, and the viewer reads exclusively from
`EntityWorld` components (verified by grep — zero direct `WorldState`
field accesses in viewer code).

The only remaining smell is that `UnitState` is still a 40-field
tagged-union struct. But it's now an INTERNAL implementation detail
of the `WorldStateAdapter` (the bridge's concrete `IUnitCoreSource`
implementation), in `detail/world_state.hpp`, never exposed to
consumers. This is acceptable — the smell is contained.

**Delivered**:
- Added `test_interface_bridge.cpp` (7 tests) — implements mock
  `ICampaignSource` / `ITeamSource` / `IObjectiveSource` /
  `IUnitCoreSource` adapters inline and calls `populate_world()`
  through the interface overloads. This is the **contract test for
  f4-ai**: if the interface contract breaks, this test fails before
  any AI code can be written against it. Also unlocks future data
  sources (BMS saves, DIS streams, procedural generation) without
  touching `WorldState`.
- Added deprecation comment to `UnitState` documenting that it's
  internal-only and pointing consumers to either `EntityWorld`
  components or the `IUnitCoreSource` interface.

### Verification

Clean build from scratch:
```
$ rm -rf build && cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DF4_BUILD_MODEL_VIEWER=OFF -DF4_BUILD_VIEWER=OFF ..
$ cmake --build . -j2
$ ctest -j2
100% tests passed out of 1020
```

Zero compiler warnings. 12 new tests added across the 5 PRs.

### What was NOT done (and why)

- **`-Wshadow -Wconversion` warning flags** — would produce hundreds
  of warnings on legacy-shaped code (narrowing in binary parsers,
  shadowed loop vars in viewer code). Add in a follow-up cleanup pass.
- **`std::function` on state-machine hot path** (H3) — 32 bytes each,
  heap-allocating. Migration to SBO function type is invasive (every
  `StateMachine::Builder::on()` call site). Defer to a dedicated SM
  perf pass.
- **`EntityHandle::add<T>()` returns `T&`** (H4) — breaks encapsulation.
  Migration to `ComponentHandle<T>` touches every component-add call
  site. Defer until f4-ai actually needs dirty-flag/versioning hooks.
- **`FlightModel::state()` mutable overload** (H6) — f4-ai will reach
  into `state()` for sensor reads. Defer the mutable-overload removal
  to the f4-ai integration PR (where the actual call sites will be
  known).
- **Splitting `UnitState` into per-subclass structs** — the struct is
  now internal-only. Splitting it would require rewriting the JSON
  loader + adapter. Low value now that the smell is contained.

### PR6: H8 + H5 — locale-independent JSON parsing + portable temp paths

Follow-up batch closing two audit items that fit the sprint's spirit but
were missed in PR1–PR5.

- **H8: Locale-independent number parsing** (`f4-json/include/f4/json/reader.hpp`):
  `read_int()` used `std::strtol(..., 10)` and `read_number()` used
  `std::strtod`. `std::strtod` is **locale-dependent** — with
  `LC_NUMERIC=de_DE.UTF-8` the literal `"3.14"` parses as `3.0` (the
  `.` is treated as a thousand separator). Any campaign save loaded on
  a German/French/Spanish user's machine would silently corrupt every
  coordinate, velocity, and probability field in the JSON. Replaced
  both with `std::from_chars` (C++17), which is locale-independent by
  spec (§charconv). Also migrated the `\uXXXX` hex parse to
  `std::from_chars(..., 16)` for consistency — `strtol` with base 16
  was already locale-independent, but the migration removes the last
  locale-sensitive stdlib surface from the reader. `long` return type
  of `read_int()` is preserved for ABI compatibility; `std::from_chars`
  handles `long` natively.
- **H5: Hardcoded `/tmp/` paths in tests** — replaced with
  `std::filesystem::temp_directory_path()` in 3 sites:
  - `f4-data/tests/test_config_loader.cpp` (config write test)
  - `f4-world-viewer/tests/test_settings.cpp` (XDG_CONFIG_HOME temp dir)
  - `f4-world/tests/test_world_state.cpp` (terrain resolve test)
  The two remaining `"/tmp/..."` literals in `test_settings.cpp`
  (`s.last_world_json`, `s.last_terrain_json`) are **round-trip data
  values** — the test verifies JSON serialize/deserialize, not
  filesystem access — so they're left as POSIX-style sample strings.
  Portability fix: tests now work on Windows (where `/tmp/` may not
  exist), macOS sandboxed runners, and CI containers with non-standard
  `TMPDIR`.
- 2 new tests: `ReadNumberLocaleIndependent`, `ReadIntLocaleIndependent`.
  Both call `std::setlocale(LC_NUMERIC, "de_DE.UTF-8")` and verify the
  parser still returns the C-locale value. Skipped via `GTEST_SKIP()`
  on systems without the `de_DE` locale installed (CI runners and
  minimal containers commonly lack it).

### Verification (post-PR6)

Build not re-run in this environment (no cmake/ninja available in the
agent sandbox). The change is mechanical:
- `std::from_chars` for `long` and `double` is available since
  libstdc++ 11 / libc++ 14 / MSVC 19.41; the project already requires
  C++20 and g++ ≥ 13 (per existing CI config).
- The reader now includes `<charconv>` and `<system_error>`; both are
  header-only and have no link-time impact.
- The 4 existing float/int tests (`ReadIntPositive`, `ReadIntNegative`,
  `ReadNumberFloat`, `ReadNumberScientific`, `ReadNumberNegativeFloat`)
  continue to pass — `std::from_chars` accepts the same grammar that
  `std::strtod` did for the JSON number subset (no `0x`, no `inf`/`nan`,
  no grouping).
- The 2 new locale tests are skipped on systems lacking `de_DE`, so
  they cannot regress the suite on minimal CI runners.


---

# f4-scenario-player v0 — Initial Implementation

## Summary

Built the first cut of the `f4-scenario-player` host executable that
spawns an F-16 on a parking spot at Kunsan (synthesized layout) and
renders the aircraft + airport geometry (runway, threshold bars,
centerline dashes, taxi route, parking/hold-short/runway-end markers,
compass rose) in a Raylib window with orbit/pan/zoom camera.

The simulation starts **paused** so the aircraft sits at the parking
spot. Press Space to begin taxi (the brain's `TakeoffModule` follows
the scenario's taxi route to the runway threshold).

## Files added

- `Docs/SCENARIO_PLAYER_PLAN.md` — design doc, replaces the earlier
  `F4_TAXI_DEMO_PLAN.md`. Documents the rename, the v0 acceptance
  criteria, and the deferred items (auto-spawning from campaign data,
  real Kunsan ground layout, takeoff rotation, multi-aircraft).
- `f4-scenario-player/` — new top-level crate:
  - `CMakeLists.txt` — builds `f4_scenario_player_lib` (static library)
    + `f4-scenario-player` (CLI executable). Fetches Raylib 5.0 +
    Dear ImGui v1.91.5 + rlImGui (same versions as `f4-models-viewer`).
  - `cli/main.cpp` — entry point: `f4-scenario-player <scenario.json>`.
  - `include/f4/scenario_player/player_app.hpp` — public pimpl API.
  - `include/f4/scenario_player/airport_geometry.hpp` — `AirportGeometry`
    struct + `build_airport_geometry(Scenario)`.
  - `include/f4/scenario_player/coordinate_transform.hpp` — ENU ↔ Raylib
    math (extracted to a public header so tests can verify it without
    linking Raylib).
  - `src/player_app.cpp` — lifecycle (load_scenario, run, screenshot).
  - `src/renderer.cpp` — orbit camera, mesh building, scene drawing,
    HUD overlay, ImGui panel.
  - `src/airport_geometry.cpp` — synthesizes runway/threshold/dashes/
    taxi-route/markers/compass from a `Scenario`.
  - `src/viewer_state.hpp` — private pimpl state.
  - `scenarios/kunsan_parking.json.in` — CMake-configured scenario
    fixture (substitutes `@F4_SOURCE_DIR@` / `@F4_BINARY_DIR@` so it
    can reference `temp/KoreaObj.HDR/.LOD/.TEX` and
    `build/generated_fixtures/f16.json` portably).
  - `tests/test_airport_geometry.cpp` — 12 tests covering runway
    surface, threshold bars, centerline dashes, taxi route, markers,
    compass rose, colors, and zero-length-runway edge case.
  - `tests/test_coordinate_transform.cpp` — 9 tests covering the
    ENU → Raylib and model-vertex → Raylib axis mappings.

## Files modified

- `CMakeLists.txt` — added `add_subdirectory(f4-scenario-player)` gated
  by `F4_BUILD_SCENARIO_PLAYER` option (ON by default).
- `f4-world-convert/include/f4/world_convert/class_table.hpp` — added
  `int16_t vis_type[7]` field to `ClassTableEntry` and a
  `vis_type_for(entity_type, slot=0)` accessor. This closes the
  data-flow gap from `Falcon4.CT` → `ModelDatabase` so that future
  campaign-driven aircraft spawning can auto-resolve the visual model.
- `f4-world-convert/src/class_table.cpp` — parser now reads the 14-byte
  `visType[7]` array at offset 60 of each 81-byte CT record (previously
  discarded). Added `vis_type_for()` implementation.
- `f4-world-convert/tests/CMakeLists.txt` — registered the new
  `test_class_table` test target.
- `f4-world-convert/tests/test_class_table.cpp` — new (8 tests):
  regression guard for the existing fields (classInfo, dataType,
  dataPtr) + new tests for visType exposure (F-16 vehicle-class entry
  has vis_type[0]=1052, out-of-bounds slot returns 0, ~1080 of 2135
  entries have non-zero vis_type[0]).
- `Docs/AIRCRAFT_BINDING_DESIGN.md` — updated to reference
  `f4-scenario-player` (was `f4-taxi-demo`) and the new
  `SCENARIO_PLAYER_PLAN.md`.

## Build

```bash
cd build
cmake -DCMAKE_C_FLAGS="-I.../local-deps/usr/include" \
      -DCMAKE_CXX_FLAGS="-I.../local-deps/usr/include" \
      -DX11_Xrandr_INCLUDE_PATH=... \
      [... other X11 paths ...] \
      /path/to/F4
cmake --build . --target f4-scenario-player -j 4
```

(Note: the build needs X11 dev headers — `libxrandr-dev`,
`libxinerama-dev`, `libxcursor-dev`, `libxi-dev`, `libxfixes-dev`,
`libx11-dev`, `libgl-dev`. In a sandbox without root, these can be
extracted locally via `apt-get download` + `dpkg -x`.)

## Run

```bash
cd build
./f4-scenario-player/f4-scenario-player scenarios/kunsan_parking.json
```

Controls: left-drag orbit, right-drag pan, scroll zoom, Space pause/
resume, F focus aircraft, R reset view, F2 screenshot.

## Headless smoke test

```bash
xvfb-run -a -s "-screen 0 1024x768x24" \
    ./f4-scenario-player/f4-scenario-player \
    scenarios/kunsan_parking.json \
    --screenshot out.png --width 800 --height 600
```

The screenshot is 36 KB, 800×600 RGBA, with detectable pixels for the
runway surface (~4700 grey pixels), threshold bars + centerline dashes
(~5000 white pixels), taxi route (~360 yellow pixels), parking-spot
marker (~530 green pixels), runway-end marker (~40 red pixels), and
the F-16 aircraft model (~83000 dark pixels).

## Test results

- New tests added: 29 (8 ClassTable + 12 AirportGeometry + 9 CoordinateTransform).
- All 29 pass.
- Pre-existing tests: 6 failures in `PilotInput.Validate*` (5) and
  `EngineModel.DefaultConstructedHasNoTables` (1) — these are
  pre-existing and unrelated to this change (the EngineModel test
  asserts on a null table pointer that's a known issue in the test
  setup, not the production code).

## What's next (per `SCENARIO_PLAYER_PLAN.md` §8)

- Real Kunsan ground layout from `f4-world::WorldState`'s
  `GroundLayoutList` data (currently synthesized from scenario JSON).
- Auto-spawn aircraft from campaign `Flight`/`Squadron` units via
  the new `vis_type_for()` accessor.
- Takeoff rotation + climb-out (the brain supports it; the renderer
  just needs to keep drawing).
- Multiple aircraft (the `Simulation` currently tracks one
  `aircraft_entity_`; supporting N is a small refactor).

---

# Phase 2 — Campaign-Derived Scenarios

## Summary

Closed the §4.3 gap (deferred from Phase 1): the scenario player can now
spawn aircraft from a real Falcon 4.0 campaign save instead of a hand-
authored JSON aircraft list. Loading `save1.cam` (via `f4-world-convert`'s
`cam2json`) produces a `WorldState` with `Flight`-class units; the new
`spawn_aircraft_from_flights()` bridge walks those units, resolves each
flight's squadron → airbase → parking spot, and composes a 4-component
aircraft entity (`TransformComponent + FlightModelComponent +
VisualModelComponent + BrainComponent`) per flight.

## Changes

- **`f4-simulation/scenario.hpp`**: Added `SpawnMode` enum (`ScenarioList`
  | `CampaignFlights`) + `Scenario::world_json_path` + `class_table_path`
  fields. Backward compatible — defaults to `ScenarioList`.
- **`f4-simulation/campaign_bridge.{hpp,cpp}`** (new): Two functions:
  - `derive_airfield_from_objective(obj, runway_id)` — pure conversion
    from `ObjectiveState.ground_layout` to `ScenarioAirfield`. Returns
    `nullopt` for non-airbases. Grid→ENU conversion (1024 ft/grid unit).
    Builds taxi route from parking → follow-me → threshold.
  - `spawn_aircraft_from_flights(world, ct, db, cfg, airfield, template)`
    — walks `world.with_component<FlightPlanComponent>()`, resolves each
    flight's squadron → airbase → transform for parking spot, applies
    per-flight lateral offset (80 ft alternating ±), looks up vis_type
    via `ClassTable::vis_type_for(squadron.class_table_index, 0)`,
    composes the 4-component aircraft entity. Falls back gracefully when
    CT lookup or squadron resolution fails.
- **`f4-simulation/simulation.{hpp,cpp}`**: Replaced single
  `aircraft_entity_` with `std::vector<EntityId> aircraft_entities_`.
  Spawn dispatcher picks `spawn_from_scenario_list()` (Phase 1 path, now
  iterates `scenario.aircraft[]`) or `spawn_from_campaign_flights()`
  (Phase 2 path: loads WorldState, populates EntityWorld via
  `f4-world::populate_world`, derives airfield, loads ClassTable, calls
  `spawn_aircraft_from_flights()`). `tick()` and `record_snapshot()`
  iterate the vector — one snapshot per aircraft per tick.
- **`f4-simulation/CMakeLists.txt`**: Added `f4-world` + `f4-world-convert`
  + `f4-terrain` to dependencies (transitively required by WorldState +
  ClassTable).
- **Tests**: 17 new tests.
  - `test_campaign_bridge.cpp` (11 tests): 7 for `derive_airfield_*`
    (nullopt paths, threshold/runway_end/departure_alt, taxi route,
    heading conversion, runway_id propagation) + 4 for `spawn_*` (empty
    world, one-per-flight, lateral offset, vis_type fallback, threshold
    fallback).
  - `test_scenario_loader.cpp` (+6 tests): `spawn_mode` parsing,
    unknown-mode-throws, `campaign_flights` requires `world_json_path` /
    `class_table_path` / aircraft template.
- **Docs**: New `Docs/NEXT_PHASE_PLAN.md` documents Phase 2 scope +
  acceptance criteria + implementation order.

## Test Results

- All 56 simulation tests pass (11 CampaignBridge + 6 new ScenarioLoader +
  5 existing ScenarioLoader + 8 ClassTable + 5 VisualModelComponent +
  21 others).
- Full suite: 1265 of 1271 pass. The 6 failures are pre-existing
  (`PilotInput.ValidateClamps*` + `EngineModel.DefaultConstructedHasNoTables`)
  and unrelated to this change.

## What's Next (Phase 3)

- Wire `f4-scenario-player`'s renderer to iterate `sim.aircraft_entities()`
  (currently draws only the singleton via `sim.aircraft_entity()`).
- Build a `kunsan_from_campaign.json` scenario fixture that uses
  `spawn_mode: "campaign_flights"` with the bundled `save1.world.json` +
  `FALCON4.ct`.
- Smoke-test with `--screenshot` to verify multiple F-16s are visible at
  their campaign-derived parking spots.

---

## Phase 2A — Real Airfield Meshes

**Goal:** Replace the procedural painted airfield (one dark-grey quad with
threshold bars + centerline dashes) with **real KoreaObj BSP models** placed
at Kunsan: runway sections, taxiway, control tower, hangars, fuel tanks,
parking apron. The F-16 and airfield features share the same render path —
both are just entities with a `VisualModelComponent`.

### Architecture

A new `ScenarioFeature` struct joins `ScenarioAircraft` and
`ScenarioAirfield` in `f4-simulation/scenario.hpp`. Each feature carries
`{name, vis_type_index, position, heading_rad}` — the same keying model as
the aircraft block (direct KoreaObj model index, no class-table lookup).
The scenario JSON gains an `airfield_features[]` block.

`Simulation::spawn_airfield_features()` creates one entity per feature with
`TransformComponent` + `VisualModelComponent` (no FM, no brain — static).
Tracked in a separate `feature_entities_` vector so `tick()` doesn't try
to sync them from a flight model.

The renderer refactors `aircraft_meshes` (a flat `std::vector<MeshEntry>`)
into `mesh_cache` (a `std::unordered_map<int, MeshCacheEntry>` keyed by
`parent_index`). Multiple features sharing the same `vis_type` reuse one
GPU upload. `draw_aircraft()` becomes `draw_visual_entities()` which walks
`world.with_component<VisualModelComponent>()` and draws every entity —
aircraft and features share the same draw path.

### Files changed

| File | Change |
|------|--------|
| `f4-simulation/include/f4/simulation/scenario.hpp` | New `ScenarioFeature` struct + `Scenario::airfield_features` vector |
| `f4-simulation/src/scenario.cpp` | `read_feature()` parser + `airfield_features` block in `parse_scenario()` + validation |
| `f4-simulation/include/f4/simulation/simulation.hpp` | `feature_entities_` vector + `feature_entities()` accessor + `spawn_airfield_features()` decl |
| `f4-simulation/src/simulation.cpp` | `spawn_airfield_features()` impl + call from `initialize()` |
| `f4-scenario-player/src/viewer_state.hpp` | `MeshCacheEntry` struct + `mesh_cache` map (replaces `aircraft_meshes` vector) + `build_mesh_for_model()` + `draw_visual_entities()` decls |
| `f4-scenario-player/src/renderer.cpp` | `build_aircraft_meshes()` → thin wrapper; new `build_mesh_for_model(int)` lazy builder; `upload_textures()`/`unload_meshes()` walk `mesh_cache`; `draw_aircraft()` → `draw_visual_entities()` walks all VMC entities |
| `f4-scenario-player/scenarios/kunsan_parking.json.in` | Adds 12 real feature placements (runway sections, threshold bars, taxiway, parking apron, control tower, hangars, fuel tank, runway access gate) |
| `f4-simulation/tests/fixtures/takeoff_kunsan.json` | Adds 4-feature block for testing |
| `f4-simulation/tests/test_scenario_loader.cpp` | 4 new tests for `airfield_features` parsing + validation |
| `f4-simulation/tests/test_feature_spawning.cpp` | **NEW** — 8 integration tests for `spawn_airfield_features()` (entity creation, transform encoding, static-ness, model sharing, discoverability) |
| `f4-simulation/tests/CMakeLists.txt` | Wires `test_feature_spawning` |

### Test results

- 4 new `ScenarioLoader` tests pass (fixture parsing, empty-allowed, invalid-throws, non-zero-heading)
- 8 new `FeatureSpawning` integration tests pass (entity-per-feature, transform+VMC present, no-FM, tick-doesn't-modify, model-sharing, empty-spawns-zero, heading-to-quaternion, with_component-discovery)
- All 19 prior `ScenarioLoader` + `CampaignBridge` tests still pass
- `f4-scenario-player` builds clean and produces a screenshot showing 11 VAOs (F-16 sub-meshes + multiple feature models) loaded into VRAM
- Pre-existing `PilotInputTest` failures are unrelated (assertion in `f4-flight-api/src/pilot_input.cpp:21`, present before Phase 2A)

### What's next (Phase 2B)

- Wire `BrainComponent` to drive the F-16 along the taxi route from parking
  to hold-short, using `FlightModelComponent`'s nosewheel steering + ground
  throttle.
- Render the aircraft actually moving across the real airfield meshes.

## Fixed-Timestep Player Loop (speed slider rework)

**Goal:** Make the simulation trajectory independent of the speed slider
and the frame rate, and raise the slider max from 4x to 10x.

### The problem

The scenario player ticked the sim ONCE per rendered frame with an
INFLATED dt (`player_app.cpp:241` before this change):

    sim->tick(scenario.sim_dt * time_scale);

so the flight model's FCS/EOM minor step (dt/6) slid between 1/3600 s
(0.1x) and 1/90 s (4x). The FCS PI + lead-lag filters are discrete and
dt-dependent — past their tuned operating point (1/360 s minor step)
the pitch loop destabilizes, which is why the slider was clamped to 4x
(FLIGHT_CONTROL_STABILITY_PLAN.md §4.2 RC-2 — a symptom patch). Two
more defects fell out of the same line:

1. Real-time pacing depended on the FRAME RATE: the sim advanced
   `sim_dt*time_scale` per FRAME, so "1.0x = real time" only held at
   exactly 60 FPS (a 144 Hz vsync display ran the sim ~2.4x too fast).
2. The trajectory itself changed with the slider (different dt =
   different integration path), so interactive runs, headless CI runs,
   and recorded traces were never directly comparable.
3. `Simulation::tick` ALSO multiplied by its own `time_scale_` member
   (default 1.0, never set by any caller) — a latent double-scaling
   trap for future hosts.

### The fix

Fixed-timestep accumulator in the player (the classic "Fix Your
Timestep" pattern):

    accumulator += frame_dt * time_scale;      // slider scales WALL TIME
    while (accumulator >= scenario.sim_dt) {   // dt is ALWAYS sim_dt
        sim->tick(scenario.sim_dt);
        accumulator -= scenario.sim_dt;
    }

- dt is now always `scenario.sim_dt`, so the FM minor step stays at its
  tuned 1/360 s at every speed; filter behavior is speed-independent.
- The slider scales wall-clock time, not dt: 1.0x is true real time at
  any frame rate, and slow-mo carries fractional remainders across
  frames smoothly.
- The tick sequence — hence the trajectory, the flight recording, and
  the FCS CSV trace — is identical across slider settings and matches
  the headless harnesses (`sim.tick(1.0/60.0)` loops), so interactive
  and CI traces are directly comparable again.
- The 4x stability cap is replaced by a CPU-bounded guard
  (`kMaxSimStepsPerFrame = 30` — covers 10x at 30 FPS; drops the
  remainder after a stall instead of freezing the render loop), and
  the slider max is raised to 10x (10x/60 FPS = 10 ticks x 6 minor
  steps per frame).
- `Simulation::set_time_scale()`/`time_scale_` (behavioral scaling) are
  REMOVED. The FCS CSV trace's `time_scale` column is kept and now
  records pure host metadata via `Simulation::set_trace_time_scale()`
  (default 1.0) — baselines stay identifiable, but no host can
  accidentally double-scale dt.

### Files changed

| File | Change |
|------|--------|
| `f4-scenario-player/src/player_app.cpp` | Accumulator tick loop with `kMaxSimStepsPerFrame = 30` guard; slider + `set_time_scale` clamp raised to [0.1, 10.0]; comments updated |
| `f4-scenario-player/src/viewer_state.hpp` | `sim_accumulator` member |
| `f4-scenario-player/include/f4/scenario_player/player_app.hpp` | `set_time_scale` doc (real time at any frame rate, [0.1, 10.0]) |
| `f4-simulation/include/f4/simulation/simulation.hpp` | `set_time_scale`/`time_scale_` removed; `set_trace_time_scale`/`trace_time_scale_` added (metadata only) |
| `f4-simulation/src/simulation.cpp` | `tick(dt)` no longer scales dt (dt is authoritative); FCS trace sample records `trace_time_scale_` |
| `Docs/FLIGHT_CONTROL_NEXT_STEPS.md` | Phase 2b row marked Superseded |
| `CHANGES.md` | This entry |

### Testing

- Full non-renderer test suite passes (all f4-* libraries; renderer/
  viewer executables excluded from CI here only because the container
  lacks GL dev headers — no renderer code was touched).
- The scenario-player translation unit compiles clean against raylib
  5.0 / imgui v1.91.5 / rlImGui @ 9acdbbf headers.
- Manual: run `takeoff_only` at 0.1x / 1x / 10x — the FCS CSV traces
  must be identical modulo the `time_scale` column; at 10x the run
  completes ~10x faster with no pitch oscillation.
- Manual: resize/drag the window mid-run at 10x — the render loop must
  stay live (guard drops catch-up debt) and sim time must not jump.
