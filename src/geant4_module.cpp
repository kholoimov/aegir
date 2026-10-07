// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// geant4_module.cpp — Phlex module plugin for Geant4 simulation
//
// Phlex TBB threads act as G4 workers directly (no separate G4 thread pool,
// no event queue, no BeamOn). Each framework thread lazily initialises a
// G4WorkerRunManagerKernel and calls G4EventManager::ProcessOneEvent with a
// manually-built G4Event. Inspired by the CMSSW OscarMTProducer pattern.
//
// Architecture:
//   - Master init (G4MTRunManager, geometry/physics) runs on the process-wide
//     ship::geometry_thread(), shared with every other Geant4 geometry user
//     (e.g. aegir-genie's GENIE geometry analyzer): Geant4 permits only one
//     geometry-creating thread per process (aegir-genie issue #11)
//   - Per phlex-thread: G4WorkerRunManagerKernel + G4EventManager for tracking
//   - No G4VUserPrimaryGeneratorAction — events built directly from MCParticle

#include <Random123/philox.h>
#include <spdlog/spdlog.h>

#include <G4Event.hh>
#include <G4EventManager.hh>
#include <G4GDMLParser.hh>
#include <G4GeometryManager.hh>
#include <G4LogicalVolumeStore.hh>
#include <G4MTRunManager.hh>
#include <G4ParticleDefinition.hh>
#include <G4ParticleTable.hh>
#include <G4PhysListFactory.hh>
#include <G4PhysicalVolumeStore.hh>
#include <G4PrimaryParticle.hh>
#include <G4PrimaryVertex.hh>
#include <G4RegionStore.hh>
#include <G4SolidStore.hh>
#include <G4StateManager.hh>
#include <G4Threading.hh>
#include <G4TransportationManager.hh>
#include <G4UImanager.hh>
#include <G4VUserActionInitialization.hh>
#include <G4VUserPrimaryGeneratorAction.hh>
#include <G4WorkerRunManagerKernel.hh>
#include <G4WorkerThread.hh>
#include <Randomize.hh>
#include <SHiP/MCParticle.hpp>
#include <SHiP/SimResult.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "FieldService/IFieldSource.h"
#include "GeometryService/GeometryThread.h"
#include "chrome_trace.hpp"
#include "detector_construction.hpp"
#include "geant4_sim_core.hpp"
#include "geometry_source.hpp"
#include "math_utils.hpp"
#include "phlex/core/product_selector.hpp"
#include "phlex/model/handle.hpp"
#include "phlex/module.hpp"
#include "phlex/utilities/max_allowed_parallelism.hpp"
#include "seed_config.hpp"
#include "units/clhep_bridge.hpp"
#include "units/config_units.hpp"

namespace {

using namespace SHiP::g4;

// Required by G4MTRunManager::Initialize but never invoked — events are
// built directly and passed to G4EventManager::ProcessOneEvent.
class StubPrimaryGenerator : public G4VUserPrimaryGeneratorAction {
 public:
  void GeneratePrimaries(G4Event*) override {}
};

class DirectActionInit : public G4VUserActionInitialization {
 public:
  void BuildForMaster() const override {}
  void Build() const override { SetUserAction(new StubPrimaryGenerator()); }
};

// Per-thread G4 worker kernel (leaked at shutdown — G4 singleton teardown
// is unsafe)
thread_local G4WorkerRunManagerKernel* tl_kernel = nullptr;

// Per-thread PDG code → G4ParticleDefinition cache
thread_local std::unordered_map<int, G4ParticleDefinition*> tl_pdg_cache;

// Moving the results out of tl_hits/tl_particles donates their buffers, so
// without a reserve every event regrows the vectors from zero capacity.
// Track per-thread high-water marks and pre-reserve to that size.
thread_local std::size_t tl_hits_hwm = 0;
thread_local std::size_t tl_particles_hwm = 0;

struct Geant4SimConfig {
  std::string physics_list = "FTFP_BERT";
  int verbosity = 0;
  int concurrency = 1;
  std::uint32_t seed = 0;
  SDMode sd_mode = SDMode::scoring;
  ship::Energy ke_threshold = ship::Energy::zero();
  bool energy_cut = false;
  ship::Energy energy_cut_threshold = ship::Energy::zero();
  ship::Energy particle_ke_cut = ship::Energy::zero();
  std::vector<std::pair<std::string, ship::Length>> regions;
  // When set, write the constructed geometry to this GDML file after
  // initialisation (e.g. to feed the same geometry to external tools).
  std::string export_gdml;
  // Log a progress line every this many simulated events (0 disables).
  int progress_interval = 100;
};

class Geant4Sim {
 public:
  explicit Geant4Sim(Geant4SimConfig cfg) : cfg_{std::move(cfg)} {}

