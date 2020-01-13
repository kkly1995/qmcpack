//////////////////////////////////////////////////////////////////////////////////////
// This file is distributed under the University of Illinois/NCSA Open Source License.
// See LICENSE file in top directory for details.
//
// Copyright (c) 2020 QMCPACK developers.
//
// File developed by: Ye Luo, yeluo@anl.gov, Argonne National Laboratory
//
// File created by: Ye Luo, yeluo@anl.gov, Argonne National Laboratory
//////////////////////////////////////////////////////////////////////////////////////


/** @file RealSpacePostions.h
 */
#ifndef QMCPLUSPLUS_REALSPACE_POSITIONS_H
#define QMCPLUSPLUS_REALSPACE_POSITIONS_H

#include <Configuration.h>
#include <OhmmsSoA/Container.h>

namespace qmcplusplus
{
/** Introduced to handle virtual moves and ratio computations, e.g. for non-local PP evaluations.
   */
class RealSpacePositions: public QuantumVariables
{
public:
  using ParticlePos_t = PtclOnLatticeTraits::ParticlePos_t;
  using RealType = QMCTraits::PosType;
  using PosType = QMCTraits::PosType;

  RealSpacePositions() : QuantumVariables(QuantumVariableKind::QV_POS) {}

  std::unique_ptr<QuantumVariables> makeClone() override { return std::make_unique<RealSpacePositions>(*this); }

  void resize(size_t n) override { RSoA.resize(n);}
  size_t size() override { return RSoA.size(); }

  void setAllParticlePos(const ParticlePos_t& R) override { resize(R.size()); RSoA.copyIn(R);}
  void setOneParticlePos(const PosType& pos, size_t iat) override {RSoA(iat) = pos;}

  const PosVectorSoa& getAllParticlePos() override { return RSoA; }
  PosType getOneParticlePos(size_t iat) const override { return RSoA[iat]; }

private:
  PosVectorSoa RSoA;
};
} // namespace qmcplusplus
#endif
