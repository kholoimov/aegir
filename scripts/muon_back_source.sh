#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
#
# SPDX-License-Identifier: LGPL-3.0-or-later

# muon_back_source: replay small hand-made FairShip muon-background files and
# check the published muons and event headers against what MuonBackGenerator
# would transport. Covers:
#   - cbmsim/PlaneHAPoint: event n is input entry n, an entry without a muon
#     on the plane gives an empty event, the muons start from their MCTrack
#     start, each muon carries its MCTrack weight, the event weight is 1 and
#     original_event_id the input entry,
#   - muons of different weights stay in one event, each with its own weight,
#   - cbmsim/vetoPoint (old production): the 70.845 m target offset,
#   - pythia8-Geant4: a non-muon entry gives an empty event, positions in m,
#   - running past the last entry fails,
#   - beam smearing and painting are reproducible for a fixed seed,
#   - through Geant4 (builtin geometry), each primary SimParticle carries the
#     weight of its muon and each secondary that of its parent.
# Relies on PHLEX_PLUGIN_PATH being set (activate.sh does this under
# `pixi run`).
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
workdir=$(mktemp -d)
trap 'rm -rf "$workdir"' EXIT

cat >"$workdir/make_inputs.py" <<'EOF'
import os
import sys
from array import array

import ROOT

# Stand-ins for FairShip's ShipMCTrack and plane point: the source reads the
# split member branches by name, so only those names matter.
ROOT.gInterpreter.Declare("""
struct ShipMCTrack { int fPdgCode; double fPx, fPy, fPz, fM, fStartX, fStartY,
  fStartZ, fStartT, fW; };
struct PlanePoint { int fTrackID; int fPdgCode; };
""")

new_file, old_file, flat_file = sys.argv[1:4]


def track(pdg, p, m, start, t, w):
    tr = ROOT.ShipMCTrack()
    tr.fPdgCode = pdg
    tr.fPx, tr.fPy, tr.fPz = p
    tr.fM = m
    tr.fStartX, tr.fStartY, tr.fStartZ = start
    tr.fStartT, tr.fW = t, w
    return tr


def point(track_id, pdg):
    pt = ROOT.PlanePoint()
    pt.fTrackID, pt.fPdgCode = track_id, pdg
    return pt


proton = track(2212, (0, 0, 400), 0.938, (0, 0, 0), 0, 1.0)
entries = [
    # 0: only a photon reaches the plane: an empty event.
    ([proton, track(22, (0.1, 0, 5), 0, (1, 2, 3), 0.1, 1.0)],
     [point(1, 22)]),
    # 1: one muon; the pion it came from is not published.
    ([proton, track(211, (0, 1, 20), 0.1396, (0, 0, 1), 0, 1.0),
      track(13, (0.1, -0.2, 10), 0.10566, (0.5, -0.5, 20), 0.2, 0.5)],
     [point(2, 13)]),
    # 2: a muon pair; the mu- crosses the plane twice but is published once.
    ([proton, track(13, (0, 0, 30), 0.10566, (1, 1, 10), 0.3, 0.25),
      track(-13, (0.2, 0, 15), 0.10566, (2, 2, 12), 0.4, 0.25)],
     [point(1, 13), point(1, 13), point(2, -13)]),
    # 3: only a neutrino: an empty event.
    ([proton, track(14, (0, 0, 50), 0, (0, 0, 5), 0, 1.0)],
     [point(1, 14)]),
    # 4: muons of weights 1 and 0.01, each published with its own weight.
    ([proton, track(13, (0, 0.1, 25), 0.10566, (3, 0, 30), 0.5, 1.0),
      track(-13, (0.1, 0, 35), 0.10566, (0, 3, 40), 0.6, 0.01),
      track(-13, (0, 0, 45), 0.10566, (1, 0, 50), 0.7, 1.0)],
     [point(3, -13), point(2, -13), point(1, 13)]),
]


def write_cbmsim(path, point_branch):
    f = ROOT.TFile(path, "RECREATE")
    tree = ROOT.TTree("cbmsim", "")
    mc = ROOT.std.vector("ShipMCTrack")()
    pts = ROOT.std.vector("PlanePoint")()
    tree.Branch("MCTrack", mc, 32000, 99)
    tree.Branch(point_branch, pts, 32000, 99)
    for tracks, points in entries:
        mc.clear()
        pts.clear()
        for tr in tracks:
            mc.push_back(tr)
        for pt in points:
            pts.push_back(pt)
        tree.Fill()
    tree.Write()
    f.Close()


write_cbmsim(new_file, "PlaneHAPoint")
write_cbmsim(old_file, "vetoPoint")

