// Replay a FairShip muon-background sample without simulating it: the muons
// (with their weights) muon_back_source publishes go straight to the
// mc_only output. Useful to inspect or validate an input sample.
//   jsonnet -J workflows --ext-str events=N --ext-str infile=muons.root \
//     --ext-str simout=mc.root --ext-str histo=valid.root \
//     workflows/muon_back_only.jsonnet
local lib = import 'lib.libsonnet';
{
  driver: lib.driver(std.parseInt(std.extVar('events'))),
  sources: {
    muons: lib.muon_back {
      input_file: std.extVar('infile'),
      seed: 1,
    },
  },
  modules: {
    output: lib.mc_only_output(std.extVar('simout'), std.extVar('histo')),
  },
}
