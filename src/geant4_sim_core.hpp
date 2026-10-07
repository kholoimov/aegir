// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// geant4_sim_core.hpp — Shared G4 user action classes and thread-local storage

#pragma once

#include <G4LogicalVolume.hh>
#include <G4Step.hh>
#include <G4StepPoint.hh>
#include <G4Track.hh>
#include <G4UserSteppingAction.hh>
#include <G4UserTrackingAction.hh>
#include <G4VProcess.hh>
#include <G4VSensitiveDetector.hh>
#include <SHiP/QuantityView.hpp>
#include <SHiP/SimHit.hpp>
#include <SHiP/SimParticle.hpp>
#include <unordered_map>
#include <utility>
#include <vector>

#include "units/clhep_bridge.hpp"

namespace SHiP::g4 {

// Thread-local storage for current event data (per G4 worker thread)
inline thread_local std::vector<SimHit> tl_hits;
inline thread_local std::vector<SimParticle> tl_particles;
inline thread_local std::unordered_map<int, std::size_t> tl_track_map;

using DetectorIdMap = std::unordered_map<G4LogicalVolume*, int>;

inline SimHit make_base_hit(G4Step const* step,
                            DetectorIdMap const& detector_ids) {
  auto const* pre = step->GetPreStepPoint();
  auto const pos = pre->GetPosition();
  auto const mom = pre->GetMomentum();
  auto* lv = pre->GetTouchable()->GetVolume()->GetLogicalVolume();

  SimHit hit;
  auto const it = detector_ids.find(lv);
  hit.detectorId = it != detector_ids.end() ? it->second : -1;
  hit.trackId = step->GetTrack()->GetTrackID();
  hit.pdgCode = step->GetTrack()->GetDefinition()->GetPDGEncoding();
  ship::view::setPosition(hit, aegir::clhep::position(pos));
  ship::view::setMomentum(hit, aegir::clhep::momentum(mom));
  ship::view::setTime(hit, aegir::clhep::time(pre->GetGlobalTime()));
  return hit;
}

class ScoringSD : public G4VSensitiveDetector {
 public:
  ScoringSD(G4String const& name, DetectorIdMap detector_ids)
      : G4VSensitiveDetector(name), detector_ids_{std::move(detector_ids)} {}

  G4bool ProcessHits(G4Step* step, G4TouchableHistory*) override {
    double const edep = step->GetTotalEnergyDeposit();
    if (edep <= 0) {
      return false;
    }

    auto hit = make_base_hit(step, detector_ids_);
    ship::view::setEnergyDeposit(hit, aegir::clhep::energy(edep));
    ship::view::setPathLength(hit, aegir::clhep::length(step->GetStepLength()));
    tl_hits.push_back(hit);
    return true;
  }

 private:
  DetectorIdMap detector_ids_;
};

// Records a SimHit when a track first enters the volume, regardless of
// energy deposit. Optionally filters on kinetic energy threshold.
// Matches FairShip's exitHadronAbsorber scoring behaviour.
class CrossingSD : public G4VSensitiveDetector {
 public:
  CrossingSD(G4String const& name, DetectorIdMap detector_ids,
             ship::Energy ke_threshold = ship::Energy::zero())
      : G4VSensitiveDetector(name),
        detector_ids_{std::move(detector_ids)},
        ke_threshold_{aegir::clhep::g4(ke_threshold)} {}

  G4bool ProcessHits(G4Step* step, G4TouchableHistory*) override {
    if (!step->IsFirstStepInVolume()) {
      return false;
    }

    auto const* track = step->GetTrack();
    if (track->GetKineticEnergy() < ke_threshold_) {
      return false;
    }

    auto hit = make_base_hit(step, detector_ids_);
    hit.energyDeposit = 0;
    ship::view::setPathLength(hit,
                              aegir::clhep::length(track->GetTrackLength()));
    tl_hits.push_back(hit);
    return true;
  }

 private:
  DetectorIdMap detector_ids_;
  double ke_threshold_;  // Geant4 internal units
};

// Kills tracks below kinetic energy threshold.
// Matches FairShip's PreTrack() stopping behaviour.
class EnergyCutAction : public G4UserSteppingAction {
 public:
  explicit EnergyCutAction(ship::Energy ke_threshold)
      : ke_threshold_{aegir::clhep::g4(ke_threshold)} {}

  void UserSteppingAction(const G4Step* step) override {
    auto* track = step->GetTrack();
    if (track->GetKineticEnergy() < ke_threshold_) {
      track->SetTrackStatus(fStopAndKill);
    }
  }

 private:
  double ke_threshold_;  // Geant4 internal units
};

class TrackingAction : public G4UserTrackingAction {
 public:
  explicit TrackingAction(ship::Energy particle_ke_cut = ship::Energy::zero())
      : particle_ke_cut_{aegir::clhep::g4(particle_ke_cut)} {}

  void PreUserTrackingAction(const G4Track* track) override {
    if (particle_ke_cut_ > 0 && track->GetParentID() != 0 &&
        track->GetKineticEnergy() < particle_ke_cut_) {
      return;
    }

    SimParticle p;
    p.trackId = track->GetTrackID();
    p.parentId = track->GetParentID();
    p.pdgCode = track->GetDefinition()->GetPDGEncoding();

    ship::view::setVertex(p, aegir::clhep::position(track->GetPosition()));
    ship::view::setMomentum(p, aegir::clhep::momentum(track->GetMomentum()));
    ship::view::setEnergy(p, aegir::clhep::energy(track->GetKineticEnergy()));
    ship::view::setTime(p, aegir::clhep::time(track->GetGlobalTime()));

    const auto* creator = track->GetCreatorProcess();
    p.creatorProcess = creator ? creator->GetProcessSubType() : 0;
    // A primary's weight comes from its MCParticle; a secondary inherits that
    // of its parent track.
    p.weight = track->GetWeight();

    tl_track_map[p.trackId] = tl_particles.size();
    tl_particles.push_back(p);
  }

  void PostUserTrackingAction(const G4Track* track) override {
    auto const it = tl_track_map.find(track->GetTrackID());
    if (it != tl_track_map.end()) {
      ship::view::setEndpoint(tl_particles[it->second],
                              aegir::clhep::position(track->GetPosition()));
    }
  }

 private:
  double particle_ke_cut_;  // Geant4 internal units
};

}  // namespace SHiP::g4
