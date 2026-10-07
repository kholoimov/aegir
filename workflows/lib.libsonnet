// lib.libsonnet — building blocks shared by the aegir workflows.
//
// Workflows compose these with jsonnet object merging, e.g.
//   lib.geant4 { concurrency: 4 }
// overrides or extends a block without repeating it.
{
  driver(events):: {
    cpp: 'generate_layers',
    layers: {
      event: { total: events },
    },
  },

  // ── sources ────────────────────────────────────────────────────────────
  null_field:: { cpp: 'field_null_provider' },

  // Field maps in the SHiP field-map format. Each magnet names a `file`
  // (bare names resolve via $SHIPFIELD_ROOT/share/field/), the `map` inside
  // it, and optionally a `translation` [x, y, z] in mm placing the map's
  // origin in the global frame.
  field_map(magnets):: {
    cpp: 'field_map_provider',
    magnets: magnets,
  },

  // 0.5 T constant By over the whole World volume — the field-smoke setup.
  world_field_05T_y:: self.field_map([
    {
      name: 'WorldField',
      volume_pattern: 'World',
      file: 'world_05T_y.root',
      map: 'world_05T_y',
    },
  ]),

  builtin_geometry:: { cpp: 'geometry_builtin_provider' },

  geomodel_geometry:: {
    cpp: 'geometry_geomodel_provider',
    db_file: 'ship_geometry.db',
    sensitive_volumes: ['ScoringPlane'],
  },

  // The generator sources follow the same seeding convention as the geant4
  // module below: without a `seed`, each run draws a random one (logged at
  // startup, so a run can be reproduced after the fact). Merge one in for
  // reproducible primaries, e.g. lib.gun { seed: 12345 }.
  // Default spray gun: 10–100 GeV μ− in a narrow cone from z = −500 mm.
  gun:: {
    cpp: 'particle_gun_source',
    pdg: 13,
    p_min: 10.0,  // GeV/c
    p_max: 100.0,  // GeV/c
    max_theta: 0.1,  // rad
    vertex_z: -500.0,  // mm
  },

  // Pencil beam for the field smokes: straight 20 GeV μ− from z = −2 m.
  pencil_gun:: {
    cpp: 'particle_gun_source',
    pdg: 13,
    p_min: 20.0,  // GeV/c
    p_max: 20.0,  // GeV/c
    max_theta: 0.0,  // rad
    vertex_z: -2000.0,  // mm
  },

  pythia8:: {
    cpp: 'pythia8_source',
    beam_energy: 400.0,  // GeV
    process: 'SoftQCD:inelastic',
  },

  fixed_target:: {
    cpp: 'fixed_target_source',
    beam_energy: 400.0,  // GeV
    target_z: 74,  // proton number, not a coordinate
    target_a: 184,
    // Target z extents to be determined from GeoModel geometry
    target_z_start: 0.0,  // mm
    target_z_end: 1164.0,  // mm
    interaction_length: 191.9,  // mm
    tau0_threshold: 1.0,  // mm/c
  },

  // Neutrino interactions pre-generated with GENIE, read from a rootracker
  // file (gntpc -f rootracker); see docs/genie.md for how to produce one.
  genie_reader:: {
    cpp: 'genie_reader_source',
    file: 'genie_events.rootracker.root',
  },

  // FairShip muon-background file (pythia8-Geant4 ntuple, or cbmsim with
  // PlaneHAPoint/vetoPoint), replayed as FairShip's MuonBackGenerator does;
  // see docs/muon_background.md. Set `input_file`. The defaults match
  // run_simScript.py --MuonBack.
  muon_back:: {
    cpp: 'muon_back_source',
    first_entry: 0,  // input entry to start from (--firstEvent)
    smear_beam: 8.0,  // mm, Gaussian sigma (--SmearBeam)
    paint_beam: 50.0,  // mm, painting radius (--PaintBeam)
    phi_randomize: false,  // --phiRandom
  },

  // ── modules ────────────────────────────────────────────────────────────
  // Without a `seed`, each run draws a random one (logged at startup, so a
  // run can be reproduced after the fact). Merge one in for reproducible
  // output, e.g. lib.geant4 { seed: 12345 }.
  geant4:: {
    cpp: 'geant4_module',
    physics_list: 'FTFP_BERT',
    verbosity: 0,
  },

  // Crossing-mode Geant4 for the GeoModel workflows: scoring-plane SD with a
  // KE threshold, production-cut regions in the target/absorber, tracked-
  // particle KE cut. Seeding works as for `geant4` above.
  geant4_crossing:: {
    cpp: 'geant4_module',
    physics_list: 'FTFP_BERT',
    sd_mode: 'crossing',
    ke_threshold: 0.5,  // GeV
    energy_cut: true,
    particle_ke_cut: 1.0,  // GeV
    regions: { '/SHiP/target': 50, '/SHiP/muon_shield/magn_absorb': 50 },  // production cuts [mm]
  },

  noop_output:: { cpp: 'sim_output_module', mode: 'noop' },

  // The writing modes also accept two optional keys bounding the writer's
  // memory use (issue #77): cluster_size_mib (default 32) and fill_contexts
  // (default 4, or the phlex thread count if lower). Override by merging, e.g.
  //   lib.full_output(f, h) { cluster_size_mib: 16, fill_contexts: 8 }
  mc_only_output(rntuple_file, histo_file):: {
    cpp: 'sim_output_module',
    mode: 'mc_only',
    rntuple_file: rntuple_file,
    histo_file: histo_file,
  },

  full_output(rntuple_file, histo_file):: {
    cpp: 'sim_output_module',
    mode: 'full',
    rntuple_file: rntuple_file,
    histo_file: histo_file,
  },
}
