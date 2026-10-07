// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// muon_back_source.cpp — Phlex source plugin reading FairShip muon-background
// files
//
// A port of FairShip's MuonBackGenerator (shipgen/MuonBackGenerator.cxx), as
// run_simScript.py --MuonBack uses it. Three input formats, told apart as
// FairShip does:
//
//   - a "pythia8-Geant4" tree of plain floats (id, px, ..., w): one muon per
//     entry; entries that are not muons (neutrinos) give no particle.
//   - a "cbmsim" tree with an MCTrack and a PlaneHAPoint branch (current
//     production) or a vetoPoint branch (old production): the MCTrack record
//     of the target + hadron-absorber simulation and the particles recorded
//     on the absorber exit plane. Every muon on the plane is published,
//     starting from its MCTrack start (its production vertex in the target).
//
// The branches are bound to plain arrays (TTree::SetMakeClass), so no FairShip
// classes or libraries are needed.
//
// Each event is one input entry, and its EventHeader carries the entry as
// original_event_id.
//
// Weights are set per muon in the production, not per proton interaction:
// the rare muon sources are enhanced (Pythia8 BRs of eta, omega, phi, rho0,
// eta' -> mu mu by --boostDiMuon; Geant4 gamma -> mu mu and e+ e- -> mu mu
// cross sections by --boostFactor), and extractMuonsAndUpdateWeight.py
// divides the weight of each muon from such a process by the boost. One
// entry can therefore hold a pi/K-decay muon of weight 1 next to a muon of
// weight 0.01 (about 0.08% of the entries of the normalised 2026 MinBias
// files), each weight being correct for its own muon only. The weight of each
// muon is therefore stored in its MCParticle, as FairShip keeps it per track;
// the files carry no per-event weight, so the EventHeader weight is left at
// 1. The full weight of a muon is the product of the two. Observables built
// from several muons of one entry need the product of their weights.
//
// Differences from FairShip, and why:
//   - FairShip puts the whole MCTrack record on its stack and transports only
//     the muons. geant4_module transports everything it is given, so only the
//     muons are published.
//   - The random draws (beam smearing and painting, phi rotation) follow
//     FairShip's distributions and order, but come from a Philox stream keyed
//     on (seed, input entry) instead of gRandom: each entry is reproducible on
//     its own, as with FairShip's --sameSeed.
//   - Event n is input entry first_entry + n, as in genie_reader_source. An
//     entry without a muon gives an event with no particles, where FairShip
//     skips to the next entry with a muon.
//   - Running past the last entry throws instead of stopping the run.

#include <TBranchElement.h>
#include <TFile.h>
#include <TTree.h>
#include <spdlog/spdlog.h>

#include <SHiP/EventHeader.hpp>
#include <SHiP/MCParticle.hpp>
#include <SHiP/QuantityView.hpp>
#include <SHiP/Units.hpp>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "mc_particle_source.hpp"
#include "philox_rng.hpp"
#include "seed_config.hpp"
#include "units/config_units.hpp"

