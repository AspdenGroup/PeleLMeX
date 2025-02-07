#include <PeleLMeX.H>
#include <PeleLMeX_K.H>
#include <memory>

using namespace amrex;

void
PeleLM::setThermoPress(const TimeStamp& a_time)
{
  BL_PROFILE("PeleLMeX::setThermoPress()");

  AMREX_ASSERT(a_time == AmrOldTime || a_time == AmrNewTime);

  for (int lev = 0; lev <= finest_level; ++lev) {
    setThermoPress(lev, a_time);
  }
}

void
PeleLM::setThermoPress(int lev, const TimeStamp& a_time)
{

  AMREX_ASSERT(a_time == AmrOldTime || a_time == AmrNewTime);

  auto* ldata_p = getLevelDataPtr(lev, a_time);
  auto const& sma = ldata_p->state.arrays();

  amrex::ParallelFor(
    ldata_p->state,
    [=] AMREX_GPU_DEVICE(int box_no, int i, int j, int k) noexcept {
      getPGivenRTY(
        i, j, k, Array4<Real const>(sma[box_no], DENSITY),
        Array4<Real const>(sma[box_no], FIRSTSPEC),
        Array4<Real const>(sma[box_no], TEMP),
        Array4<Real>(sma[box_no], RHORT));
    });
  Gpu::streamSynchronize();
}

void
PeleLM::calcDivU(
  int is_init,
  int computeDiff,
  int do_avgDown,
  const TimeStamp& a_time,
  std::unique_ptr<AdvanceDiffData>& diffData)
{
  BL_PROFILE("PeleLMeX::calcDivU()");

  AMREX_ASSERT(a_time == AmrOldTime || a_time == AmrNewTime);

  // If requested, compute diffusion terms
  // otherwise assumes it has already been computed and stored in the proper
  // container of diffData
  if (computeDiff != 0) {
    calcDiffusivity(a_time);
    computeDifferentialDiffusionTerms(a_time, diffData, is_init);
  }

  // Assemble divU on each level
  for (int lev = 0; lev <= finest_level; lev++) {

    auto* ldata_p = getLevelDataPtr(lev, a_time);

    MultiFab RhoYdot;
    if ((m_do_react != 0) && (m_skipInstantRR == 0)) {
      if (is_init != 0) { // Either pre-divU, divU or press initial iterations
        if (m_dt > 0.0) { // divU ite   -> use I_R
          auto* ldataR_p = getLevelDataReactPtr(lev);
          RhoYdot.define(grids[lev], dmap[lev], nCompIR(), 0);
          MultiFab::Copy(RhoYdot, ldataR_p->I_R, 0, 0, nCompIR(), 0);
        } else { // press ite  -> set to zero
          RhoYdot.define(grids[lev], dmap[lev], nCompIR(), 0);
          RhoYdot.setVal(0.0);
        }
      } else { // Regular    -> use instantaneous RR
        RhoYdot.define(grids[lev], dmap[lev], nCompIR(), 0);
#ifdef PELE_USE_EFIELD
        computeInstantaneousReactionRateEF(lev, a_time, &RhoYdot);
#else
        computeInstantaneousReactionRate(lev, a_time, &RhoYdot);
#endif
      }
    }

    //----------------------------------------------------------------
#ifdef AMREX_USE_EB
    // Get EBFact
    const auto& ebfact = EBFactory(lev);
#endif

#ifdef AMREX_USE_OMP
#pragma omp parallel if (Gpu::notInLaunchRegion())
#endif
    for (MFIter mfi(ldata_p->divu, TilingIfNotGPU()); mfi.isValid(); ++mfi) {
      const Box& bx = mfi.tilebox();

#ifdef AMREX_USE_EB
      auto const& flagfab = ebfact.getMultiEBCellFlagFab()[mfi];
      auto const& flag = flagfab.const_array();
#endif

      auto const& rhoY = ldata_p->state.const_array(mfi, FIRSTSPEC);
      auto const& T = ldata_p->state.const_array(mfi, TEMP);
      auto const& SpecD = (a_time == AmrOldTime)
                            ? diffData->Dn[lev].const_array(mfi, 0)
                            : diffData->Dnp1[lev].const_array(mfi, 0);
      auto const& Fourier =
        (a_time == AmrOldTime)
          ? diffData->Dn[lev].const_array(mfi, NUM_SPECIES)
          : diffData->Dnp1[lev].const_array(mfi, NUM_SPECIES);
      auto const& DiffDiff =
        (a_time == AmrOldTime)
          ? diffData->Dn[lev].const_array(mfi, NUM_SPECIES + 1)
          : diffData->Dnp1[lev].const_array(mfi, NUM_SPECIES + 1);
      auto const& r =
        ((m_do_react != 0) && (m_skipInstantRR == 0))
          ? RhoYdot.const_array(mfi)
          : ldata_p->state.const_array(mfi, FIRSTSPEC); // Dummy unused Array4
      auto const& extRhoY = m_extSource[lev]->const_array(mfi, FIRSTSPEC);
      auto const& extRhoH = m_extSource[lev]->const_array(mfi, RHOH);
      auto const& divu = ldata_p->divu.array(mfi);
      int use_react = ((m_do_react != 0) && (m_skipInstantRR == 0)) ? 1 : 0;

#ifdef AMREX_USE_EB
      if (flagfab.getType(bx) == FabType::covered) { // Covered boxes
        amrex::ParallelFor(
          bx, [divu] AMREX_GPU_DEVICE(int i, int j, int k) noexcept {
            divu(i, j, k) = 0.0;
          });
      } else if (flagfab.getType(bx) != FabType::regular) { // EB containing
                                                            // boxes
        amrex::ParallelFor(
          bx, [rhoY, T, SpecD, Fourier, DiffDiff, r, extRhoY, extRhoH, divu,
               use_react, flag] AMREX_GPU_DEVICE(int i, int j, int k) noexcept {
            if (flag(i, j, k).isCovered()) {
              divu(i, j, k) = 0.0;
            } else {
              compute_divu(
                i, j, k, rhoY, T, SpecD, Fourier, DiffDiff, r, extRhoY, extRhoH,
                divu, use_react);
            }
          });
      } else
#endif
      {
        amrex::ParallelFor(
          bx, [rhoY, T, SpecD, Fourier, DiffDiff, r, extRhoY, extRhoH, divu,
               use_react] AMREX_GPU_DEVICE(int i, int j, int k) noexcept {
            compute_divu(
              i, j, k, rhoY, T, SpecD, Fourier, DiffDiff, r, extRhoY, extRhoH,
              divu, use_react);
          });
      }
    }
  }

  // Average down divU
  if (do_avgDown != 0) {
    for (int lev = finest_level; lev > 0; --lev) {
      auto* ldataFine_p = getLevelDataPtr(lev, a_time);
      auto* ldataCrse_p = getLevelDataPtr(lev - 1, a_time);
#ifdef AMREX_USE_EB
      EB_average_down(
        ldataFine_p->divu, ldataCrse_p->divu, 0, 1, refRatio(lev - 1));
#else
      average_down(
        ldataFine_p->divu, ldataCrse_p->divu, 0, 1, refRatio(lev - 1));
#endif
    }
  }

  // fillPatch a_time divu to get properly filled ghost cells
  for (int lev = 0; lev <= finest_level; ++lev) {
    Real time = getTime(lev, a_time);
    auto* ldata_p = getLevelDataPtr(lev, a_time);
    fillpatch_divu(lev, time, ldata_p->divu, m_nGrowdivu);
  }
}

