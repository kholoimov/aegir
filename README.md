# aegir

Phlex-based simulation framework for the SHiP experiment.

## Documentation

An [automatic class reference](https://shipsoft.github.io/aegir/) is built using Doxygen from comments in the C++ code.

## Architecture

```
[Source: ParticleGun / Pythia8 / FixedTarget] -> std::vector<MCParticle>
    |
[Provider: IGeometrySource]
    |
[Transform: Geant4] -> SimResult {hits, particles}
    |
[Observer: RNTupleWriter + Histogrammer] -> output.root, validation.root
```

### Plugins

| Plugin | Type | Description |
|--------|------|-------------|
| `particle_gun_source` | Source | Configurable single-particle gun with Random123 RNG |
| `pythia8_source` | Source | Pythia8 fixed-target p-p (serial or PythiaParallel) |
| `fixed_target_source` | Source | Dual-target Pythia8 (p-p/p-n) with interaction point sampling |
| `muon_back_source` | Source | FairShip muon-background files, replayed as MuonBackGenerator does ([docs](docs/muon_background.md)) |
| `geometry_builtin_provider` | Provider | W target + Si scoring planes (test geometry) |
| `geometry_gdml_provider` | Provider | GDML file loader |
| `geometry_geomodel_provider` | Provider | GeoModel .db via SHiPGeometryService (optional) |
| `geant4_module` | Transform | Geant4 simulation (direct worker, configurable concurrency) |
| `sim_output_module` | Observer | RNTuple parallel writer + validation histograms |

## Dependencies

Pixi resolves everything below from [`prefix.dev/ship`][ship-channel] (the
SHiP-specific packages live there, with rattler-build recipes in
[ship-conda-recipes][recipes]) and `conda-forge`. The list is reference; you
do not need to install these manually when using pixi.

- [SHiPDataModel](https://github.com/ShipSoft/data-model) (event data classes)
- [Phlex](https://github.com/Framework-R-D/phlex) (+ Boost, TBB, spdlog)
- ROOT 6.40+ (Core, RIO, Hist, MathCore, ROOTNTuple)
- Geant4 11.3+ (with multi-threading enabled)
- Pythia8 8.3+
- Random123
- Optional: SHiPGeometryService (for GeoModel geometry)

[ship-channel]: https://prefix.dev/channels/ship
[recipes]: https://github.com/ShipSoft/ship-conda-recipes

## Building with pixi (recommended)

[Install pixi](https://pixi.sh), then:

```bash
pixi install              # resolve dependencies (one-off)
pixi run build            # configure + build
```

`pixi run build` depends on `configure`, so you do not need to run them
separately. Available tasks (see [`pixi.toml`](pixi.toml)):

- `configure` — run CMake with `CMAKE_PREFIX_PATH=$CONDA_PREFIX`.
- `build` — `cmake --build build -j`. Depends on `configure`.
- `install` — `cmake --install build`, populating `$CONDA_PREFIX/lib` and `bin`.
- `test` — `ctest --test-dir build --output-on-failure`. Runs the integration
  tests registered in [`CMakeLists.txt`](CMakeLists.txt), each a script in
  [`scripts/`](scripts/). CI runs this task.
- `smoke` — quick end-to-end check used by CI ([`scripts/smoke.sh`](scripts/smoke.sh)).
- `clean` — `rm -rf build`.

### `pixi run` vs `pixi shell`

- `pixi run <task>` (or `pixi run <command>`) activates the environment for
  the duration of one command. Use it for scripted invocations and CI.
- `pixi shell` drops you into an interactive subshell with the environment
  active until you `exit`. Use it for iterative development — `just`
  targets, ad-hoc `phlex` invocations, debugging.

`activate.sh` sets `PHLEX_PLUGIN_PATH`, `LD_LIBRARY_PATH`, `SHIPGEOMETRY_ROOT`,
and `AEGIR_ROOT` automatically in both modes.

## Building manually

For environments without pixi (e.g. existing aliBuild setups), the project is a
plain CMake build. You are responsible for providing the dependencies; see
[ship-conda-recipes][recipes] for the canonical version pins.

```bash
cmake -B build
cmake --build build
```

Pass `-DSHiPDataModel_ROOT=/path/to/data-model/install` if a dependency is not in
the default search path.

## Running

`workflows/` ships a sampler of jsonnet configs; the snippets below pick five
common ones. `ls workflows/` shows the full set (multi-threaded variants, no-op
sinks, GeoModel-based geometry, etc.).

### From `pixi shell` (interactive)

```bash
pixi shell

# Particle gun only (no Geant4)
phlex -c workflows/gun_only.jsonnet

# Particle gun -> Geant4 (single-threaded, concurrency defaults to 1)
phlex -c workflows/gun_st.jsonnet

# Particle gun -> Geant4 (multi-threaded, concurrency=4)
phlex -c workflows/gun_mt.jsonnet

# Pythia8 -> Geant4
phlex -c workflows/pythia8_st.jsonnet

# Fixed-target -> Geant4 MT
phlex -c workflows/fixed_target_mt.jsonnet
```

### One-off via `pixi run`

```bash
pixi run phlex -c workflows/gun_only.jsonnet
```

### Manual environment (no pixi)

If you built manually, point `PHLEX_PLUGIN_PATH` at the build tree yourself:

```bash
export PHLEX_PLUGIN_PATH="$PWD/build:$PHLEX_PLUGIN_PATH"
phlex -c workflows/gun_only.jsonnet
```

## Geometry

Three geometry providers are available, selectable via workflow configuration:

- **Builtin** (`geometry_builtin_provider`) — Tungsten target + Si scoring planes
  (test geometry). Used by `workflows/gun_st.jsonnet` and friends. No setup
  needed.
- **GDML** (`geometry_gdml_provider`) — load from a `.gdml` file. Set `gdml_file`
  in the workflow to an absolute path. Example: `workflows/gun_st_gdml.jsonnet`.
- **GeoModel** (`geometry_geomodel_provider`) — load from a GeoModel `.db` file
  via `SHiPGeometryService`. The `shipgeometry` conda package ships a
  `build_geometry` tool but does not bundle a pre-built DB; you generate one
  yourself:

  ```bash
  pixi run build_geometry          # writes ship_geometry.db to the CWD
  pixi run phlex -c workflows/gun_st_geomodel.jsonnet
  ```

  Workflows reference the DB by bare filename (`db_file: 'ship_geometry.db'`);
  the provider resolves it against the CWD first and, failing that, against
  `$SHIPGEOMETRY_ROOT/share/geometry/`. `activate.sh` sets `SHIPGEOMETRY_ROOT`
  to `$CONDA_PREFIX` by default, so dropping a DB under
  `$CONDA_PREFIX/share/geometry/` makes it discoverable from any CWD.

## Geant4 integration

See [docs/geant4_integration.md](docs/geant4_integration.md) for details on the direct worker integration strategy.

## Benchmarking

See `docs/benchmarks.md` for details. The benchmark targets need `hyperfine`
and `just`, which the `bench` pixi feature provides:

```bash
pixi shell -e bench
just bench-geant4          # ST vs MT comparison
just bench-geant4-threads  # Thread count sweep
just bench-pythia8         # Standalone vs Phlex Pythia8
```

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for details on our development workflow and coding standards.

## Known issues

- Geant4 bundles its own zlib, which conflicts with ROOT's system zlib. Build
  Geant4 with `-DGEANT4_USE_SYSTEM_ZLIB=ON` — only relevant for manual builds;
  the conda-forge `geant4` package already takes care of this.
- G4RunManager destruction accesses global singletons that may already be torn
  down during plugin unloading. The RunManager is intentionally leaked.

## Licence

LGPL-3.0-or-later. See `LICENSES/` for the full text.
