// Simulate a FairShip muon-background sample through the GeoModel geometry,
// single-threaded, with the crossing-mode Geant4 setup (production cuts in
// the target and absorber). As in FairShip, muons start from their
// production vertex in the target, so they cross the absorber again. Each
// muon carries its FairShip weight, which its SimParticles inherit.
//   jsonnet -J workflows --ext-str events=N --ext-str infile=muons.root \
//     --ext-str simout=sim.root --ext-str histo=valid.root \
//     --ext-str db=/abs/path/ship_geometry.db \
//     workflows/muon_back_st.jsonnet
local lib = import 'lib.libsonnet';
{
  driver: lib.driver(std.parseInt(std.extVar('events'))),
  sources: {
    field: lib.null_field,
    geometry: lib.geomodel_geometry {
      db_file: std.extVar('db'),
    },
    muons: lib.muon_back {
      input_file: std.extVar('infile'),
      seed: 1,
    },
  },
  modules: {
    geant4: lib.geant4_crossing { seed: 1 },
    output: lib.full_output(std.extVar('simout'), std.extVar('histo')),
  },
}