  ~Geant4Sim() {
    if (!master_constructed_) {
      return;
    }
    if (int kernels = built_kernels_.load(); kernels > 0) {
      spdlog::info("geant4: {} worker kernel(s) built for {} configured slots",
                   kernels, cfg_.concurrency);
    }
    // Empty the geometry stores now, on the geometry thread. Their static
    // singletons' destructors run at process exit on the program's main
    // thread, which never initialised Geant4's thread-local split-class
    // data — so mass-deleting the still-registered volumes there
    // dereferences a null per-thread base (e.g. ~G4PVPlacement →
    // GetRotation → G4PVData) and segfaults (#68). The geometry thread ran
    // the master initialisation, so the same deletions are safe there, and
    // the stores' destructors are left with nothing to delete. The relative
    // order of the Clean() calls is immaterial: every CleanStore() locks
    // its objects before deleting them, which suppresses all cross-store
    // references (e.g. ~G4LogicalVolume skips its
    // fRegion->RemoveRootLogicalVolume backreference when locked).
    ship::geometry_thread().run([] {
      G4GeometryManager::GetInstance()->OpenGeometry();
      G4RegionStore::Clean();
      G4PhysicalVolumeStore::Clean();
      G4LogicalVolumeStore::Clean();
      G4SolidStore::Clean();
    });
    // The run manager is intentionally leaked (G4 singleton teardown is
    // unsafe).
  }