namespace {

namespace su = ship::units;

constexpr int kMuon = 13;
constexpr double kMuonMass = 0.1056583755;  // GeV

// Target position run_simScript.py gives FairPrimaryGenerator, which adds it
// to every vertex: 0 for the current production (PlaneHAPoint), 70.845 m
// for the older ones.
constexpr double kOldProductionZOffsetCm = 7084.5;

// 0x4D55424B ("MUBK"): this source's Philox stream, independent of the gun
// (0xBEEFCAFE), fixed_target (0xF14ED0A7) and geant4 (0x47345EED).
constexpr std::uint32_t kStreamKey = 0x4D55424B;

// Gaussian draw with mean 0 (Box-Muller, cosine branch).
double gaussian(aegir::PhiloxRng& rng, double sigma) {
  double const u1 = 1.0 - rng.uniform();
  double const u2 = rng.uniform();
  return sigma * std::sqrt(-2.0 * std::log(u1)) *
         std::cos(2.0 * std::numbers::pi * u2);
}

struct Options {
  double smear_beam_cm = 0.8;  // FairShip --SmearBeam
  double paint_beam_cm = 5.0;  // FairShip --PaintBeam
  bool phi_randomize = false;  // FairShip --phiRandom
  std::optional<double> z_offset_cm;
};

struct Event {
  std::vector<SHiP::MCParticle> particles;
  SHiP::EventHeader header;
};

class MuonBackSource : public phlex::source {
 public:
  MuonBackSource(std::string const& file, long long first_entry, Options opts,
                 std::uint32_t seed)
      : file_name_{file}, first_entry_{first_entry}, opts_{opts}, seed_{seed} {
    file_.reset(TFile::Open(file.c_str(), "READ"));
    if (!file_ || file_->IsZombie()) {
      throw std::runtime_error("muon_back_source: cannot open '" + file + "'");
    }
    tree_ = file_->Get<TTree>("pythia8-Geant4");
    flat_format_ = tree_ != nullptr;
    if (!flat_format_) {
      tree_ = file_->Get<TTree>("cbmsim");
    }
    if (!tree_) {
      throw std::runtime_error("muon_back_source: '" + file +
                               "' has neither a pythia8-Geant4 nor a cbmsim "
                               "tree");
    }
    if (first_entry < 0 || first_entry >= tree_->GetEntries()) {
      throw std::runtime_error(
          "muon_back_source: first_entry " + std::to_string(first_entry) +
          " is out of range for " + std::to_string(tree_->GetEntries()) +
          " entries");
    }

    tree_->SetBranchStatus("*", false);
    if (flat_format_) {
      bind_flat();
    } else {
      bind_cbmsim();
    }

    bool const old_production = !flat_format_ && point_branch_ == "vetoPoint";
    z_offset_cm_ = opts_.z_offset_cm.value_or(
        (flat_format_ || old_production) ? kOldProductionZOffsetCm : 0.0);
    spdlog::info(
        "muon_back_source: '{}' ({}): {} entries, reading from entry {}; z "
        "offset {} cm",
        file, flat_format_ ? "pythia8-Geant4" : "cbmsim/" + point_branch_,
        tree_->GetEntries(), first_entry, z_offset_cm_);
  }

  phlex::detail::provider_bundles create_providers(
      phlex::product_selector const& selector) override {
    return aegir::mc_particle_provider_bundles(
        selector,
        [this](phlex::data_cell_index const& id) {
          std::scoped_lock const lock{mutex_};
          return event(id.number()).particles;
        },
        phlex::concurrency::serial,
        [this](phlex::data_cell_index const& id) {
          std::scoped_lock const lock{mutex_};
          return event(id.number()).header;
        });
  }

  phlex::index_generator indices() override { co_return; }

 private:
  // ── reading the file ───────────────────────────────────────────────────

  // Enable a branch and point it at our buffer.
  void bind(std::string const& name, void* address) {
    if (!tree_->GetBranch(name.c_str())) {
      throw std::runtime_error("muon_back_source: branch '" + name +
                               "' missing from '" + file_name_ + "'");
    }
    tree_->SetBranchStatus(name.c_str(), true);
    if (tree_->SetBranchAddress(name.c_str(), address) < 0) {
      throw std::runtime_error("muon_back_source: cannot read branch '" + name +
                               "' of '" + file_name_ + "'");
    }
  }

  // pythia8-Geant4: one muon per entry. Newer ntuples also store the
  // vertex and momentum at the origin (ox, opx, ...), which FairShip prefers.
  void bind_flat() {
    bool const has_origin = tree_->GetListOfLeaves()->GetSize() >= 17;
    std::string const o = has_origin ? "o" : "";
    bind("id", &flat_.id);
    bind("w", &flat_.w);
    bind(o + "x", &flat_.x);
    bind(o + "y", &flat_.y);
    bind(o + "z", &flat_.z);
    bind(o + "px", &flat_.px);
    bind(o + "py", &flat_.py);
    bind(o + "pz", &flat_.pz);
  }

  // cbmsim: the MCTrack and plane-point collections are split into one branch
  // per data member. In MakeClass mode each member branch reads into a plain
  // array and the collection branch itself into its size.
  void bind_cbmsim() {
    point_branch_ =
        tree_->GetBranch("PlaneHAPoint") ? "PlaneHAPoint" : "vetoPoint";
    tree_->SetMakeClass(1);

    // The largest collection in the file sizes the arrays.
    mc_.resize(max_size("MCTrack"));
    points_.resize(max_size(point_branch_));

    bind("MCTrack", &mc_.n);
    bind("MCTrack.fPdgCode", mc_.pdg.data());
    bind("MCTrack.fPx", mc_.px.data());
    bind("MCTrack.fPy", mc_.py.data());
    bind("MCTrack.fPz", mc_.pz.data());
    bind("MCTrack.fM", mc_.mass.data());
    bind("MCTrack.fStartX", mc_.x.data());
    bind("MCTrack.fStartY", mc_.y.data());
    bind("MCTrack.fStartZ", mc_.z.data());
    bind("MCTrack.fStartT", mc_.t.data());
    bind("MCTrack.fW", mc_.weight.data());
    bind(point_branch_, &points_.n);
    bind(point_branch_ + ".fPdgCode", points_.pdg.data());
    bind(point_branch_ + ".fTrackID", points_.track.data());
  }

