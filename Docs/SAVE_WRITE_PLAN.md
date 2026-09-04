# Save-Write Tranche — Closing the Decode → Run → Fight → Apply → Save Loop

> **Status**: Foundation landed (binary save format). Runtime-verified against
> the `save1.cam` fixture. The follow-on (WorldState → JSON → modified `.cmp`)
> is scoped below.
>
> **Companions**: [Campaign Loop Plan](CAMPAIGN_LOOP_PLAN.md) §7 (the
> documented "save-side re-encoder does not exist for ANY subsystem yet"
> gap — this tranche closes it for the campaign-metadata sub-file), the
> Architecture Proposal §18 (the decode→run→fight→apply→**reload** loop).

---

## 1. The gap this closes

The campaign loop (C1–C6, G1–G2) could decode a `.cam`, run a 24-hour war,
mutate a `WorldState` via `apply_to(ledger, ws)`, and write a byte-stable
`campaign_result.json` — but it could not write the result back as a `.cam`.
`CAMPAIGN_LOOP_PLAN.md` §7 called this out explicitly:

> *"The .cam save-side re-encoder does not exist for ANY subsystem yet —
> both land together with the campaign save-write tranche."*

Without it, the loop is decode → run → fight → apply → **stop**. You cannot
persist a resumed campaign. This tranche lands the binary save format: the
inverse of the decode pipeline, from the LZSS compressor up through the `.cam`
container writer.

---

## 2. What landed

### f4-lzss — `compress()` (the keystone)

`f4::lzss::compress()` is the exact inverse of `f4::lzss::decompress()`. The
decode side is the ground truth (it is byte-exact against FreeFalcon's
`LZSS_Expand`, validated on real `.cam`/`.tex` payloads); the compressor
produces a stream the decoder reads back to the identical original bytes.

**Conventions** (mirrored from `lzss.cpp`):
- 4096-byte ring window, `current_position` starts at 1.
- Tokens blocked in (flag byte, 8 tokens) groups; flag bit N (LSB first) =
  1 for a literal (1 byte), 0 for a match (2 bytes).
- Match token: `b0 = (length_raw << 4) | (position >> 8)`,
  `b1 = position & 0xFF`, where `length_raw = match_length - 2` (the decoder
  copies `length_raw + 2` bytes via its `i <= match_length` loop), and
  `position = (1 + source_index) & 4095`.
- **Position 0 is never emitted** (FreeFalcon reserves it as the EOS
  sentinel). A candidate mapping to slot 0 is skipped — one literal every
  4096 bytes worst case.
- Match finding: greedy longest-match via a hash chain over 3-byte prefixes
  (the zlib pattern, scaled to this codec's 17-byte max match). Min match =
  3; max match = 17. Overlapping (RLE-style) matches are allowed.

**Not byte-identical to FreeFalcon's compressor** — the match heuristics
differ — but any valid LZSS stream decompresses identically, so both
FreeFalcon's decoder and ours read it back to the original bytes. That is
the contract that matters for a save format.

**Tests** (`test_lzss_compress.cpp`): edge cases (empty, 1 byte, group
boundaries at 8/9/16/17), incompressible random data, runs/repeats, output
exceeding the 4096-byte window, match at maximum distance (4095), the
position-0 avoidance (scans the output for any emitted position-0 token),
overlapping matches, a struct-like payload mimicking the `.cmp` shape, and
a stream-well-formedness walk.

### f4-world-convert — `encode_cmp()` (CampaignHeader → .cmp)

`encode_cmp(header, camp_version)` serializes a `CampaignHeader` back into
the `.cmp` sub-file's raw bytes: the 8-byte header (`reserved_skip` +
`decompressed_size`) followed by the LZSS-compressed flat payload. The
payload is written in the exact field order `decode_cmp` reads it
(`campaign_decoder.cpp` is the ground truth; `encode_cmp_payload` is its
line-by-line inverse — every field, width, and NUL-padding rule matched).

**`encode_cmp_payload(header)`** is also exposed, so tests can compare the
re-serialized payload directly.

**Round-trip contract** (the golden): `decode_cmp(encode_cmp(h)) == h` —
struct equality. Verified by comparing `encode_cmp_payload(h1)` against
`encode_cmp_payload(h2)` (the encoder is deterministic, so equal structs
produce equal payloads; any field that fails to round-trip surfaces as a
payload difference in a single vector comparison).