  SHiP::SimResult simulate(
      std::shared_ptr<SHiP::IGeometrySource> const& geo,
      std::shared_ptr<ship::IFieldSource> const& field,
      phlex::handle<std::vector<SHiP::MCParticle>> particles) {
    AEGIR_TRACE_EVENT("g4", "simulate");

    // A failed master init is sticky: retrying would construct a second
    // G4MTRunManager next to the leaked first one and die on Geant4's fatal
    // "constructed twice" abort, masking the original error. Store the
    // exception instead and rethrow it on this and every later call — phlex
    // logs it at shutdown and terminates the job cleanly.
    std::call_once(init_flag_, [this, &geo, &field] {
      try {
        init_master(geo, field);
      } catch (std::exception const& e) {
        spdlog::error("geant4_module: master initialisation failed: {}",
                      e.what());
        init_error_ = std::current_exception();
      } catch (...) {
        init_error_ = std::current_exception();
      }
    });
    if (init_error_) {
      std::rethrow_exception(init_error_);
    }

    if (!tl_kernel) {
      init_worker();
    }

    // Seed the calling worker's engine from the data-cell index so the
    // event↔RNG pairing does not depend on which thread processes which
    // event, or in what order — any -j then reproduces bitwise.
    auto const& index = particles.data_cell_index();
    seed_engine(index.hash());

    tl_hits.clear();
    tl_particles.clear();
    tl_track_map.clear();
    tl_hits.reserve(tl_hits_hwm);
    tl_particles.reserve(std::max(tl_particles_hwm, particles->size()));

    // unique_ptr so a throw from ProcessOneEvent below can't leak the event
    // (and its primary vertices/particles); G4 does not take ownership.
    // The event id is the index number within the parent layer — stable
    // across runs, unlike an arrival-order counter. It is globally unique
    // only under the current flat event-under-job layout (a nested layout
    // would repeat numbers across parents); the hash-based seed above is
    // what needs cross-hierarchy uniqueness. G4int is 32-bit, so the id
    // saturates past ~2^31 events — far beyond any single run here.
    auto event = std::make_unique<G4Event>(static_cast<G4int>(index.number()));
    {
      AEGIR_TRACE_EVENT("g4", "build_primaries");
      std::size_t unknown_pdg = 0;
      std::size_t no_momentum = 0;
      for (auto const& mc : *particles) {
        auto [it, inserted] = tl_pdg_cache.try_emplace(mc.pdgCode, nullptr);
        if (inserted) {
          it->second =
              G4ParticleTable::GetParticleTable()->FindParticle(mc.pdgCode);
          // Warn once per unseen code (per thread): FindParticle also returns
          // null for nuclear (10-digit) PDG codes, which would need
          // G4IonTable::GetIon() to resolve.
          if (!it->second) {
            spdlog::warn(
                "geant4_module: no G4 particle definition for PDG code {} — "
                "such particles are skipped",
                mc.pdgCode);
          }
        }
        auto const* def = it->second;
        if (!def) {
          ++unknown_pdg;
          continue;
        }
        auto const mom = ship::view::momentum(mc);
        if (aegir::magnitude(mom) <= ship::Momentum::zero()) {
          ++no_momentum;
          continue;
        }

        auto const vtx = ship::view::vertex(mc);
        namespace cb = aegir::clhep;
        auto* vertex =
            new G4PrimaryVertex(cb::g4(vtx[0]), cb::g4(vtx[1]), cb::g4(vtx[2]),
                                cb::g4(ship::view::time(mc)));
        auto* particle = new G4PrimaryParticle(def, cb::g4(mom[0]),
                                               cb::g4(mom[1]), cb::g4(mom[2]));
        // The particle weight becomes the G4Track weight, which Geant4 hands
        // on to every secondary; SimParticle records it.
        particle->SetWeight(mc.weight);
        vertex->SetPrimary(particle);
        event->AddPrimaryVertex(vertex);
      }
      if (unknown_pdg > 0 || no_momentum > 0) {
        spdlog::warn(
            "geant4_module: event {}: skipped {} of {} primaries ({} unknown "
            "PDG, {} non-positive momentum) — output mc_particles still "
            "contains them",
            event->GetEventID(), unknown_pdg + no_momentum, particles->size(),
            unknown_pdg, no_momentum);
      }
    }

    // G4EventManager expects G4State_GeomClosed; it transitions to
    // G4State_EventProc internally and back when done.
    auto* state_mgr = G4StateManager::GetStateManager();
    state_mgr->SetNewState(G4State_GeomClosed);
    {
      AEGIR_TRACE_EVENT("g4", "ProcessOneEvent");
      tl_kernel->GetEventManager()->ProcessOneEvent(event.get());
    }
    state_mgr->SetNewState(G4State_GeomClosed);

    event.reset();

    SHiP::SimResult result;
    {
      AEGIR_TRACE_EVENT("g4", "flush_hits");
      AEGIR_TRACE_COUNTER("g4", "hits", tl_hits.size());
      AEGIR_TRACE_COUNTER("g4", "particles", tl_particles.size());
      tl_hits_hwm = std::max(tl_hits_hwm, tl_hits.size());
      tl_particles_hwm = std::max(tl_particles_hwm, tl_particles.size());
      result.hits = std::move(tl_hits);
      result.particles = std::move(tl_particles);
    }

    auto const done = completed_.fetch_add(1) + 1;
    if (cfg_.progress_interval > 0 &&
        done % static_cast<std::size_t>(cfg_.progress_interval) == 0) {
      auto const elapsed = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - sim_start_)
                               .count();
      spdlog::info("geant4_module: {} events simulated ({:.1f} events/s)", done,
                   elapsed > 0 ? done / elapsed : 0.0);
    }