void
PeleLM::setRhoToSumRhoY(int lev, const TimeStamp& a_time)
{

  AMREX_ASSERT(a_time == AmrOldTime || a_time == AmrNewTime);

  auto* ldata_p = getLevelDataPtr(lev, a_time);
  auto const& sma = ldata_p->state.arrays();

  amrex::ParallelFor(
    ldata_p->state,
    [=] AMREX_GPU_DEVICE(int box_no, int i, int j, int k) noexcept {
      sma[box_no](i, j, k, DENSITY) = 0.0;
      for (int n = 0; n < NUM_SPECIES; n++) {
        sma[box_no](i, j, k, DENSITY) += sma[box_no](i, j, k, FIRSTSPEC + n);
      }
    });
  Gpu::streamSynchronize();
}

void
PeleLM::setTemperature(const TimeStamp& a_time)
{
  BL_PROFILE_VAR("PeleLMeX::setTemperature()", setTemperature);

  AMREX_ASSERT(a_time == AmrOldTime || a_time == AmrNewTime);

  for (int lev = 0; lev <= finest_level; ++lev) {
    setTemperature(lev, a_time);
  }
  Gpu::streamSynchronize();
}

void
PeleLM::setTemperature(int lev, const TimeStamp& a_time)
{

  AMREX_ASSERT(a_time == AmrOldTime || a_time == AmrNewTime);

  auto* ldata_p = getLevelDataPtr(lev, a_time);
  auto const& sma = ldata_p->state.arrays();

  amrex::ParallelFor(
    ldata_p->state,
    [=] AMREX_GPU_DEVICE(int box_no, int i, int j, int k) noexcept {
      getTfromHY(
        i, j, k, Array4<Real const>(sma[box_no], DENSITY),
        Array4<Real const>(sma[box_no], FIRSTSPEC),
        Array4<Real const>(sma[box_no], RHOH), Array4<Real>(sma[box_no], TEMP));
    });
  Gpu::streamSynchronize();
}

void
PeleLM::calc_dPdt(const TimeStamp& a_time, const Vector<MultiFab*>& a_dPdt)
{
  BL_PROFILE("PeleLMeX::calc_dPdt()");

  AMREX_ASSERT(a_time == AmrOldTime || a_time == AmrNewTime);

  for (int lev = 0; lev <= finest_level; ++lev) {
    calc_dPdt(lev, a_time, a_dPdt[lev]);
#ifdef AMREX_USE_EB
    EB_set_covered(*a_dPdt[lev], 0.0);
#endif
  }

  // Fill ghost cell(s)
  if (a_dPdt[0]->nGrow() > 0) {
    fillpatch_forces(m_cur_time, a_dPdt, a_dPdt[0]->nGrow());
  }
}