  int max_size(std::string const& collection) const {
    auto const* branch = dynamic_cast<TBranchElement const*>(
        tree_->GetBranch(collection.c_str()));
    if (!branch) {
      throw std::runtime_error("muon_back_source: no '" + collection +
                               "' collection in '" + file_name_ + "'");
    }
    return branch->GetMaximum();
  }

  void read(long long entry) {
    if (entry != loaded_entry_) {
      tree_->GetEntry(entry);
      loaded_entry_ = entry;
    }
  }

  // ── building an event (the AddTrack part of ReadEvent) ─────────────────

  // cbmsim: the MCTracks of the muons on the plane; a track crossing twice
  // counts once.
  std::vector<bool> muons_on_plane() const {
    std::vector<bool> on_plane(static_cast<std::size_t>(mc_.n), false);
    for (int j = 0; j < points_.n; ++j) {
      int const track = points_.track[j];
      if (std::abs(points_.pdg[j]) == kMuon && track >= 0 && track < mc_.n) {
        on_plane[static_cast<std::size_t>(track)] = true;
      }
    }
    return on_plane;
  }

  // Event n is input entry first_entry_ + n. Both providers ask for the same
  // event, so the last one is kept. Caller holds mutex_.
  Event const& event(std::size_t n) {
    long long const entry = first_entry_ + static_cast<long long>(n);
    if (entry >= tree_->GetEntries()) {
      throw std::runtime_error(
          "muon_back_source: input exhausted — the workflow requested entry " +
          std::to_string(entry) + " but '" + file_name_ + "' holds only " +
          std::to_string(tree_->GetEntries()) +
          " entries. Reduce the driver's event count or provide a larger "
          "file.");
    }
    if (entry != built_entry_) {
      read(entry);
      aegir::PhiloxRng rng{seed_, kStreamKey,
                           static_cast<std::uint32_t>(entry)};
      built_ = flat_format_ ? build_flat(rng) : build_cbmsim(rng);
      built_.header.original_event_id = entry;
      built_entry_ = entry;
    }
    return built_;
  }

  // FairShip's CalculateBeamOffset: Gaussian smearing plus a random point on
  // the painting circle, in cm.
  std::pair<double, double> beam_offset(aegir::PhiloxRng& rng) const {
    double dx = 0.0;
    double dy = 0.0;
    if (opts_.smear_beam_cm > 0) {
      dx = gaussian(rng, opts_.smear_beam_cm);
      dy = gaussian(rng, opts_.smear_beam_cm);
    }
    if (opts_.paint_beam_cm > 0) {
      double const phi = rng.uniform(0.0, 2.0 * std::numbers::pi);
      dx += opts_.paint_beam_cm * std::cos(phi);
      dy += opts_.paint_beam_cm * std::sin(phi);
    }
    return {dx, dy};
  }

  // Rotate the transverse momentum to a random azimuth (--phiRandom).
  static void rotate_phi(double& px, double& py, aegir::PhiloxRng& rng) {
    double const phi = rng.uniform(0.0, 2.0 * std::numbers::pi);
    double const pt = std::sqrt((px * px) + (py * py));
    px = pt * std::cos(phi);
    py = pt * std::sin(phi);
  }

  // A muon in FairShip's units (cm, GeV, ns), converted to aegir's, with its
  // FairShip weight.
  SHiP::MCParticle muon(int pdg, double x, double y, double z, double px,
                        double py, double pz, double mass, double t,
                        double weight) const {
    double const e =
        std::sqrt((px * px) + (py * py) + (pz * pz) + (mass * mass));
    SHiP::MCParticle mc;
    mc.pdgCode = pdg;
    ship::view::setVertex(
        mc, {x * su::cm, y * su::cm, (z + z_offset_cm_) * su::cm});
    ship::view::setMomentum(
        mc, {px * su::GeV_per_c, py * su::GeV_per_c, pz * su::GeV_per_c});
    ship::view::setEnergy(mc, e * su::GeV);
    ship::view::setTime(mc, t * su::ns);
    mc.motherId = -1;
    mc.status = 1;
    mc.weight = weight;
    return mc;
  }