# pythia8-Geant4 ntuple: positions in m. Entry 0 is a neutrino.
f = ROOT.TFile(flat_file, "RECREATE")
tree = ROOT.TTree("pythia8-Geant4", "")
names = ["id", "parentid", "pythiaid", "ecut", "w", "x", "y", "z", "px", "py", "pz"]
buf = {n: array("f", [0.0]) for n in names}
for n in names:
    tree.Branch(n, buf[n], f"{n}/F")
for row in [(14, 0, 211, 1, 1.0, 0, 0, -1, 0, 0, 20),
            (13, 0, 211, 1, 3.5, 0.01, 0.02, -1, 0.5, 0, 40),
            (-13, 0, 321, 1, 2.0, -0.01, 0, -0.5, 0, 0.3, 60)]:
    for n, v in zip(names, row):
        buf[n][0] = v
    tree.Fill()
tree.Write()
f.Close()
sys.stdout.flush()
os._exit(0)
EOF

cat >"$workdir/check.py" <<'EOF'
import json
import math
import os
import sys

import ROOT

# Keep readers and views alive until os._exit (PyROOT teardown can crash).
alive = []


def events(path):
    reader = ROOT.RNTupleReader.Open("events", path)
    vp = reader.GetView["std::vector<SHiP::MCParticle>"]("mc_particles")
    vh = reader.GetView["SHiP::EventHeader"]("event_header")
    alive.extend((reader, vp, vh))
    out = []
    for i in range(reader.GetNEntries()):
        h = vh(i)
        parts = [[p.pdgCode, list(p.vertex), list(p.momentum), p.energy,
                  p.time, p.motherId, p.weight] for p in vp(i)]
        out.append([int(h.original_event_id), h.weight, parts])
    # The writer emits in completion order.
    return sorted(out, key=lambda e: e[0])


def close(a, b):
    if isinstance(a, list):
        return len(a) == len(b) and all(close(x, y) for x, y in zip(a, b))
    return math.isclose(a, b, rel_tol=1e-6, abs_tol=1e-9)


def mu(pdg, vertex_mm, p, t, w, m=0.10566):
    e = math.sqrt(sum(x * x for x in p) + m * m)
    return [pdg, vertex_mm, list(p), e, t, -1, w]


# Every event has weight 1; the FairShip weights are per muon.
def cbmsim(z0):
    return [
        [0, 1.0, []],
        [1, 1.0, [mu(13, [5, -5, z0 + 200], (0.1, -0.2, 10), 0.2, 0.5)]],
        [2, 1.0, [mu(13, [10, 10, z0 + 100], (0, 0, 30), 0.3, 0.25),
                  mu(-13, [20, 20, z0 + 120], (0.2, 0, 15), 0.4, 0.25)]],
        [4, 1.0, [mu(13, [30, 0, z0 + 300], (0, 0.1, 25), 0.5, 1.0),
                  mu(-13, [0, 30, z0 + 400], (0.1, 0, 35), 0.6, 0.01),
                  mu(-13, [10, 0, z0 + 500], (0, 0, 45), 0.7, 1.0)]],
        [3, 1.0, []],
    ]


# Positions: cm -> mm; old productions and the ntuple get +70845 mm in z.
mmu = 0.1056583755
want = {
    "new": cbmsim(0),
    "old": cbmsim(70845),
    "flat": [
        [0, 1.0, []],
        [1, 1.0, [mu(13, [10, 20, 69845], (0.5, 0, 40), 0, 3.5, mmu)]],
        [2, 1.0, [mu(-13, [-10, 0, 70345], (0, 0.3, 60), 0, 2.0, mmu)]],
    ],
}

case, path = sys.argv[1], sys.argv[2]
got = events(path)
expected = sorted(want[case], key=lambda e: e[0])
ok = len(got) == len(expected)
for g, w in zip(got, expected):
    ok &= g[0] == w[0] and close(g[1], w[1]) and len(g[2]) == len(w[2])
    ok &= all(gp[0] == wp[0] and gp[5] == wp[5] and close(gp[1:5], wp[1:5])
              and close(gp[6], wp[6]) for gp, wp in zip(g[2], w[2]))
if not ok:
    print(f"{case}: got\n{json.dumps(got)}\nexpected\n{json.dumps(expected)}")
sys.stdout.flush()
os._exit(0 if ok else 1)
EOF

cat >"$workdir/read.jsonnet" <<'EOF'
local lib = import 'lib.libsonnet';
{
  driver: lib.driver(std.parseInt(std.extVar('events'))),
  sources: {
    muons: lib.muon_back {
      input_file: std.extVar('infile'),
      seed: 7,
      smear_beam: std.parseJson(std.extVar('smear')),
      paint_beam: std.parseJson(std.extVar('paint')),
    },
  },
  modules: {
    output: lib.mc_only_output(std.extVar('simout'), std.extVar('histo')),
  },
}
EOF

