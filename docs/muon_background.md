# Muon background from FairShip samples

`muon_back_source` replays the muon-background samples produced with FairShip
— the muons leaving the target and hadron absorber — as FairShip's
`MuonBackGenerator` does for `run_simScript.py --MuonBack`. It binds the
file's branches to plain arrays, so no FairShip installation is needed.

```jsonnet
local lib = import 'lib.libsonnet';
{
  sources: {
    muons: lib.muon_back { input_file: 'muons.root', seed: 1 },
    ...
  },
}
```

[`workflows/muon_back_only.jsonnet`](../workflows/muon_back_only.jsonnet)
writes the published muons without simulating them;
[`workflows/muon_back_st.jsonnet`](../workflows/muon_back_st.jsonnet) runs them
through Geant4 and the GeoModel geometry. One file per job; split a sample
over several jobs.

## Input formats

| Tree | Branches | Production |
|------|----------|------------|
| `pythia8-Geant4` | flat floats `id`, `w`, `x`/`ox`, `px`/`opx`, … (m, GeV) | muon ntuples |
| `cbmsim` | `MCTrack` + `PlaneHAPoint` | current (2026 MinBias) |
| `cbmsim` | `MCTrack` + `vetoPoint` | old |

## What is published

Event n is input entry `first_entry` + n, as in `genie_reader_source`:

- `pythia8-Geant4`: the entry's muon, if its `id` is a muon.
- `cbmsim`: every muon on the hadron-absorber exit plane. Each is published
  from its **MCTrack start** — its production vertex in the target, with its
  production momentum and time — so it is transported through the absorber
  and muon shield again, as in FairShip.

An entry without a muon gives an event with no particles; FairShip skips to
the next entry with a muon instead. Such entries are rare in the normalised
`_mu` files (80 of 406,696 in `j87_mu`) but common in the raw ones (30% of
`j87`).

FairShip also puts the rest of the MCTrack record on its stack, untransported;
`geant4_module` transports everything it is given, so only the muons are
published (with `motherId` -1).

## Weights and provenance

Weights are set per muon in the production, not per proton interaction. The
rare muon sources are enhanced — the Pythia8 branching ratios of η, ω, φ, ρ⁰
and η′ to μμ by `run_fixedTarget.py --boostDiMuon`, the Geant4 γ → μμ and
e⁺e⁻ → μμ cross sections by `--boostFactor` — and
`extractMuonsAndUpdateWeight.py` divides the weight of each muon from such a
process by the boost. One entry can therefore hold a π/K-decay muon of weight
1 next to an ω → μμ muon of weight 0.01 (about 0.08% of the entries of the
normalised 2026 MinBias files). Each weight is correct for its own muon only.

The weights are therefore kept per particle, as FairShip keeps them per track:

- `MCParticle.weight` — the muon's weight: the MCTrack `fW` for `cbmsim`, `w`
  for `pythia8-Geant4`.
- `SimParticle.weight` — after `geant4_module`, the weight of the muon a
  particle descends from: Geant4 gives a primary the weight of its
  MCParticle and every secondary that of its parent.
- `event_header.weight` — 1: the files carry no per-event weight.
- `event_header.original_event_id` — the input entry the event came from.

The full weight of a particle is the event weight times its particle weight.
Observables built from several muons of one entry (coincidences, occupancy
per proton) need the product of the muons' weights.

Per-particle weights need a data model newer than v0.5.0; until it is
released, build against a local checkout with the `local-dm` pixi
environment (see `pixi.toml`).

## Options

| Key | Default | FairShip equivalent |
|-----|---------|---------------------|
| `input_file` | — | `-f` |
| `first_entry` | 0 | `--firstEvent` (an input entry) |
| `smear_beam` | 8 mm | `--SmearBeam`, Gaussian sigma of the beam spot |
| `paint_beam` | 50 mm | `--PaintBeam`, radius of the beam painting circle |
| `phi_randomize` | false | `--phiRandom` |
| `z_offset` | 0 for `PlaneHAPoint`, 70845 mm otherwise | target z given to `FairPrimaryGenerator` |
| `seed` | random, logged | `--sameSeed` |

Lengths are in mm. The beam offset (smearing plus painting) is drawn once per
event and added to every muon's x and y; `z_offset` is added to every z, as
`FairPrimaryGenerator` adds the target position.

`MuonBackGenerator`'s `FollowAllParticles()` and `SetDownScaleDiMuon()` are
not ported: `run_simScript.py` never enables them.

## Randomness

The draws follow FairShip's order and distributions — two Gaussians and one
angle for the beam offset, then one angle per MCTrack for `phi_randomize` —
but come from a Philox stream keyed on the seed and the input entry, not from
`gRandom`. The numbers differ from a FairShip run, but each entry is
reproducible on its own whatever the threading or event range, which is what
`--sameSeed` gives in FairShip.

## Running out of input

Asking for more events than the file has entries (from `first_entry` on)
fails with an `input exhausted` error, as the other file-reading sources do.
FairShip stops the run instead.