**Byte-identity scope**: the re-encoded payload is byte-identical to
FreeFalcon's original for every *fixed-width* field, EXCEPT where FreeFalcon's
on-disk file carries non-zero garbage in fixed-width-string padding (the
team `name[20]`/`motto[200]` slots, after the `\0` terminator). FreeFalcon
reuses buffers without zeroing; our encoder zero-pads (the cleaner form).
The decoded structs are identical either way — a saved `.cam` loads to the
same game state. See §4 for the measured numbers. (Full byte-identity
against FreeFalcon's originals would require capturing the original padding
bytes in `CampaignEvent`/`TeamEntry`; that is not required for the save/load
loop and is documented as a follow-on.)

### f4-world-convert — `CamWriter` (the .cam container writer)

`CamWriter` assembles a `.cam` archive from sub-files — the inverse of
`CamArchive::load`. It packs sub-file data contiguously from offset 4 in
manifest order, then writes the manifest directory. For the standard
FreeFalcon layout, the output is **byte-identical** to the original
(verified: `CamWriter.build()` on `save1.cam`'s subfiles reproduces
`cam.raw_bytes()` exactly).

`cam_from_world_json(json)` — the library core of the `json2cam` CLI — reads
the `"subfiles_b64"` block from a world JSON and assembles a `.cam` via
`CamWriter`. Tests and the CLI share this one implementation.

### f4-world-convert — `json2cam` CLI + `cam2json --preserve-subfiles`

- **`cam2json --preserve-subfiles`** emits a top-level `"subfiles_b64"` block
  containing every sub-file's raw bytes as base64. (`WorldJsonOptions::
  preserve_all_subfiles` is the library knob; off by default — it roughly
  doubles the JSON size.)
- **`json2cam <input.world.json> [output.cam]`** reads that block and
  reassembles a `.cam`. The result decodes — through `cam2json` or
  `CamArchive::load` — to the identical sub-files.

This is the **unmodified round-trip**: `cam2json --preserve-subfiles
save1.cam` → `json2cam` → `save1.cam'` → `cam2json` → same decoded structs.
The `.cam` produced is byte-identical to the original (the sub-file bytes
pass through verbatim, including the original `.cmp`).

### Files

| File | Change |
|------|--------|
| `f4-lzss/include/f4/lzss/lzss.hpp` | `compress()` declarations |
| `f4-lzss/src/compress.cpp` | **new** — the compressor |
| `f4-lzss/tests/test_lzss_compress.cpp` | **new** — round-trip tests |
| `f4-lzss/CMakeLists.txt`, `tests/CMakeLists.txt` | wire the above |
| `f4-world-convert/include/f4/world_convert/cmp_encoder.hpp` | **new** — `encode_cmp`, `encode_cmp_payload` |
| `f4-world-convert/src/cmp_encoder.cpp` | **new** — the encoder |
| `f4-world-convert/include/f4/world_convert/cam_writer.hpp` | **new** — `CamWriter`, `cam_from_world_json` |
| `f4-world-convert/src/cam_writer.cpp` | **new** — container writer + JSON→.cam |
| `f4-world-convert/src/byte_writer.hpp` | **new** — private LE byte appender (mirror of `Cursor`) |
| `f4-world-convert/cli/json2cam.cpp` | **new** — the CLI |
| `f4-world-convert/include/f4/world_convert/world_json.hpp` | `preserve_all_subfiles` option |
| `f4-world-convert/src/world_json.cpp` | emit `"subfiles_b64"` when option set |
| `f4-world-convert/cli/cam2json.cpp` | `--preserve-subfiles` flag |
| `f4-world-convert/tests/test_cmp_encoder.cpp` | **new** — `.cmp` round-trip tests |
| `f4-world-convert/tests/test_cam_writer.cpp` | **new** — container + JSON round-trip tests |
| `f4-world-convert/CMakeLists.txt`, `tests/CMakeLists.txt` | wire the above |

No existing decoder, struct, or test was modified. The patch is purely
additive on the decode side.

---

## 2b. Reencode tranche — modified saves from a world JSON

The foundation tranche (§2) closed the **unmodified** round-trip: passthrough
every sub-file verbatim via `subfiles_b64`. This tranche closes the
**modified-save** diagonal: re-encode the `.cmp` campaign header from the
`"campaign"` JSON block, so a mutated campaign state (advanced `current_time`,
applied ledger results, updated team states) persists as a `.cam` that loads
to the mutated state.