run() {  # run <events> <infile> <out> <smear> <paint>
  phlex -c <(jsonnet -J "$here/../workflows" \
    --ext-str events="$1" --ext-str infile="$2" \
    --ext-str simout="$3" --ext-str histo="$3.valid.root" \
    --ext-str smear="$4" --ext-str paint="$5" \
    "$workdir/read.jsonnet")
}

python3 "$workdir/make_inputs.py" \
  "$workdir/new.root" "$workdir/old.root" "$workdir/flat.root"

# 1. The three formats, without beam smearing: one event per entry.
for case in new:5 old:5 flat:3; do
  name=${case%:*}
  run "${case#*:}" "$workdir/$name.root" "$workdir/${name}_out.root" 0 0
  python3 "$workdir/check.py" "$name" "$workdir/${name}_out.root"
done

# 2. Asking for more events than the file has entries fails.
if run 6 "$workdir/new.root" "$workdir/exhausted.root" 0 0 \
  >"$workdir/exhausted.log" 2>&1; then
  echo "expected the run past the last entry to fail"
  exit 1
fi
grep -q "input exhausted" "$workdir/exhausted.log" || {
  cat "$workdir/exhausted.log"
  exit 1
}

# 3. Beam smearing + painting: moves x/y, reproducibly for a fixed seed.
run 5 "$workdir/new.root" "$workdir/smear_a.root" 8 50
run 5 "$workdir/new.root" "$workdir/smear_b.root" 8 50
if python3 "$workdir/check.py" new "$workdir/smear_a.root" >/dev/null; then
  echo "beam smearing left the vertices unchanged"
  exit 1
fi
python3 - "$workdir/smear_a.root" "$workdir/smear_b.root" <<'EOF'
import os
import sys

import ROOT

alive = []


def dump(path):
    r = ROOT.RNTupleReader.Open("events", path)
    vp = r.GetView["std::vector<SHiP::MCParticle>"]("mc_particles")
    vh = r.GetView["SHiP::EventHeader"]("event_header")
    alive.extend((r, vp, vh))
    return sorted((int(vh(i).original_event_id),
                   tuple(tuple(p.vertex) for p in vp(i)))
                  for i in range(r.GetNEntries()))


ok = dump(sys.argv[1]) == dump(sys.argv[2])
if not ok:
    print("smeared vertices differ between two runs with the same seed")
sys.stdout.flush()
os._exit(0 if ok else 1)
EOF

# 4. Through Geant4: primaries carry their muon's weight, secondaries their
# parent's. Entry 4 has muons of weights 1 and 0.01 in one event; entries 0
# and 3 are empty events.
cat >"$workdir/g4.jsonnet" <<'EOF'
local lib = import 'lib.libsonnet';
{
  driver: lib.driver(5),
  sources: {
    field: lib.null_field,
    geometry: lib.builtin_geometry,
    muons: lib.muon_back {
      input_file: std.extVar('infile'),
      seed: 7,
      smear_beam: 0,
      paint_beam: 0,
    },
  },
  modules: {
    geant4: lib.geant4 { seed: 7 },
    output: lib.full_output(std.extVar('simout'), std.extVar('histo')),
  },
}
EOF
phlex -c <(jsonnet -J "$here/../workflows" --ext-str infile="$workdir/new.root" \
  --ext-str simout="$workdir/g4.root" --ext-str histo="$workdir/g4.valid.root" \
  "$workdir/g4.jsonnet")
python3 - "$workdir/g4.root" <<'EOF'
import math
import os
import sys

import ROOT

r = ROOT.RNTupleReader.Open("events", sys.argv[1])
vm = r.GetView["std::vector<SHiP::MCParticle>"]("mc_particles")
vs = r.GetView["std::vector<SHiP::SimParticle>"]("sim_particles")
vh = r.GetView["SHiP::EventHeader"]("event_header")
ok = r.GetNEntries() == 5
weights_seen = set()
n_secondaries = 0
for i in range(r.GetNEntries()):
    mc = [p.weight for p in vm(i)]
    sim = {p.trackId: (p.parentId, p.weight) for p in vs(i)}
    ok &= vh(i).weight == 1.0
    for track_id, (parent_id, w) in sim.items():
        weights_seen.add(round(w, 6))
        if parent_id == 0:
            # Geant4 numbers the primaries 1..N in the order they were given.
            ok &= math.isclose(w, mc[track_id - 1])
        elif parent_id in sim:
            n_secondaries += 1
            ok &= w == sim[parent_id][1]
ok &= n_secondaries > 0 and {0.5, 0.25, 1.0, 0.01} <= weights_seen
if not ok:
    print(f"SimParticle weights wrong: seen {sorted(weights_seen)}, "
          f"{n_secondaries} secondaries")
sys.stdout.flush()
os._exit(0 if ok else 1)
EOF

echo "muon_back_source passed: the three formats, per-muon weights, exhaustion,"
echo "seeded beam smearing and weights through Geant4 all as expected"
