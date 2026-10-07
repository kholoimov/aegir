// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// file_source.cpp — Phlex source plugin
//
// Reads pre-generated events from a ROOT RNTuple, one entry per event, and
// publishes them as the "mc_particles" product. Each entry holds a
// std::vector, so the number of particles per event is arbitrary. This fills
// the role of FairShip's MuonBackGenerator / TTreeGenerator and enables a
// file-driven particle gun.
//
// The RNTuple layout matches what sim_output_module writes: a tuple (default
// name "events") with a std::vector<SHiP::MCParticle> field "mc_particles"
// and, in full-simulation output, a std::vector<SHiP::SimParticle> field
// "sim_particles". SimParticles are simulation *output*, so when reading them
// back as simulation *input* they are projected onto MCParticles.

#include <spdlog/spdlog.h>

#include <ROOT/RNTupleDescriptor.hxx>
#include <ROOT/RNTupleReader.hxx>
#include <ROOT/RNTupleView.hxx>
#include <SHiP/EventHeader.hpp>
#include <SHiP/MCParticle.hpp>
#include <SHiP/QuantityView.hpp>
#include <SHiP/SimParticle.hpp>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "math_utils.hpp"
#include "mc_particle_source.hpp"

namespace {

namespace su = ship::units;

// Project a simulation SimParticle onto an MCParticle suitable as sim input.
// SimParticle::energy is kinetic while MCParticle::energy is total; recover the
// total energy from |p|c and the kinetic energy without needing a PDG mass
// table: E_total = ((|p|c)^2 + E_kin^2) / (2 E_kin), from
// mc^2 = ((|p|c)^2 - E_kin^2) / (2 E_kin) and E_total = E_kin + mc^2.
SHiP::MCParticle to_mc_particle(SHiP::SimParticle const& sp) {
  SHiP::MCParticle mc;
  mc.pdgCode = sp.pdgCode;
  // Same storage units on both sides — direct copies, no unit crossing.
  mc.vertex = sp.vertex;
  mc.momentum = sp.momentum;
  auto const pc = (aegir::magnitude(ship::view::momentum(sp)) * su::c)
                      .in(su::GeV);           // |p|c; conversion factor is 1
  auto const e_kin = ship::view::energy(sp);  // SimParticle::energy is kinetic
  ship::view::setEnergy(mc, e_kin > ship::Energy::zero()
                                ? (pc * pc + e_kin * e_kin) / (2.0 * e_kin)
                                : pc);
  mc.time = sp.time;
  // SimParticle::parentId is the parent's track id (0 for primaries); map the
  // primary case to MCParticle's -1 convention.
  mc.motherId = sp.parentId == 0 ? -1 : sp.parentId;
  mc.status = 1;
  mc.weight = sp.weight;
  return mc;
}

class FileSource : public phlex::source {
 public:
  FileSource(std::string const& input_file, std::string const& ntuple,
             std::string const& product, long skip)
      : skip_{skip}, read_sim_{product == "sim_particles"} {
    // skip_ is cast to the unsigned ROOT::NTupleSize_t in generate(); reject a
    // negative skip here rather than let it wrap around to a huge offset.
    if (skip < 0) {
      throw std::runtime_error(
          "file_source: 'skip' must be non-negative, got " +
          std::to_string(skip));
    }

    if (product != "mc_particles" && product != "sim_particles") {
      throw std::runtime_error(
          "file_source: unknown product '" + product +
          "' (expected 'mc_particles' or 'sim_particles')");
    }

    reader_ = ROOT::RNTupleReader::Open(ntuple, input_file);
    n_entries_ = reader_->GetNEntries();
    if (read_sim_) {
      sim_view_ = reader_->GetView<std::vector<SHiP::SimParticle>>(product);
    } else {
      mc_view_ = reader_->GetView<std::vector<SHiP::MCParticle>>(product);
    }

    // The event header is optional: files written before it existed have no
    // such field, and GetView would throw. Probe the descriptor first and only
    // read it back when present, so older files still replay (publishing the
    // unweighted default header via the empty generator below). Warn in that
    // case, so a run that reports weight 1.0 because the input says so is
    // distinguishable from one where the header was simply not there to read.
    if (reader_->GetDescriptor().FindFieldId("event_header") !=
        ROOT::kInvalidDescriptorId) {
      header_view_ = reader_->GetView<SHiP::EventHeader>("event_header");
    } else {
      spdlog::warn(
          "file_source: '{}' has no 'event_header' field — publishing the "
          "unweighted default (weight 1.0, id -1)",
          input_file);
    }
  }