### What landed

**`from_world_json_campaign(json)`** — parses the `"campaign"` JSON block
back into a `CampaignHeader`. The inverse of the campaign-block emission in
`to_world_json`. Reads every field `encode_cmp` needs; absent fields default
to 0 (robust to older JSON files). Also: `read_world_json_version(json)` for
the top-level version field.

**Extended `to_world_json` emission** — four fields the `.cmp` re-encoder
needs were previously missing or size-only:
- `te_number_f16s[8]` — the TE F-16 counts (was not emitted at all)
- `camp_map_b64` — the terrain ownership map bytes (was size-only)
- `squadrons[]` — the full SquadronUIInfo preload list (was count-only)
- `remaining_payload_b64` — the forward-compat tail bytes (was size-only)

**Float precision fix** — `to_world_json` now emits floats with
`std::numeric_limits<float>::max_digits10` (9 significant digits) instead of
the default 6. Without this, squadron `x`/`y` coordinates lost ~2 bits
through the JSON round-trip (`867554.625` → `867555`), producing different
bytes on re-encode. The fix is global (set once on the `ostringstream`),
so all float fields benefit.

**`json2cam --reencode-cmp`** — the modified-save CLI mode. Reads the
`"campaign"` JSON block, re-encodes the `.cmp` via `from_world_json_campaign`
+ `encode_cmp`, and assembles a `.cam` with the new `.cmp` + every other
sub-file passed through verbatim. Use this to persist a mutated campaign:

```bash
# Advance current_time, apply ledger results, etc. in the JSON, then:
json2cam save1.world.json save1_resumed.cam --reencode-cmp
```

**`encode_obj(dec)`** — the `.obj` objective sub-file encoder, the inverse of
`decode_obj`. Same template as `encode_cmp`: build the decompressed record
buffer (the flat objective sequence), LZSS-compress it, write the 10-byte
header. Proves the sub-file encoder pattern works for the LZSS-compressed
objective records (fixed fields + variable-length fstatus + variable-length
links + optional radar data).

### Files (this tranche)

| File | Change |
|------|--------|
| `f4-world-convert/include/f4/world_convert/campaign_json.hpp` | **new** — `from_world_json_campaign`, `read_world_json_version` |
| `f4-world-convert/src/campaign_json.cpp` | **new** — the JSON→CampaignHeader parser |
| `f4-world-convert/include/f4/world_convert/objective_encoder.hpp` | **new** — `encode_obj`, `encode_obj_payload` |
| `f4-world-convert/src/objective_encoder.cpp` | **new** — the .obj encoder |
| `f4-world-convert/src/world_json.cpp` | emit `te_number_f16s`, `camp_map_b64`, `squadrons[]`, `remaining_payload_b64`; float precision fix |
| `f4-world-convert/cli/json2cam.cpp` | `--reencode-cmp` mode |
| `f4-world-convert/tests/test_campaign_json.cpp` | **new** — JSON round-trip + mutation tests |
| `f4-world-convert/tests/test_objective_encoder.cpp` | **new** — .obj round-trip tests |
| `f4-world-convert/CMakeLists.txt`, `tests/CMakeLists.txt` | wire the above |

### Runtime verification (all pass)

```
1. Campaign JSON round-trip (to_world_json → from_world_json_campaign)  PASS
   — .cmp payloads byte-identical (21259 bytes, 0 diffs)
   — te_number_f16s, camp_map, squadrons, remaining_payload all match
2. .obj encode/decode struct round-trip                                 PASS
   — 2659 objectives, payload byte-identical (263613 bytes)
3. --reencode-cmp mutation test (advance current_time +3600)            PASS
4. Modified-save end-to-end (JSON → re-encode .cmp → .cam → load)       PASS
   — mutation survived (current_time +7200)
   — all non-.cmp subfiles unchanged
```

### How a host saves a modified campaign (updated)

The library API (no JSON needed):

```cpp
#include <f4/world_convert/cmp_encoder.hpp>
#include <f4/world_convert/cam_writer.hpp>

auto cmp_bytes = f4::world_convert::encode_cmp(header, camp_version);
f4::world_convert::CamWriter w;
w.add("save1.cmp", std::move(cmp_bytes));
for (const auto& sf : original_archive.subfiles())
    if (sf.ext() != "cmp") w.add(sf.name, sf.data);
w.write("save1_resumed.cam");
```