    return result;
  }

 private:
  // Derive a per-data-cell engine seed with Philox, following the
  // counter-based convention of src/philox_rng.hpp: the key selects the
  // stream (config seed + a G4-specific constant so detector simulation and
  // event generation draw uncorrelated sequences), the counter is the full
  // index-path hash (unique across hierarchy levels, unlike number()).
  void seed_engine(std::size_t index_hash) const {
    r123::Philox4x32 const philox;
    r123::Philox4x32::key_type const key{{cfg_.seed, 0x47345EEDu}};
    r123::Philox4x32::ctr_type const ctr{
        {static_cast<std::uint32_t>(index_hash),
         static_cast<std::uint32_t>(index_hash >> 32), 0, 0}};
    auto const out = philox(ctr, key);
    // >> 1 clears the sign bit so the value always fits a non-negative long
    // (it can be 0, which the engine accepts).
    auto const seed64 =
        (static_cast<std::uint64_t>(out[1]) << 32 | out[0]) >> 1;
    G4Random::getTheEngine()->setSeed(static_cast<long>(seed64), 0);
  }

  void init_master(std::shared_ptr<SHiP::IGeometrySource> const& geo,
                   std::shared_ptr<ship::IFieldSource> const& field) {
    // Validate the export target up front, before anything is constructed:
    // G4GDMLParser::Write aborts via G4Exception on some write failures, so
    // pre-check the cases we can to fail with a catchable, clear error
    // instead. Only the existing-file and missing-parent-directory cases are
    // validated here — other fatal write errors (e.g. a read-only directory)
    // remain Geant4's responsibility. Failing before the G4MTRunManager is
    // constructed keeps the failure cheap: nothing has been built yet.
    if (!cfg_.export_gdml.empty()) {
      if (std::filesystem::exists(cfg_.export_gdml)) {
        throw std::runtime_error(
            "geant4_module: export_gdml target '" + cfg_.export_gdml +
            "' already exists — remove it or choose another path");
      }
      auto const parent = std::filesystem::path(cfg_.export_gdml).parent_path();
      if (!parent.empty() && !std::filesystem::exists(parent)) {
        throw std::runtime_error(
            "geant4_module: export_gdml target directory '" + parent.string() +
            "' does not exist");
      }
    }

    // Validate the physics list before constructing anything. Like export_gdml
    // above, GetReferencePhysList raises a fatal G4Exception for an unknown
    // list, so query IsReferencePhysList here — before detector_ or the run
    // manager exist — to fail with a catchable, clear error instead of aborting
    // the process. Failing this early also keeps the failure cheap: nothing has
    // been built yet. G4PhysListFactory only consults a static name table (it
    // creates no geometry), so validating on the calling thread is safe.
    G4PhysListFactory const phys_factory;
    if (!phys_factory.IsReferencePhysList(cfg_.physics_list)) {
      throw std::runtime_error("Unknown physics list: " + cfg_.physics_list);
    }

    field_ = field;  // keep alive for the G4 run
    detector_ = new ConfigurableDetectorConstruction(
        *geo, *field, cfg_.sd_mode, cfg_.ke_threshold, cfg_.regions);

    // Master initialisation runs on the process-wide geometry thread: only
    // one thread per process may create Geant4 geometry (G4GeomSplitter's
    // slot count is shared while its data array is thread-local), and other
    // plugins — e.g. aegir-genie's GENIE geometry analyzer — create theirs
    // on this thread too (aegir-genie issue #11, Geant4 bug #2747). The call
    // blocks until initialisation is done; exceptions propagate to the
    // caller in simulate(), which records them as a sticky init failure.
    ship::geometry_thread().run([this] {
      AEGIR_TRACE_THREAD_NAME("g4_master");
      AEGIR_TRACE_EVENT("g4", "init_master");
      auto* rm = new G4MTRunManager();
      master_constructed_ = true;
      rm->SetNumberOfThreads(cfg_.concurrency);
      rm->SetUserInitialization(detector_);

      // Physics list already validated in init_master before the run manager
      // was constructed, so GetReferencePhysList won't hit its unknown-list
      // abort.
      G4PhysListFactory factory;
      auto* physics = factory.GetReferencePhysList(cfg_.physics_list);
      rm->SetUserInitialization(physics);

      rm->SetUserInitialization(new DirectActionInit());

      auto* ui = G4UImanager::GetUIpointer();
      ui->ApplyCommand("/run/verbose " + std::to_string(cfg_.verbosity));
      ui->ApplyCommand("/event/verbose 0");
      ui->ApplyCommand("/tracking/verbose 0");

      rm->Initialize();
      rm->RunInitialization();

      world_pv_ = G4TransportationManager::GetTransportationManager()
                      ->GetNavigatorForTracking()
                      ->GetWorldVolume();
      physics_list_ = physics;

      if (!cfg_.export_gdml.empty()) {
        // The target path was validated at the top of init_master, before the
        // run manager was constructed.
        G4GDMLParser parser;
        parser.Write(cfg_.export_gdml, world_pv_);
        spdlog::info("geant4_module: geometry exported to {}",
                     cfg_.export_gdml);
      }
      // The run manager stays alive (and leaked) for the whole process.
    });

    sim_start_ = std::chrono::steady_clock::now();

    spdlog::info("Geant4 direct simulation ready ({} worker slots)",
                 cfg_.concurrency);
  }

