# IR / VISUAL SENSORS + COUNTERMEASURES PLAN

Status: **As-built** (landed with this patch — every section describes
shipped code; §8 is the fusion tranche, landed with its gates and its
perf certificate. §9's queue is empty — the ECM data source landed as
ECM-DATA-1 (the converted tables' jammer bits); what remains of the
deeper tranche is behavior (the AI's notching response), not data).

The AI_IMPLEMENTATION_PLAN's named queue item "IR/visual sensor models +
countermeasures (the data is already in Data/SimData; radar is the
template)", executed. The tranche has three legs:

1. **The passive sensors** — IRST + visual as ECS behavioral components
   in f4-sensors, driven by the SENSDATA cards and the SIGDATA IR/VIS
   grids (all converted since the SimData pipeline landed).
2. **The countermeasures** — dispensers, decoy entities, and the
   seeker-seduction model in f4-weapons: the consumption half of the
   MissileModule's defeat intents (its header documented "no
   countermeasure consumption model exists in f4-weapons yet").
3. **The host wiring** — f4-simulation executes the deploy intents in
   the combat pass, attaches the seduction seeker to every AI-guided
   release, sweeps expired decoys — ALL behind the golden-identity
   gate (§5), because §6's first attempt broke that rule and the
   pinned fights caught it.

---

## 1. The data (already converted; this tranche is its runtime consumer)

| File | Content | Consumer |
|---|---|---|
| `Data/SimData/irstdata.json` | 8 cards: generic (the airframe IRST: az 120/el 60/10 NM/gf 1.0), six IR seeker cards (aim9l 0.2, aim9p 0.4, sa7 0.5, sa14 0.3, sa9 0.4, sa13 0.4 — `flare_chance` is P(seeker takes the flare)), agm65b (gf 2.0, the ground-pounder) | f4-sensors `IrstComponent` (the generic card's shape = the defaults) + f4-weapons' seduction (the seeker cards' `flare_chance`) |
| `Data/SimData/visualdata.json` | 3 cards: generic (eyeball, 181/91, gain 3.7e9), mav, tpod | f4-sensors `VisualComponent` |
| `Data/SimData/sigdata.json` | the generic signature's FIVE grids: rcs (10 m² flat), ir0/ir1/ir2 (the IR bands: baseline 0.05 / afterburner 0.5 / max 3.5-ish, hot-rear aspect lobes), visual (flat 1.0) | `SignatureComponent` extensions (§2) |

Fidelity note (carried from f4-data's sensor_data.hpp): the FreeFalcon
runtime read precompiled binary tables, not these text cards — the exact
original signal math is unrecoverable from the tree. The models below
are documented placeholders SHAPED by the data, the same discipline the
radar detection model flew in on.

## 2. The passive sensors (f4-sensors)

### SignatureComponent extensions

- `sig_data` — non-owning `const AircraftSignatureData*` (all five
  grids; the host's SignatureDataLibrary owns it, exactly like
  `rcs_grid`).
- `ir_power` — `IrPowerMode { Baseline, Afterburner, Max }` selects the
  band; default Afterburner (a fighting aircraft is not cruising).
- `ir_signature_value(aspect_rad)` — the band grid's bilinear lookup,
  **1.0 when no sig_data** (every data-free target reads as the
  reference airframe the sensor cards' nominal ranges were authored
  against — the golden identity at the component level too).
- `visual_signature_value(aspect_rad)` — the VIS grid lookup, 1.0
  data-free. The radar path is untouched (`rcs_grid` stays the radar's
  input).

### PassiveTrackStore (`passive_track.hpp`)

The contact book the passive sensors share: on_detection
(insert-or-refresh + first-seen return), decay(now, hold) returning
dropped ids. No track quality, no NCTR — a scan sees the thing or it
doesn't; what it keeps is "what I saw last and when".

### IrstComponent (priority 45 — the radar's pass)

Scan: candidate walk (`with_component_ref`, the FID-OPT-3 shape) →
range pre-gate (8× nominal) → gimbal gates (|bearing off own heading| ≤
az_limit; |elevation| ≤ el_limit; stationary → azimuth gate off, the
RWR omni contract) → ground-clutter rejection UNLESS
`track_ground_clutter` (the airframe card watches the air picture) →
detection roll.

Detection model:

    R_det = nominal_range_nm * sqrt(ir_sig / reference_ir) * ground_factor

- **sqrt** because IR flux is one-way (power ~ 1/r²) — the radar's
  fourth root is the two-way echo; IR is the emitter itself.
- ground_factor applies to ground-clutter targets (the seeker cards'
  0.001 = an IR missile cannot track a cold parked airframe; the
  generic card's 1.0 = no penalty; the Maverick's 2.0).
- P(detect) = the radar's 0.75-knee ramp. Seeded mt19937 per component
  (the reproducibility discipline).

Publishing: NONE. Contacts are queryable state (`contacts()`, `find()`),
held `contact_hold_s` (3 s) past the last sighting. The radar's
transition messages exist for consumers IRST does not have yet (debrief,
RWR audio); the fusion tranche decides the event surface when a
consumer exists (§8).

### VisualComponent (priority 45)

THE ORIGINAL'S DOCUMENTED SIGNAL LAW (f4-data recorded it from
visual.cpp's threshold comment):

    signal = gain * visual_sig / range_ft^2    —  detect iff >= 1.0

Deterministic: NO RNG (the original is a comparison, not a
probability). Gain 3.7e9 ⇒ threshold at 10.0109 NM (the shipped JSON's
own implied nominal — 3.7e9 is a rounded authoring value). The VIS
grid's size factor scales the range (a 4× bigger transport reads at 2×
the range — one-way inverse square). No clutter rejection: an eyeball
sees the ramp. Same gimbal gates as the IRST.

## 3. The countermeasures (f4-weapons)

### CountermeasureComponent (the dispenser, passive)

Chaff 30 / flare 15 (the documented defaults — CAMP-SCALE-1 converted
them: `combat.theater_tables_path` / `--theater-tables` resolves the
vehicle's own VCD hardpoint supply through the WCD and the arm path
spends the real counts; a vehicle the tables cannot resolve — or no
tables at all — keeps these numbers), salvo 2 chaff / 1 flare,
interval 0.5 s. The interval is the pacemaker: the defeat module raises
its intents every tick of the beam; the dispenser releases one salvo
per interval.

### DecoyComponent + DecoySimComponent (decoys are ENTITIES)

The FreeFalcon model — a flare is a real object with its own id, team,
and lifecycle, exactly like a missile. Behavioral priority **42**: after
the radar pass (45), before the missile sims (40), so a seeker reading
this tick sees this tick's decoy positions. Motion: chaff stalls hard
into the airstream (drag 2.5/s), flares fall (gravity + 0.35/s drag).
Released ~15 ft behind the airframe with a slice of its velocity plus
seeded dispersion; TEAM tag copied (the IFF rule); ROLE "decoy"; ttl
5 s chaff / 3.5 s flare. Signature fields ride the component (chaff
25 m² — dominates a fighter's 10; flare ir_signal 8).

`deploy_countermeasure(world, bus, aircraft, kind, now, seed)` —
validates rounds + interval, debits, spawns the salvo, publishes ONE
`CountermeasureDeployedMessage` per salvo. Refusals (dry, inside the
interval, missing components) change nothing.
`sweep_expired_decoys(world, now)` — the host-side sweep (the missile
sweep's rule: never inside update_all).

### The seduction model (make_decoy_aware_seeker_source)

The documented `MissileComponent::seeker_source` hook's first
production user. Built per missile at launch; state lives in a
shared_ptr (component copies share one book: seduced id, failed rolls,
RNG). Per tick:

1. Seduced decoy sticky while it lives (no re-rolls, no flip-flop);
   burnout/sweep re-arms the seeker.
2. Candidates: right kind for the guidance (flares⇔Ir, chaff⇔
   SemiActive/Active radar), enemy-owned (TEAM ≠ shooter's), alive,
   inside the seeker envelope (the weapon record's own cone half-angle
   + max range, measured off the missile's velocity axis).
3. **One honest roll per decoy** — success transfers the track to the
   decoy; failure is remembered (a 60 Hz re-roll would be certain
   seduction). P = the seeker card's `flare_chance` (IR — the DATA's
   own number per missile class) or `chaff_transfer_p` (radar, 0.5 — a
   tuning constant documented as such until a data source exists).
4. No bait → the direct target read, EXACTLY the M1 snapshot_target
   contract.

### The miss-distance fix (missile_battery.cpp)

Seduction exposed a latent proxy bug: the terminal handler measured
the miss distance as `min_range_` — the closest range to whatever the
seeker TRACKED. When the seeker rides a flare, that is the range to
the FLARE (≈ 0 at the fuze) — a seduced missile would have read as a
direct hit on the aircraft it missed. With `decoy_aware_seeker` set
(the host sets it with the factory), the miss distance is measured
against the ASSIGNED TARGET entity's live position; every legacy path
keeps the byte-identical min_range_ math. This is why seduction SAVES
the jet: the fuze fires at the flare, the burst-to-aircraft distance
is large, the damage model's range falloff does the rest.

## 4. The host wiring (f4-simulation)

- `ScenarioConfig::ir_seeker_data_path` — the irstdata library (loud
  failure when configured and unloadable; empty = the default-flare
  identity). Loaded once with the weapon table; `resolve_ir_seeker_data`
  + `find_ir_seeker_flare_chance` (exact stem → card-prefix → family
  bridges AIM-9*→aim9p, AGM-65*→agm65b → kDefaultIrFlareChance 0.3)
  in combat_bridge.
- The intents pass (`execute_brain_combat_intents`): per aircraft —
  chaff/flare deploy intents (the dispenser paces them), and the
  seduction seeker attached to every guided release (seed derived from
  the shooter/missile ids — the ids are the entropy; deterministic per
  launch).
- The tick: `sweep_expired_decoys` after the missile/bomb sweeps.

## 5. THE GOLDEN IDENTITY GATE (read this before touching the wiring)

`CombatConfig::countermeasures` — default **FALSE**. With it off:

- no dispenser attaches (scenario or campaign arm paths),
- the deploy intents never execute,
- no seeker override attaches,
- no decoy sweep runs.

Every pre-tranche fight is byte-identical — the pinned combat
harnesses (test_combat_integration's merge fight, the BVR/WVR harness
certificates, the transcripts) all run with the default and passed
unchanged. The countermeasure E2E turns the flag ON in its scenario
JSON. The first implementation attached dispensers unconditionally and
six pinned fights failed (the AIM-9 that "never killed the bandit" was
flying against flares — working as designed, landing as WRONG: a
fidelity tranche may not silently re-price every existing
deterministic fight). The gate is the correction.

## 6. Tests (all green)

- `test_irst_component` — the sqrt law, the ground factor, the knee,
  gimbal gates (az/el), clutter rejection + the A/G switch, hot-vs-cold
  signature scaling, hold drops, seeded determinism.
- `test_visual_component` — the signal law at/beyond the threshold,
  the sqrt-of-gain range, size scaling, parked targets visible, gates,
  hold drops, determinism.
- `test_simdata_sensors` (extended) — band selection by IrPowerMode,
  VIS lookup, the data-free 1.0 identity.
- `test_countermeasures` — deploy (debits/salvo/publish/IFF tags),
  interval pacing, dry + clip refusals, missing-component refusal,
  decoy motion (chaff stalls, flares fall), ttl sweep, the seduction
  model (identity with no decoys, cone gating, kind gating, team IFF,
  expiry, stickiness through burnout, one-honest-roll), and the
  integration: a seduced AIM-9M detonates at the flare and the jet
  walks away.
- `test_countermeasure_e2e` — the host chain: the AI fires on its own,
  the release carries the seduction seeker, the victim's RWR lights,
  its brain beams, the dispenser releases under fire (and the shooter
  does NOT dispense — the intents are threat-driven, not lock-driven),
  the shipped irstdata.json flows through
  `ir_seeker_data_path`, the loud-failure discipline holds.
- `test_ecm` (the fusion tranche) — the burn-through model (a live
  enemy pod in the beam blinds the radar; closing the range wins
  through — the same pod that blinds at 5 NM cannot blind at 1 NM;
  friendly/corpse/disabled pods and stern-aspect jammers degrade
  nothing, each pinning the exact pre-ECM track state) and the RWR's
  Jamming warning (rank order Launch < Lock < Jamming < Search,
  transition-published exactly once, corpses stop jamming, a victim's
  own pod never warns itself, the brain-facing lock/launch flags stay
  silent for noise).
- `test_sensor_fidelity` (f4-simulation) — the policy's passive legs
  (a radar-less ownship still classifies its IRST contact visual; the
  radar-only policy keeps visual false — the pre-fusion identity; the
  passive books cannot fabricate a radar track), the attach gates
  (passive components only under `passive_sensors`; the ECM pod needs
  the gate AND the per-aircraft fit), and the ir_power stamp (gate off
  = the Afterburner default stands; gate on = the band matches the
  FM's last-flown throttle through the documented mapping).
- `CampaignInitWarsFast.ArmedWarWithPassiveSensorsHoldsThe60xPreset` —
  the fusion tranche's perf certificate: the generated small war,
  armed (aa_combat), passive sensors on, at the 60x preset — zero
  dilation (the throughput verdict: if the passive scans or the stamp
  collapsed the tick, the gate fires), green, deterministic (two runs,
  identical ledger bytes).

## 7. Cost shape

Off (default): zero new per-tick work (the gate short-circuits the
sweep; no components exist to scan). On: the deploy pass is a component
lookup per armed aircraft per tick; the seduction walks decoys per
missile per tick (decoys are few and short-lived); the sweeps walk
their component sets between ticks. Nothing rides the hot radar/air-
picture walks.

## 8. The fusion tranche (the §9 queue, landed)

Three legs, each behind its own `combat.*` scenario gate (default FALSE
— the §5 golden-identity discipline; every pinned fight runs the
defaults and passed byte-identically):

### Passive-sensor fusion — `combat.passive_sensors`

- **The attach**: `attach_combat_loadout` / `arm_campaign_combat` give
  every armed aircraft an `IrstComponent` + `VisualComponent` (seeded
  and team-stamped like the radar; inert until consumed — the passive
  sensors publish nothing). The campaign session carries the gate
  through `CampaignSessionOptions::passive_sensors`.
- **The policy legs**: `RadarBackedDetectionPolicy` batch-caches the
  ownship's passive components alongside the radar + RWR (the PERF-1
  shape — resolved once per rebuild) and answers the `visual` verdict
  from their contact books. TargetInfo's source vocabulary has one
  passive slot; both sensors fill it. No passive component = the
  lookups miss = the pre-fusion verdict, byte for byte. The radar
  verdict stays radar-only — the IRST book cannot fabricate a radar
  track, so a radar missile still needs the radar.
- **What it buys**: a fighter with a dead radar still sees, and fights,
  what its eye and IRST hold. The FID-OPT-tuned machinery is untouched
  — the policy consumes per-ownship state the shared air picture never
  carries — and the armed war holds the 60x preset with the gate on
  (the perf certificate).

### ECM / jamming — `combat.ecm` + the per-aircraft `"ecm"` fit

- **`EcmComponent`** (f4-sensors, pure state): `jamming_strength` (1.0
  = the reference pod), `burn_through_range_nm` (20), `own_team` (IFF),
  `enabled`. Both must agree — the scenario gate AND the aircraft fit.
  **ECM-DATA-1 landed the campaign face of that contract**: the fit is
  no longer scenario-only — the converted theater tables carry the two
  upstream flag bits (VCD Flags & VEH_HAS_JAMMER 0x10000, WCD Flags &
  WEAP_ECM 0x04 — the EA-6B/EF-111A/F-4G/bomber family + the ALQ-131
  pod), `resolve_vehicle_ecm` (f4-world) resolves them through the
  CT→VCD→WCD chain, the spawn paths stamp an `EcmFitComponent` (the
  CAMP-SCALE-1 supply stamp's sibling), and the arm attaches a pod —
  with the fit's strength/burn-through values — only where the session's
  `ecm` gate AND the stamped fit agree. No fit (the F-16C-shaped
  majority) or gate off → nobody jams, the golden identity. The pod's
  burn-through seeds from its own WCD range_km (the ALQ-131's 30 km ≈
  16.2 NM — the data's own number); built-in jammers keep the documented
  20-NM default; upstream has no real per-unit strength field (the S.G.
  Name[14] hack is deliberately not read), so 1.0 stands.
- **The burn-through model** (`RadarSimComponent::perform_scan`): per
  scan, the live ENEMY pods (friendly team never, corpses never,
  disabled never) resolve to a bearing + weight — one-way noise falls
  with 1/r², saturating inside the pod's burn-through range. Per
  candidate, pods within the scan bar's azimuth half-width of the
  candidate's bearing sum their weights (capped 0.95 — a blanket never
  fully blinds) and the ramp reads the STRETCHED range
  `range / (1 - W)`: the effective detection range degrades; closing
  the range is how the echo wins through. No `EcmComponent` in the
  world = one empty-bucket probe per scan and zero per-candidate
  arithmetic — the pre-ECM RNG stream byte for byte.
- **The RWR hears jammers**: `update_rwr`'s emitter gather gains the
  live pods (a radar/jammer hybrid keeps one record with both flags) and
  `RwrWarningType::Jamming` rides the classification (rank Launch <
  Lock < Jamming < Search; a victim's own pod never warns itself; the
  brain-facing lock/launch flags stay silent for noise — the strobe is
  informational). New Jamming emitters transition-publish on the bus,
  like locks.

### Throttle-driven ir_power — `combat.throttle_ir_power`

- The FM's last-flown throttle selects each active aircraft's IR band
  after update_all (the IRST scanned this tick with last tick's stamp —
  one 16 ms tick of latency, deterministic): `>= 1.05` (the AB detent)
  → ir2 `Max`; `>= 0.6` (mil/high dry) → ir1 `Afterburner`; below →
  ir0 `Baseline`. Gate off = the Afterburner default every pre-tranche
  target reads stands untouched.

## 9. Queue (named, not started)

- ~~**SensorFusion fusion**: fold IRST/visual contacts into the AI's
  target list (the radar-backed policy gains passive legs — a fighter
  with a dead radar still fights). Deliberately deferred: the fusion
  + air-picture machinery is FID-OPT-tuned; it gets its own tranche
  with its own perf certificate.~~ — LANDED (§8,
  `combat.passive_sensors`; the perf certificate is the armed 60x
  war).
- ~~**ECM/jamming**: the radar burn-through model (the RWR hears
  jammers; the radar's detection range degrades).~~ — LANDED (§8,
  `combat.ecm` + the per-aircraft fit). The data source that gated the
  deeper tranche has since landed as **ECM-DATA-1** (the converted
  tables' VCD/WCD jammer bits → `EcmFitComponent` → the arm's double
  gate). **Still open**: the AI's notching response to a Jamming strobe
  (the code half is now unblocked — nothing waits on data) and the
  standoff-jamming arm (AMIS_ECM flights / the package `ecm_id` VU the
  decoder already carries).
- ~~**Throttle-driven ir_power**: the FM's power state drives the
  target's IR band (AB = ir1/ir2, idle = ir0).~~ — LANDED (§8,
  `combat.throttle_ir_power`).
- ~~**VCD countermeasure counts**: the per-unit chaff/flare counts
  replace the documented defaults when the unit-data conversion runs
  (the Tier-3 full-data pass).~~ — LANDED with **CAMP-SCALE-1** (the
  `theater_tables_path` flow — see CAMP_HOST_PLAN.md).