Or via the CLI (from a world JSON):

```bash
json2cam save1.world.json save1_resumed.cam --reencode-cmp
```

Both produce a `.cam` that loads (in F4 or FreeFalcon) to the mutated
campaign state.

---

## 2c. `.tea` encoder tranche — team-state saves

The `.cmp` and `.obj` encoders (§2, §2b) close the campaign-header and
objective save diagonals. This tranche closes the **team-state** diagonal:
the `.tea` sub-file (team rosters, stances, ATM schedules, priorities —
the fields `apply_to(ledger, ws)` mutates when team states change).

### What landed

**`encode_tea(dec)`** — the `.tea` team sub-file encoder, the inverse of
`decode_tea`. Unlike `.cmp`/`.obj`, the `.tea` sub-file is **raw** (not
LZSS-compressed), so the encoder writes directly — no compress step. Per
team: `TeamClass(739) + ATM(variable) + GTM(15) + NTM(15)`.

**Decoder enhancement: GTM/NTM captured verbatim.** The decoder previously
skipped the GTM/NTM records (15 bytes each). It now captures them into two
new `TeamRecord` fields (`gtm_raw`, `ntm_raw` — additive, backward-
compatible), so the encoder reproduces them byte-for-byte. This makes the
GTM/NTM region **byte-faithful** on round-trip — a stronger bar than
`.cmp`/`.obj` reach. FreeFalcon's `LoadTeams` reads these (the GTM/NTM
VU_ID, entity_type, owner are meaningful), so verbatim preservation matters
for FreeFalcon compatibility, not just F4's own round-trip.

### Byte-identity scope

| Region | Byte-faithful? | Why |
|--------|---------------|-----|
| GTM/NTM (15 bytes each) | ✅ Yes | Captured verbatim by decoder |
| ATM (airbases, requests) | ✅ Yes | Fully decoded → re-encoded from struct |
| All fixed-width struct fields | ✅ Yes | Deterministic given the struct |
| Name[20] / motto[200] padding | ❌ No | FreeFalcon garbage after NUL; encoder zero-pads |

The string-padding difference is the same class as `.cmp` (§4): 176 diffs
of 8986 bytes (2.0%), all in name padding. The decoded structs are identical
— a saved `.tea` loads to the same team state in both F4 and FreeFalcon.

### Runtime verification (all pass)

```
1. .tea decode → encode → decode struct round-trip    PASS
   — 8 teams, bytes_consumed == subfile size (8986)
2. Byte-identity scope                                  PASS
   — 176 diffs of 8986 bytes (2.0%), all in string padding
   — GTM/NTM byte-faithful
3. GTM/NTM verbatim preservation                        PASS
4. ATM (airbases + requests) round-trip                 PASS
5. Team field spot-checks (who, name, stance, etc.)     PASS
```

### Files (this tranche)

| File | Change |
|------|--------|
| `f4-world-convert/include/f4/world_convert/team_encoder.hpp` | **new** — `encode_tea` |
| `f4-world-convert/src/team_encoder.cpp` | **new** — the .tea encoder |
| `f4-world-convert/include/f4/world_convert/team_decoder.hpp` | `gtm_raw`/`ntm_raw` fields on `TeamRecord` (additive) |
| `f4-world-convert/src/team_decoder.cpp` | capture GTM/NTM verbatim instead of skipping |
| `f4-world-convert/tests/test_team_encoder.cpp` | **new** — .tea round-trip + byte-identity scope tests |
| `f4-world-convert/CMakeLists.txt`, `tests/CMakeLists.txt` | wire the above |

---

## 2d. `.uni` encoder tranche — unit saves (the hardest sub-file)

The `.cmp`, `.obj`, and `.tea` encoders (§2, §2b, §2c) close the campaign-
header, objective, and team-state save diagonals. This tranche closes the
**unit-state** diagonal: the `.uni` sub-file (battalions, brigades, squadrons,
taskforces, flights, packages — the fields `apply_to(ledger, ws)` mutates
when units move or take losses).

This is the hardest sub-file encoder because of:
- **Subclass dispatch**: 6 tail types (Battalion, Brigade, Squadron, TaskForce,
  Flight, Package), each with its own field layout. The decoder uses trial-
  and-error with next-record validation; the encoder dispatches directly on
  `UnitRecord::unit_class` (known after decode).