  void init_worker() {
    AEGIR_TRACE_EVENT("g4", "init_worker");
    int const id = next_thread_id_.fetch_add(1);
    // TBB does not promise that the same OS threads serve this transform
    // for the whole run, so more kernels than configured slots can appear
    // (each new thread that ever runs simulate builds one). Geant4 11
    // tolerates thread ids beyond SetNumberOfThreads in this direct-
    // injection flow, but each extra kernel costs memory and a fresh RNG
    // stream — make it visible instead of silent.
    if (id >= cfg_.concurrency) {
      spdlog::warn(
          "geant4: initialising worker kernel #{} beyond the {} configured "
          "slots (TBB thread churn)",
          id + 1, cfg_.concurrency);
    }
    AEGIR_TRACE_THREAD_NAME("g4_worker_" + std::to_string(id));
    G4Threading::G4SetThreadId(id);
    G4WorkerThread::BuildGeometryAndPhysicsVector();

    tl_kernel = new G4WorkerRunManagerKernel();
    tl_kernel->WorkerDefineWorldVolume(world_pv_);

    physics_list_->InitializeWorker();
    tl_kernel->SetPhysics(physics_list_);
    tl_kernel->InitializePhysics();

    detector_->ConstructSDandField();

    auto* evt_mgr = tl_kernel->GetEventManager();
    evt_mgr->SetUserAction(new TrackingAction(cfg_.particle_ke_cut));
    if (cfg_.energy_cut) {
      evt_mgr->SetUserAction(new EnergyCutAction(cfg_.energy_cut_threshold));
    }

    tl_kernel->RunInitialization();
    G4StateManager::GetStateManager()->SetNewState(G4State_GeomClosed);

    // Count only here, once the kernel is fully built: next_thread_id_ is
    // bumped on entry (to allocate the thread id) and would overcount a worker
    // whose init_worker threw partway through.
    built_kernels_.fetch_add(1);
  }

  Geant4SimConfig cfg_;