  Event build_flat(aegir::PhiloxRng& rng) const {
    Event ev;
    if (std::abs(static_cast<int>(flat_.id)) != kMuon) {
      return ev;
    }
    auto const [dx, dy] = beam_offset(rng);
    double px = flat_.px;
    double py = flat_.py;
    if (opts_.phi_randomize) {
      rotate_phi(px, py, rng);
    }
    // The ntuple stores positions in m.
    double const x = (flat_.x * 100.) + dx;
    double const y = (flat_.y * 100.) + dy;
    double const z = flat_.z * 100.;
    ev.particles.push_back(muon(static_cast<int>(flat_.id), x, y, z, px, py,
                                flat_.pz, kMuonMass, 0.0, flat_.w));
    return ev;
  }

  Event build_cbmsim(aegir::PhiloxRng& rng) const {
    std::vector<bool> const on_plane = muons_on_plane();
    auto const [dx, dy] = beam_offset(rng);
    Event ev;
    for (int i = 0; i < mc_.n; ++i) {
      double px = mc_.px[i];
      double py = mc_.py[i];
      // FairShip draws a phi for every MCTrack, transported or not.
      if (opts_.phi_randomize) {
        rotate_phi(px, py, rng);
      }
      if (!on_plane[static_cast<std::size_t>(i)]) {
        continue;
      }
      ev.particles.push_back(muon(mc_.pdg[i], mc_.x[i] + dx, mc_.y[i] + dy,
                                  mc_.z[i], px, py, mc_.pz[i], mc_.mass[i],
                                  mc_.t[i], mc_.weight[i]));
    }
    return ev;
  }

  // ── state ──────────────────────────────────────────────────────────────

  std::string file_name_;
  long long first_entry_;
  Options opts_;
  std::uint32_t seed_;
  double z_offset_cm_ = 0.0;

  // The tree belongs to the file, which must outlive it.
  std::unique_ptr<TFile> file_;
  TTree* tree_ = nullptr;
  bool flat_format_ = false;
  std::string point_branch_;
  long long loaded_entry_ = -1;

  // pythia8-Geant4 buffers (Float_t branches).
  struct {
    float id = 0, w = 0, x = 0, y = 0, z = 0, px = 0, py = 0, pz = 0;
  } flat_;

  // cbmsim buffers: one array per MCTrack member (Double32_t is read as
  // double), sized to the largest collection in the file.
  struct Tracks {
    int n = 0;
    std::vector<int> pdg;
    std::vector<double> px, py, pz, mass, x, y, z, t, weight;
    void resize(int size) {
      auto const s = static_cast<std::size_t>(size);
      pdg.resize(s);
      for (auto* v : {&px, &py, &pz, &mass, &x, &y, &z, &t, &weight}) {
        v->resize(s);
      }
    }
  } mc_;

  struct Points {
    int n = 0;
    std::vector<int> pdg, track;
    void resize(int size) {
      pdg.resize(static_cast<std::size_t>(size));
      track.resize(static_cast<std::size_t>(size));
    }
  } points_;

  long long built_entry_ = -1;
  Event built_;

  // Serialises the reads of the two providers (TTree is not thread-safe).
  std::mutex mutex_;
};

}  // namespace

PHLEX_REGISTER_SOURCE(s, config) {
  auto const file = config.get<std::string>("input_file");
  auto const first_entry = config.get<long>("first_entry", 0L);

  Options opts;
  opts.smear_beam_cm = aegir::get_quantity(config, "smear_beam", 8.0 * su::mm)
                           .numerical_value_in(su::cm);
  opts.paint_beam_cm = aegir::get_quantity(config, "paint_beam", 50.0 * su::mm)
                           .numerical_value_in(su::cm);
  opts.phi_randomize = config.get<bool>("phi_randomize", false);
  if (auto const z = config.get_if_present<double>("z_offset")) {
    opts.z_offset_cm = (*z * su::mm).numerical_value_in(su::cm);
  }

  auto seed = aegir::resolve_seed(config, "muon_back_source");

  s.add_source<MuonBackSource>("muon_back", file,
                               static_cast<long long>(first_entry), opts, seed);
}