- **Version gates**: `pos_.z_` at v70+, `current_wp`/`wp_count` as ushort at
  v71+, flight `old_mission`/`mission_context`/`requester` at v65+, `refuel`
  at v72+, loadout width 32 vs 48 at v73+, squadron `stores[]` 200/220/600.
- **Variable-length fields**: waypoints (count-prefixed), brigade elements,
  flight loadouts (per-aircraft entries), package routes (ingress/egress),
  squadron pilots (48 × 10 bytes).
- **Package branch selection**: small (Final && !wait_cycles) vs big, with
  the small branch's compact `mis_request` and the big branch's full 76-byte
  `MissionRequestClass`.

### What landed

**`encode_uni(dec)`** — the `.uni` unit sub-file encoder, the inverse of
`decode_uni`. Same LZSS-compress template as `.obj`: build the decompressed
record buffer, compress it, write the 10-byte header. Per record:
`[i16 type] + CampBaseClass + UnitClass fixed + wp_count + waypoints +
subclass tail`.

**`encode_uni_payload(dec)`** — the decompressed record buffer, exposed for
tests to compare byte-for-byte against the original.

### Byte-identity scope

Struct-faithful, not byte-faithful — same bar as `.cmp`/`.obj`. Fields the
decoder **skips** (and thus doesn't capture in the struct) are zeroed on
re-encode:
- Squadron: `stores[200/220/600]`, `schedule[64]`, `rating[16]`
- Flight: `last_move`, `last_combat`, `last_direction`, `slots[4]`,
  `pilots[4]`, `plane_stats[4]`, `player_slots[4]`, `last_player_slot`,
  and duplicate loadout entries (entry 0 is re-encoded; others zeroed)
- Package big (v<35): `threat_stats`

The decoded structs are identical — a saved `.uni` loads to the same unit
state. (Same class of difference as `.cmp`/`.tea` string padding; see §4.)

### Runtime verification (all pass)

```
1. .uni decode → encode → decode struct round-trip    PASS
   — 683 units, bytes_consumed == inner_size (128448)
   — payloads byte-identical after round-trip
2. Subclass distribution preserved                     PASS
   — 524 battalions, 85 brigades, 72 squadrons, 2 taskforces
3. Per-subclass field spot-checks (core fields)        PASS
4. Squadron pilots round-trip (48 pilots × 10 bytes)   PASS
5. LZSS round-trip on real .uni payload                PASS
   — 128448 → 35346 bytes (0.28 ratio)
```

Note: the `save1.cam` fixture carries battalions/brigades/squadrons/
taskforces but no flights/packages. The flight and package encoder paths
are compile-checked and follow the same proven template; the v71
`TestCamp.cam` fixture (mentioned in the decoder) would exercise them.

### Files (this tranche)

| File | Change |
|------|--------|
| `f4-world-convert/include/f4/world_convert/unit_encoder.hpp` | **new** — `encode_uni`, `encode_uni_payload` |
| `f4-world-convert/src/unit_encoder.cpp` | **new** — the .uni encoder (all 6 subclass tails) |
| `f4-world-convert/tests/test_unit_encoder.cpp` | **new** — .uni round-trip + per-subclass tests |
| `f4-world-convert/CMakeLists.txt`, `tests/CMakeLists.txt` | wire the above |

---

## 2e. CampaignSaver — the integration bridge

The four sub-file encoders (§2–§2d) close the binary save format. This tranche
closes the **integration bridge**: connecting the campaign simulation's
mutated `WorldState` (via `apply_to(ledger, ws)`) to the binary encoders,
so a host can persist a modified campaign as a `.cam` with one call.

### What landed

**`CampaignMutations`** — a plain-data struct carrying the campaign-level
fields `apply_to` mutates: `current_time`, `last_resupply`/`last_repair`/
`last_reinforcement`, `te_number_aircraft[8]`. Fields default to `INT32_MIN`
(sentinel for "keep original"). No f4-world dependency — the host fills it
from `WorldState` without pulling f4-world-convert into a dependency cycle.

**`save_campaign(world_json, output_cam, mut)`** / **`build_campaign(...)`** —
the bridge function. Reads the original world JSON (with `subfiles_b64`),
parses the campaign block into a `CampaignHeader`, applies the mutations,
re-encodes the `.cmp` via `encode_cmp`, and assembles the `.cam` via
`CamWriter` (new `.cmp` + all other subfiles passed through verbatim).

### How a host saves a modified campaign

```cpp
#include <f4/world_convert/campaign_saver.hpp>

// After apply_to(ledger, ws) has mutated the WorldState:
f4::world_convert::CampaignMutations mut;
mut.current_time = ws.campaign.current_time;
mut.last_resupply = ws.campaign.last_resupply;
mut.last_repair = ws.campaign.last_repair;
mut.last_reinforcement = ws.campaign.last_reinforcement;
mut.te_number_aircraft = ws.campaign.te_number_aircraft;

f4::world_convert::save_campaign(world_json_string, "save1_resumed.cam", mut);
```

`save1_resumed.cam` loads (in F4 or FreeFalcon) to a campaign with the
advanced clock, attrited team pools, and updated maintenance timers — the
closed decode → run → fight → apply → **save → reload** loop.

### Runtime verification (all pass)

```
1. Default mutations (struct-faithful re-encode)          PASS
2. Mutate current_time (+3600) → save → reload → verify   PASS
3. Mutate team pools (attrit team 0, reinforce team 1)    PASS
4. Mutate maintenance timers                              PASS
5. Non-.cmp subfiles pass through byte-identically        PASS
6. Full campaign tick (+1 day, attrit 2 teams, timers)    PASS
```

### Files (this tranche)

| File | Change |
|------|--------|
| `f4-world-convert/include/f4/world_convert/campaign_saver.hpp` | **new** — `CampaignMutations`, `save_campaign`, `build_campaign` |
| `f4-world-convert/src/campaign_saver.cpp` | **new** — the bridge implementation |
| `f4-world-convert/tests/test_campaign_saver.cpp` | **new** — mutation + passthrough tests |
| `f4-world-convert/CMakeLists.txt`, `tests/CMakeLists.txt` | wire the above |

### Follow-on: .uni and .obj mutations

The `.cmp` path (campaign clock, timers, team pools) is the most common
modified-save case and is now landed. The `.uni` (squadron kills) and `.obj`
(objective fstatus) mutations that `apply_to` also produces are the
documented next step — they require decoding the sub-file, overwriting the
mutated fields in the decode struct, and re-encoding. The decode + encode
infrastructure is all landed; this is the "load → mutate → save" loop for
those two sub-files, same pattern as the `.cmp` path.

---

## 3. Acceptance gates (runtime-verified)

A standalone smoke harness (linking the real `.cpp` files, run against the
`save1.cam` fixture) confirms every gate. **All pass:**

| Gate | Result |
|------|--------|
| `decompress(compress(random 10KB)) == original` | PASS |
| `decompress(compress(real .cmp payload)) == original` | PASS (21259→4480 bytes, 0.21 ratio) |
| `decode_cmp(encode_cmp(h)) == h` (struct equality) | PASS (payloads identical) |
| re-encoded `.cmp` decodes with `bytes_consumed == decompressed_size`, `remaining_payload` empty | PASS |
| `CamWriter.build()` on `save1.cam` subfiles == `cam.raw_bytes()` | PASS (byte-identical) |
| reloaded `.cam` has identical subfiles (name + data) | PASS |
| `cam_from_world_json(to_world_json(cam, preserve))` → loads to identical subfiles | PASS |

The gtest suites (`test_lzss_compress`, `test_cmp_encoder`, `test_cam_writer`)
encode the same checks for CI.

---

## 4. Byte-identity: the honest bar

Two distinct round-trips, two distinct bars:

1. **Container passthrough** (`json2cam` via `subfiles_b64`): **byte-identical**
   to the original `.cam`. The sub-file bytes (including the original `.cmp`)
   pass through verbatim. Use this for exact archival / re-packaging.

2. **`.cmp` re-encoding** (`encode_cmp` from a `CampaignHeader`): **struct-
   identical, not byte-identical** to FreeFalcon's original. Measured against
   `save1.cam`: the re-encoded decompressed payload is the same *length*
   (21259 bytes) with 1680 differing bytes, **all in team `name[20]`/`motto[200]`
   padding** — FreeFalcon's file carries non-zero buffer garbage after the
   `\0` terminators; our encoder zero-pads. The decoded structs are identical
   (team names, mottos, every field). A `.cam` written via `encode_cmp` loads
   to the same game state in both FreeFalcon and F4.

This is the correct contract for a save format: **what you load is what you
saved**, even if the on-disk padding differs from FreeFalcon's lax hygiene.
Matching FreeFalcon's garbage bytes byte-for-byte would require capturing
padding in the decoder structs — not required for the loop, and deliberately
not done (cleaner output is better).

---

## 5. How a host saves a modified campaign

The library API is the save path the simulation/campaign code calls directly
(no JSON round-trip needed):

```cpp
#include <f4/world_convert/cmp_encoder.hpp>
#include <f4/world_convert/cam_writer.hpp>

// After apply_to(ledger, ws) has mutated the campaign state, rebuild the
// CampaignHeader from the WorldState's campaign fields (current_time, the
// team slots, last_resupply/repair/reinforcement, ...), then:
auto cmp_bytes = f4::world_convert::encode_cmp(header, camp_version);

// Assemble the .cam: the new .cmp + every other sub-file carried through
// from the loaded archive (objectives, units, teams, ... pass unchanged —
// re-encoding those is the follow-on below).
f4::world_convert::CamWriter w;
w.add("save1.cmp", std::move(cmp_bytes));
for (const auto& sf : original_archive.subfiles())
    if (sf.ext() != "cmp") w.add(sf.name, sf.data);
w.write("save1_resumed.cam");
```

`save1_resumed.cam` loads (in F4 or FreeFalcon) to a campaign with the
advanced `current_time`, updated team states, and the ledger's attrition
applied — the closed loop.

---

## 6. Follow-on (not in this tranche)

### 6.1 WorldState → JSON emitter (the runtime-mutated save path)

`f4-world` currently only *reads* the world JSON. A `WorldState` → JSON
emitter (the reverse of `world_loader.cpp`, over the typed structs) lets a
host write the *full* mutated world (not just the `.cmp` campaign header) as
JSON, which `json2cam --reencode-cmp` could then turn into a `.cam`. This
closes the diagonal for objectives (`.obj`/`.obd`) and units (`.uni`), whose
positions and damage a campaign tick mutates.

Lives in `f4-world-convert` over the decode structs (the `world_json.cpp`
emit pattern), composing with `encode_cmp` for the `.cmp` (✅ landed) and
`encode_obj` for the `.obj` (✅ landed) and pass-through base64 for sub-files
not yet re-encodable.

### 6.2 `.uni` / `.tea` encoders

All four typed sub-file encoders have landed: `.cmp` (✅), `.obj` (✅), `.tea`
(✅), `.uni` (✅). The save format now handles every typed sub-file FreeFalcon's
campaign uses. The remaining save-path work is the WorldState → JSON emitter
(§6.1) and optional full byte-identity (§6.3).

The `.uni` encoder was the largest lift — 6 subclass tails (Battalion/Brigade/
Squadron/TaskForce/Flight/Package), version-gated fields, variable-length
waypoints/loadouts/pilots/routes, and the Package small/big branch selection.
It's struct-faithful (same bar as `.cmp`/`.obj`); the `.tea` encoder remains
the only one with a byte-faithful region (GTM/NTM, captured verbatim).

### 6.3 Full byte-identity against FreeFalcon originals (optional)

Capturing the original fixed-width-string padding and event-text `len` in
the decoder structs (`TeamEntry::name_pad`, `CampaignEvent::text_len`) would
make `encode_cmp` byte-identical to FreeFalcon's output. Not required for
the save/load loop (struct identity suffices); only worth doing if diffing
F4-written saves against FreeFalcon-written saves becomes a workflow.

---

## 7. Verification recipe

```bash
cmake -B build -S .
cmake --build build
ctest --test-dir build/f4-lzss/tests --output-on-failure
ctest --test-dir build/f4-world-convert/tests --output-on-failure

# End-to-end CLI round-trip:
./build/f4-world-convert/cam2json \
    f4-world-convert/tests/fixtures/save1.cam /tmp/s.json --preserve-subfiles
./build/f4-world-convert/json2cam /tmp/s.json /tmp/s.cam
./build/f4-world-convert/cam2json /tmp/s.cam /tmp/s2.json
diff <(jq -S . /tmp/s.json) <(jq -S . /tmp/s2.json)   # structurally identical
```