  std::vector<SHiP::MCParticle> generate(phlex::data_cell_index const& id) {
    auto const entry = static_cast<ROOT::NTupleSize_t>(id.number()) +
                       static_cast<ROOT::NTupleSize_t>(skip_);
    if (entry >= n_entries_) {
      spdlog::warn("file_source: entry {} out of range (file has {})", entry,
                   n_entries_);
      return {};
    }

    // The mc_particles and event_header providers become separate phlex nodes
    // that may run concurrently; a single RNTupleReader and its views are not
    // thread-safe, so all reads share io_mutex_. The view returns a reference
    // into its own buffer, so copy out while still holding the lock.
    std::scoped_lock const lock{io_mutex_};
    if (read_sim_) {
      auto const& sim_particles = (*sim_view_)(entry);
      std::vector<SHiP::MCParticle> particles;
      particles.reserve(sim_particles.size());
      for (auto const& sp : sim_particles) {
        particles.push_back(to_mc_particle(sp));
      }
      return particles;
    }
    return (*mc_view_)(entry);
  }

  SHiP::EventHeader generate_header(phlex::data_cell_index const& id) {
    auto const entry = static_cast<ROOT::NTupleSize_t>(id.number()) +
                       static_cast<ROOT::NTupleSize_t>(skip_);
    // Out-of-range mirrors generate()'s empty-particle fallback: publish the
    // unweighted default rather than reading past the end. create_providers
    // only installs this generator when the input carries the field, but fall
    // back the same way if there is no view to read.
    if (!header_view_ || entry >= n_entries_) {
      return SHiP::EventHeader{};
    }
    std::scoped_lock const lock{io_mutex_};
    return (*header_view_)(entry);
  }

  phlex::detail::provider_bundles create_providers(
      phlex::product_selector const& selector) override {
    // Read events serially (RNTupleReader/view is not thread-safe); io_mutex_
    // additionally guards the reader against the concurrent header provider.
    aegir::event_header_generator header_gen{};
    if (header_view_) {
      header_gen = [this](phlex::data_cell_index const& id) {
        return generate_header(id);
      };
    }
    return aegir::mc_particle_provider_bundles(
        selector,
        [this](phlex::data_cell_index const& id) { return generate(id); },
        phlex::concurrency::serial, std::move(header_gen));
  }

  phlex::index_generator indices() override { co_return; }

 private:
  long skip_;
  bool read_sim_;
  ROOT::NTupleSize_t n_entries_{0};
  std::unique_ptr<ROOT::RNTupleReader> reader_;
  std::optional<ROOT::RNTupleView<std::vector<SHiP::MCParticle>>> mc_view_;
  std::optional<ROOT::RNTupleView<std::vector<SHiP::SimParticle>>> sim_view_;
  // Set only when the input carries an "event_header" field; absent for files
  // written before the header existed.
  std::optional<ROOT::RNTupleView<SHiP::EventHeader>> header_view_;
  // Serialises all reader/view access across the concurrent mc_particles and
  // event_header provider nodes.
  std::mutex io_mutex_;
};

}  // namespace

PHLEX_REGISTER_SOURCE(s, config) {
  using namespace phlex;

  auto const input_file = config.get<std::string>("input_file");
  auto const ntuple = config.get<std::string>("ntuple", std::string{"events"});
  auto const product =
      config.get<std::string>("product", std::string{"mc_particles"});
  auto const skip =
      config.get<long>("skip", 0L);  // start reading at this entry

  s.add_source<FileSource>("file_source", input_file, ntuple, product, skip);
}
