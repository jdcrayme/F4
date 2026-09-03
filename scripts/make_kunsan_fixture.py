#!/usr/bin/env python3
"""Regenerate f4-simulation/tests/fixtures/kunsan_campaign.world.json.

Base: save1.cam (the stock korea scenario start, v63) decoded by
cam2json — real objectives, real ground/naval roster, real terrain refs.

Modifications (documented in test_campaign_tick.cpp's header comment):
  1. Team names: slot 1 "USA", slot 2 "ROK", slot 6 "DPRK" (the wire
     names the golden tests pin).
  2. Stances: RelType enum (cmpglobl.h: 0..5, War=5 — the C3
     vocabulary fix; the pre-C3 "negative = war" reading misdecoded
     garbage columns): 1<->6, 2<->6 at WAR (5); all other relations
     Allied (1)/NoRelations (0).
  3. te_number_aircraft: 24 for slots 1, 2, 6 (the pool the tick test
     draws to zero).
  4. Squadrons: exactly ONE ARO_CA wing (specialty 0) for team 1 and
     team 6 (first in wire order); ROK(2) fields none. All other
     squadrons removed. Kept squadrons carry roster 0 so their
     available count comes from the team pool (24 each).

Run from the repo root:
    python3 scripts/make_kunsan_fixture.py
"""
import json, subprocess, sys, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CAM2JSON = os.path.join(ROOT, 'build', 'f4-world-convert', 'cam2json')
SRC = os.path.join(ROOT, 'f4-world-convert', 'tests', 'fixtures', 'save1.cam')
OUT = os.path.join(ROOT, 'f4-simulation', 'tests', 'fixtures', 'kunsan_campaign.world.json')

subprocess.run([CAM2JSON, SRC, OUT], check=True, cwd=ROOT)

d = json.load(open(OUT))
c = d['campaign']

# 1. Team names
c['teams'][1]['name'] = 'USA'
c['teams'][2]['name'] = 'ROK'
c['teams'][6]['name'] = 'DPRK'

# 2. Stances — RelType enum: War = 5 (cmpglobl.h; the C3 vocabulary fix)
for t in c['teams']:
    st = t.get('stance')
    if st is not None:
        t['stance'] = [1 if v != 0 else 0 for v in st]   # sanitize to allied/none
c['teams'][1]['stance'][6] = 5
c['teams'][2]['stance'][6] = 5
c['teams'][6]['stance'][1] = 5
c['teams'][6]['stance'][2] = 5

# 3. Aircraft pools
c['te_number_aircraft'] = [0, 24, 24, 0, 0, 0, 24, 0]

# 4. Squadron filter — keep one ARO_CA wing per team 1 & 6
kept = []
seen = {1: 0, 6: 0}
for u in d['units']['items']:
    if u.get('unit_class') != 'squadron':
        kept.append(u)
        continue
    owner = u.get('owner')
    if owner in seen and seen[owner] == 0 and u.get('specialty') == 0:
        u['roster'] = 0          # draw from the team pool
        kept.append(u)
        seen[owner] += 1
    # else: drop
d['units']['items'] = kept
d['units']['decoded'] = len(kept)
d['units']['note'] = ('kunsan test fixture: single ARO_CA wing per war team '
                      '(see scripts/make_kunsan_fixture.py)')

json.dump(d, open(OUT, 'w'), indent=1)
print(f"wrote {OUT}: {len(kept)} units "
      f"({sum(1 for u in kept if u.get('unit_class')=='squadron')} squadrons)")