void
PeleLM::calc_dPdt(int lev, const TimeStamp& a_time, MultiFab* a_dPdt)
{
  auto const& sma = getLevelDataPtr(lev, a_time)->state.arrays();
  auto const& dPdtma = a_dPdt->arrays();

  // Use new ambient pressure to compute dPdt
  Real p_amb = m_pNew;

  amrex::ParallelFor(
    *a_dPdt, [=, dt = m_dt, dpdt_fac = m_dpdtFactor] AMREX_GPU_DEVICE(
               int box_no, int i, int j, int k) noexcept {
      auto dPdta = dPdtma[box_no];
      auto sa = sma[box_no];
      dPdta(i, j, k) =
        (sa(i, j, k, RHORT) - p_amb) / (dt * sa(i, j, k, RHORT)) * dpdt_fac;
    });
  Gpu::streamSynchronize();
}

Real
PeleLM::adjustPandDivU(std::unique_ptr<AdvanceAdvData>& advData)
{
  BL_PROFILE("PeleLMeX::adjustPandDivU()");

  Vector<std::unique_ptr<MultiFab>> ThetaHalft(finest_level + 1);

  // Get theta = 1 / (\Gamma * P_amb) at half time
  for (int lev = 0; lev <= finest_level; ++lev) {

    ThetaHalft[lev] = std::make_unique<MultiFab>(
      grids[lev], dmap[lev], 1, 0, MFInfo(), *m_factory[lev]);
    auto const& tma = ThetaHalft[lev]->arrays();
    auto const& sma_o = getLevelDataPtr(lev, AmrOldTime)->state.const_arrays();
    auto const& sma_n = getLevelDataPtr(lev, AmrNewTime)->state.const_arrays();

    amrex::ParallelFor(
      *ThetaHalft[lev], [=, pOld = m_pOld, pNew = m_pNew] AMREX_GPU_DEVICE(
                          int box_no, int i, int j, int k) noexcept {
        auto theta = tma[box_no];
        Real gammaInv_o = getGammaInv(
          i, j, k, Array4<Real const>(sma_o[box_no], FIRSTSPEC),
          Array4<Real const>(sma_o[box_no], TEMP));
        Real gammaInv_n = getGammaInv(
          i, j, k, Array4<Real const>(sma_n[box_no], FIRSTSPEC),
          Array4<Real const>(sma_n[box_no], TEMP));
        theta(i, j, k) = 0.5 * (gammaInv_o / pOld + gammaInv_n / pNew);
      });
  }
  Gpu::streamSynchronize();

  // Get the mean mac_divu (Sbar) and mean theta
  Real Sbar = MFSum(GetVecOfConstPtrs(advData->mac_divu), 0);
  Sbar /= m_uncoveredVol;
  Real Thetabar = MFSum(GetVecOfConstPtrs(ThetaHalft), 0);
  Thetabar /= m_uncoveredVol;

  // Adjust
  for (int lev = 0; lev <= finest_level; ++lev) {
    // ThetaHalft is now delta_theta
    ThetaHalft[lev]->plus(-Thetabar, 0, 1);
    // mac_divu is now delta_S
    advData->mac_divu[lev].plus(-Sbar, 0, 1);
  }

  // Compute 1/Volume * int(U_inflow)dA across all boundary faces
  amrex::Real umacFluxBalance = AMREX_D_TERM(
    m_domainUmacFlux[0] + m_domainUmacFlux[1],
    +m_domainUmacFlux[2] + m_domainUmacFlux[3],
    +m_domainUmacFlux[4] + m_domainUmacFlux[5]);
  Real divu_vol = umacFluxBalance / m_uncoveredVol;

  // Advance the ambient pressure
  m_pNew = m_pOld + m_dt * (Sbar - divu_vol) / Thetabar;
  m_dp0dt = (Sbar - divu_vol) / Thetabar;

  // subtract \tilde{theta} * Sbar / Thetabar from divu
  for (int lev = 0; lev <= finest_level; ++lev) {
    auto const& tma = ThetaHalft[lev]->arrays();
    auto const& uma = advData->mac_divu[lev].arrays();
    amrex::ParallelFor(
      *ThetaHalft[lev],
      [=] AMREX_GPU_DEVICE(int box_no, int i, int j, int k) noexcept {
        auto theta = tma[box_no];
        uma[box_no](i, j, k) -=
          (theta(i, j, k) * Sbar / Thetabar -
           divu_vol * (1 + theta(i, j, k) / Thetabar));
      });
  }
  Gpu::streamSynchronize();

  if (m_verbose > 2) {
    Print() << " >> Closed chamber pOld: " << m_pOld << ", pNew: " << m_pNew
            << ", dp0dt: " << m_dp0dt << "\n";
    Print() << " >> Total mass old: " << m_massOld
            << ", mass new: " << m_massNew << std::endl;
  }

  // Return Sbar so that we'll add it back to mac_divu after the MAC projection
  return Sbar;
}