  std::once_flag init_flag_;
  // Written at most once inside the call_once body, read only after the
  // call_once returns — no further synchronisation needed.
  std::exception_ptr init_error_;
  G4VPhysicalVolume* world_pv_ = nullptr;
  G4VUserPhysicsList* physics_list_ = nullptr;
  ConfigurableDetectorConstruction* detector_ = nullptr;  // owned by G4
  std::shared_ptr<ship::IFieldSource> field_;             // outlives G4 run
  std::atomic<int> next_thread_id_{0};
  // Kernels that finished init_worker successfully (unlike next_thread_id_,
  // which counts allocated ids). Reported in the shutdown summary.
  std::atomic<int> built_kernels_{0};
  // Completed-event count (completion order, not event id, so the logged
  // count is monotonic) and the reference point for the average event rate.
  // sim_start_ is written in init_master and published by the call_once
  // every simulate call passes through first.
  std::atomic<std::size_t> completed_{0};
  std::chrono::steady_clock::time_point sim_start_;
  // Set on the geometry thread as soon as the G4MTRunManager exists. From
  // that point the geometry stores may hold volumes — even when a later init
  // step throws, detector construction has typically already registered them
  // — so the destructor must clean the stores on the geometry thread (#68)
  // for failed and successful inits alike. Guarding on full init success
  // instead would leave the stores populated after a failed init and their
  // static destructors would segfault at process exit.
  bool master_constructed_ = false;
};

}  // namespace

PHLEX_REGISTER_ALGORITHMS(m, config) {
  using namespace phlex;

  namespace su = ship::units;

  auto const sd_mode_str =
      config.get<std::string>("sd_mode", std::string{"scoring"});
  auto const ke_threshold =
      aegir::get_quantity(config, "ke_threshold", 0.0 * su::GeV);
  auto const regions_map = config.get<std::map<std::string, double>>(
      "regions", std::map<std::string, double>{});
  std::vector<std::pair<std::string, ship::Length>> regions;
  regions.reserve(regions_map.size());
  for (auto const& [pattern, cut_mm] : regions_map) {
    regions.emplace_back(pattern, cut_mm * su::mm);
  }

  // Default to the framework's TBB parallelism (phlex -j) so G4 workers
  // match the threads that can actually run them. A configured value above
  // the framework parallelism starves the graph: the extra worker slots
  // never run concurrently but G4 still initialises kernels for them.
  auto const active_parallelism =
      static_cast<int>(detail::max_allowed_parallelism::active_value());

  auto const seed = aegir::resolve_seed(config, "geant4");

  Geant4SimConfig cfg{
      .physics_list =
          config.get<std::string>("physics_list", std::string{"FTFP_BERT"}),
      .verbosity = config.get<int>("verbosity", 0),
      .concurrency = config.get<int>("concurrency", int{active_parallelism}),
      .seed = seed,
      .sd_mode = sd_mode_str == "crossing" ? SDMode::crossing : SDMode::scoring,
      .ke_threshold = ke_threshold,
      .energy_cut = config.get<bool>("energy_cut", false),
      .energy_cut_threshold = aegir::get_quantity(
          config, "energy_cut_threshold", ship::Energy{ke_threshold}),
      .particle_ke_cut =
          aegir::get_quantity(config, "particle_ke_cut", 0.0 * su::GeV),
      .regions = std::move(regions),
      .export_gdml = config.get<std::string>("export_gdml", std::string{}),
      .progress_interval = config.get<int>("progress_interval", 100),
  };

  if (cfg.concurrency > active_parallelism) {
    spdlog::warn(
        "geant4 concurrency ({}) exceeds framework parallelism ({}); "
        "at most {} events run concurrently — raise phlex -j or lower "
        "the module's concurrency",
        cfg.concurrency, active_parallelism, active_parallelism);
  }

  auto const num_threads = cfg.concurrency;
  auto g4 = m.make<Geant4Sim>(std::move(cfg));

  g4.transform("simulate", &Geant4Sim::simulate,
               concurrency{static_cast<std::size_t>(num_threads)})
      .input_family(
          product_selector{.creator = "geometry"_id, .layer = "job"_id},
          product_selector{.creator = "field"_id, .layer = "job"_id},
          product_selector{.creator = "mc_particles"_id, .layer = "event"_id})
      .output_product_suffixes("sim_result");
}
