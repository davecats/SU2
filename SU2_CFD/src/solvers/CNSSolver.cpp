/*!
 * \file BNSSolver.cpp
 * \brief Main subroutines for solving Finite-Volume Navier-Stokes flow problems.
 * \author F. Palacios, T. Economon
 * \version 7.2.1 "Blackbird"
 *
 * SU2 Project Website: https://su2code.github.io
 *
 * The SU2 Project is maintained by the SU2 Foundation
 * (http://su2foundation.org)
 *
 * Copyright 2012-2021, SU2 Contributors (cf. AUTHORS.md)
 *
 * SU2 is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * SU2 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with SU2. If not, see <http://www.gnu.org/licenses/>.
 */

#include "../../include/solvers/CNSSolver.hpp"

#include "../../../Common/include/toolboxes/geometry_toolbox.hpp"
#include "../../../Common/include/toolboxes/printing_toolbox.hpp"
#include "../../include/fluid/CIdealGas.hpp"
#include "../../include/solvers/CFVMFlowSolverBase.inl"
#include "../../include/variables/CNSVariable.hpp"

/*--- Explicit instantiation of the parent class of CEulerSolver,
 *    to spread the compilation over two cpp files. ---*/
template class CFVMFlowSolverBase<CEulerVariable, ENUM_REGIME::COMPRESSIBLE>;

CNSSolver::CNSSolver(CGeometry* geometry, CConfig* config, unsigned short iMesh)
    : CEulerSolver(geometry, config, iMesh, true) {
  /*--- This constructor only allocates/inits what is extra to CEulerSolver. ---*/

  /*--- Buffet sensor in all the markers and coefficients ---*/

  Buffet_Sensor.resize(nMarker);
  for (unsigned long i = 0; i < nMarker; ++i) Buffet_Sensor[i].resize(nVertex[i], 0.0);
  Buffet_Metric.resize(nMarker, 0.0);
  Surface_Buffet_Metric.resize(config->GetnMarker_Monitoring(), 0.0);

  /*--- Read farfield conditions from config ---*/

  Viscosity_Inf = config->GetViscosity_FreeStreamND();
  Prandtl_Lam = config->GetPrandtl_Lam();
  Prandtl_Turb = config->GetPrandtl_Turb();
  Tke_Inf = config->GetTke_FreeStreamND();

  /*--- Initialize the seed values for forward mode differentiation. ---*/

  switch (config->GetDirectDiff()) {
    case D_VISCOSITY:
      SU2_TYPE::SetDerivative(Viscosity_Inf, 1.0);
      break;
    default:
      /*--- Already done upstream. ---*/
      break;
  }
}

void CNSSolver::Preprocessing(CGeometry* geometry, CSolver** solver_container, CConfig* config, unsigned short iMesh,
                              unsigned short iRKStep, unsigned short RunTime_EqSystem, bool Output) {
  const auto InnerIter = config->GetInnerIter();
  const bool muscl = config->GetMUSCL_Flow() && (iMesh == MESH_0);
  const bool center = (config->GetKind_ConvNumScheme_Flow() == SPACE_CENTERED);
  const bool limiter = (config->GetKind_SlopeLimit_Flow() != NO_LIMITER) && (InnerIter <= config->GetLimiterIter());
  const bool van_albada = (config->GetKind_SlopeLimit_Flow() == VAN_ALBADA_EDGE);
  const bool wall_functions = config->GetWall_Functions();

  /*--- Common preprocessing steps (implemented by CEulerSolver) ---*/

  CommonPreprocessing(geometry, solver_container, config, iMesh, iRKStep, RunTime_EqSystem, Output);

  /*--- Compute gradient for MUSCL reconstruction, for output (i.e. the
   turbulence solver, and post) only temperature and velocity are needed ---*/

  const auto nPrimVarGrad_bak = nPrimVarGrad;
  if (Output) ompMasterAssignBarrier(nPrimVarGrad, 1 + nDim);

  if (config->GetReconstructionGradientRequired() && muscl && !center) {
    switch (config->GetKind_Gradient_Method_Recon()) {
      case GREEN_GAUSS:
        SetPrimitive_Gradient_GG(geometry, config, true);
        break;
      case LEAST_SQUARES:
      case WEIGHTED_LEAST_SQUARES:
        SetPrimitive_Gradient_LS(geometry, config, true);
        break;
      default:
        break;
    }
  }

  /*--- Compute gradient of the primitive variables ---*/

  if (config->GetKind_Gradient_Method() == GREEN_GAUSS) {
    SetPrimitive_Gradient_GG(geometry, config);
  } else if (config->GetKind_Gradient_Method() == WEIGHTED_LEAST_SQUARES) {
    SetPrimitive_Gradient_LS(geometry, config);
  }

  if (Output) ompMasterAssignBarrier(nPrimVarGrad, nPrimVarGrad_bak);

  /*--- Compute the limiters ---*/

  if (muscl && !center && limiter && !van_albada && !Output) {
    SetPrimitive_Limiter(geometry, config);
  }

  ComputeVorticityAndStrainMag(*config, iMesh);

  /*--- Compute the TauWall from the wall functions ---*/

  if (wall_functions) {
    SetTau_Wall_WF(geometry, solver_container, config);
  }
}

unsigned long CNSSolver::SetPrimitive_Variables(CSolver** solver_container, const CConfig* config) {
  /*--- Number of non-physical points, local to the thread, needs
   *    further reduction if function is called in parallel ---*/
  unsigned long nonPhysicalPoints = 0;

  const TURB_MODEL turb_model = config->GetKind_Turb_Model();
  const bool tkeNeeded = (turb_model == TURB_MODEL::SST) || (turb_model == TURB_MODEL::SST_SUST);

  AD::StartNoSharedReading();

  SU2_OMP_FOR_STAT(omp_chunk_size)
  for (unsigned long iPoint = 0; iPoint < nPoint; iPoint++) {
    /*--- Retrieve the value of the kinetic energy (if needed). ---*/

    su2double eddy_visc = 0.0, turb_ke = 0.0;

    if (turb_model != TURB_MODEL::NONE && solver_container[TURB_SOL] != nullptr) {
      eddy_visc = solver_container[TURB_SOL]->GetNodes()->GetmuT(iPoint);
      if (tkeNeeded) turb_ke = solver_container[TURB_SOL]->GetNodes()->GetSolution(iPoint, 0);

      if (config->GetKind_HybridRANSLES() != NO_HYBRIDRANSLES) {
        su2double DES_LengthScale = solver_container[TURB_SOL]->GetNodes()->GetDES_LengthScale(iPoint);
        nodes->SetDES_LengthScale(iPoint, DES_LengthScale);
      }
    }

    /*--- Compressible flow, primitive variables nDim+5, (T, vx, vy, vz, P, rho, h, c, lamMu, eddyMu, ThCond, Cp) ---*/

    bool physical = static_cast<CNSVariable*>(nodes)->SetPrimVar(iPoint, eddy_visc, turb_ke, GetFluidModel());
    nodes->SetSecondaryVar(iPoint, GetFluidModel());

    /*--- Check for non-realizable states for reporting. ---*/

    nonPhysicalPoints += !physical;
  }
  END_SU2_OMP_FOR

  AD::EndNoSharedReading();

  return nonPhysicalPoints;
}

void CNSSolver::Viscous_Residual(unsigned long iEdge, CGeometry* geometry, CSolver** solver_container,
                                 CNumerics* numerics, CConfig* config) {
  Viscous_Residual_impl(iEdge, geometry, solver_container, numerics, config);
}

void CNSSolver::Buffet_Monitoring(const CGeometry* geometry, const CConfig* config) {
  unsigned long iVertex;
  unsigned short iMarker, iMarker_Monitoring;
  const su2double* Vel_FS = Velocity_Inf;
  const su2double k = config->GetBuffet_k(), lam = config->GetBuffet_lambda(), Sref = config->GetRefArea();

  const su2double VelMag_FS = GeometryToolbox::Norm(nDim, Vel_FS);

  /*-- Variables initialization ---*/

  Total_Buffet_Metric = 0.0;

  for (iMarker_Monitoring = 0; iMarker_Monitoring < config->GetnMarker_Monitoring(); iMarker_Monitoring++) {
    Surface_Buffet_Metric[iMarker_Monitoring] = 0.0;
  }

  /*--- Loop over the Euler and Navier-Stokes markers ---*/

  for (iMarker = 0; iMarker < nMarker; iMarker++) {
    Buffet_Metric[iMarker] = 0.0;

    const auto Monitoring = config->GetMarker_All_Monitoring(iMarker);

    if (config->GetViscous_Wall(iMarker)) {
      /*--- Loop over the vertices to compute the buffet sensor ---*/

      for (iVertex = 0; iVertex < geometry->nVertex[iMarker]; iVertex++) {
        /*--- Perform dot product of skin friction with freestream velocity ---*/

        const su2double SkinFrictionMag = GeometryToolbox::Norm(nDim, CSkinFriction[iMarker][iVertex]);
        su2double SkinFrictionDot = GeometryToolbox::DotProduct(nDim, CSkinFriction[iMarker][iVertex], Vel_FS);

        /*--- Normalize the dot product ---*/

        SkinFrictionDot /= SkinFrictionMag * VelMag_FS;

        /*--- Compute Heaviside function ---*/

        Buffet_Sensor[iMarker][iVertex] = 1. / (1. + exp(2. * k * (SkinFrictionDot + lam)));

        /*--- Integrate buffet sensor ---*/

        if (Monitoring == YES) {
          auto Normal = geometry->vertex[iMarker][iVertex]->GetNormal();
          su2double Area = GeometryToolbox::Norm(nDim, Normal);

          Buffet_Metric[iMarker] += Buffet_Sensor[iMarker][iVertex] * Area / Sref;
        }
      }

      if (Monitoring == YES) {
        Total_Buffet_Metric += Buffet_Metric[iMarker];

        /*--- Per surface buffet metric ---*/

        for (iMarker_Monitoring = 0; iMarker_Monitoring < config->GetnMarker_Monitoring(); iMarker_Monitoring++) {
          auto Monitoring_Tag = config->GetMarker_Monitoring_TagBound(iMarker_Monitoring);
          auto Marker_Tag = config->GetMarker_All_TagBound(iMarker);
          if (Marker_Tag == Monitoring_Tag) Surface_Buffet_Metric[iMarker_Monitoring] = Buffet_Metric[iMarker];
        }
      }
    }
  }

  /*--- Add buffet metric information using all the nodes ---*/

  su2double MyTotal_Buffet_Metric = Total_Buffet_Metric;
  SU2_MPI::Allreduce(&MyTotal_Buffet_Metric, &Total_Buffet_Metric, 1, MPI_DOUBLE, MPI_SUM, SU2_MPI::GetComm());

  /*--- Add the buffet metric on the surfaces using all the nodes ---*/

  auto local_copy = Surface_Buffet_Metric;
  SU2_MPI::Allreduce(local_copy.data(), Surface_Buffet_Metric.data(), local_copy.size(), MPI_DOUBLE, MPI_SUM,
                     SU2_MPI::GetComm());
}

void CNSSolver::Evaluate_ObjFunc(const CConfig* config) {
  unsigned short iMarker_Monitoring, Kind_ObjFunc;
  su2double Weight_ObjFunc;

  /*--- Evaluate objective functions common to Euler and NS solvers ---*/

  CEulerSolver::Evaluate_ObjFunc(config);

  /*--- Evaluate objective functions specific to NS solver ---*/

  for (iMarker_Monitoring = 0; iMarker_Monitoring < config->GetnMarker_Monitoring(); iMarker_Monitoring++) {
    Weight_ObjFunc = config->GetWeight_ObjFunc(iMarker_Monitoring);
    Kind_ObjFunc = config->GetKind_ObjFunc(iMarker_Monitoring);

    switch (Kind_ObjFunc) {
      case BUFFET_SENSOR:
        Total_ComboObj += Weight_ObjFunc * Surface_Buffet_Metric[iMarker_Monitoring];
        break;
      default:
        break;
    }
  }
}

void CNSSolver::SetRoe_Dissipation(CGeometry* geometry, CConfig* config) {
  const unsigned short kind_roe_dissipation = config->GetKind_RoeLowDiss();

  SU2_OMP_FOR_STAT(omp_chunk_size)
  for (unsigned long iPoint = 0; iPoint < nPoint; iPoint++) {
    if (kind_roe_dissipation == FD || kind_roe_dissipation == FD_DUCROS) {
      su2double wall_distance = geometry->nodes->GetWall_Distance(iPoint);

      nodes->SetRoe_Dissipation_FD(iPoint, wall_distance);

    } else if (kind_roe_dissipation == NTS || kind_roe_dissipation == NTS_DUCROS) {
      const su2double delta = geometry->nodes->GetMaxLength(iPoint);
      assert(delta > 0 && "Delta must be initialized and non-negative");
      nodes->SetRoe_Dissipation_NTS(iPoint, delta, config->GetConst_DES());
    }
  }
  END_SU2_OMP_FOR
}

void CNSSolver::AddDynamicGridResidualContribution(unsigned long iPoint, unsigned long Point_Normal,
                                                   const CGeometry* geometry, const su2double* UnitNormal,
                                                   su2double Area, const su2double* GridVel, su2double** Jacobian_i,
                                                   su2double& Res_Conv, su2double& Res_Visc) const {
  su2double ProjGridVel = Area * GeometryToolbox::DotProduct(nDim, GridVel, UnitNormal);

  /*--- Retrieve other primitive quantities and viscosities ---*/

  su2double Density = nodes->GetDensity(iPoint);
  su2double Pressure = nodes->GetPressure(iPoint);
  su2double laminar_viscosity = nodes->GetLaminarViscosity(iPoint);
  su2double eddy_viscosity = nodes->GetEddyViscosity(iPoint);
  su2double total_viscosity = laminar_viscosity + eddy_viscosity;

  /*--- Compute the viscous stress tensor ---*/

  su2double tau[MAXNDIM][MAXNDIM] = {{0.0}};
  CNumerics::ComputeStressTensor(nDim, tau, nodes->GetGradient_Primitive(iPoint) + 1, total_viscosity);

  /*--- Dot product of the stress tensor with the grid velocity ---*/

  su2double tau_vel[MAXNDIM] = {0.0};
  for (auto iDim = 0u; iDim < nDim; iDim++) tau_vel[iDim] = GeometryToolbox::DotProduct(nDim, tau[iDim], GridVel);

  /*--- Compute the convective and viscous residuals (energy eqn.) ---*/

  Res_Conv += Pressure * ProjGridVel;
  Res_Visc += GeometryToolbox::DotProduct(nDim, tau_vel, UnitNormal) * Area;

  /*--- Implicit Jacobian contributions due to moving walls ---*/

  if (Jacobian_i != nullptr) {
    /*--- Jacobian contribution related to the pressure term ---*/

    su2double GridVel2 = GeometryToolbox::SquaredNorm(nDim, GridVel);

    Jacobian_i[nDim + 1][0] += 0.5 * (Gamma - 1.0) * GridVel2 * ProjGridVel;

    for (auto jDim = 0u; jDim < nDim; jDim++)
      Jacobian_i[nDim + 1][jDim + 1] += -(Gamma - 1.0) * GridVel[jDim] * ProjGridVel;

    Jacobian_i[nDim + 1][nDim + 1] += (Gamma - 1.0) * ProjGridVel;

    /*--- Now the Jacobian contribution related to the shear stress ---*/

    /*--- Get coordinates of i & nearest normal and compute distance ---*/

    const auto Coord_i = geometry->nodes->GetCoord(iPoint);
    const auto Coord_j = geometry->nodes->GetCoord(Point_Normal);

    su2double dist_ij = GeometryToolbox::Distance(nDim, Coord_i, Coord_j);

    const su2double theta2 = 1.0;

    su2double factor = total_viscosity * Area / (Density * dist_ij);

    if (nDim == 2) {
      su2double thetax = theta2 + UnitNormal[0] * UnitNormal[0] / 3.0;
      su2double thetay = theta2 + UnitNormal[1] * UnitNormal[1] / 3.0;

      su2double etaz = UnitNormal[0] * UnitNormal[1] / 3.0;

      su2double pix = GridVel[0] * thetax + GridVel[1] * etaz;
      su2double piy = GridVel[0] * etaz + GridVel[1] * thetay;

      Jacobian_i[nDim + 1][0] += factor * (-pix * GridVel[0] + piy * GridVel[1]);
      Jacobian_i[nDim + 1][1] += factor * pix;
      Jacobian_i[nDim + 1][2] += factor * piy;
    } else {
      su2double thetax = theta2 + UnitNormal[0] * UnitNormal[0] / 3.0;
      su2double thetay = theta2 + UnitNormal[1] * UnitNormal[1] / 3.0;
      su2double thetaz = theta2 + UnitNormal[2] * UnitNormal[2] / 3.0;

      su2double etaz = UnitNormal[0] * UnitNormal[1] / 3.0;
      su2double etax = UnitNormal[1] * UnitNormal[2] / 3.0;
      su2double etay = UnitNormal[0] * UnitNormal[2] / 3.0;

      su2double pix = GridVel[0] * thetax + GridVel[1] * etaz + GridVel[2] * etay;
      su2double piy = GridVel[0] * etaz + GridVel[1] * thetay + GridVel[2] * etax;
      su2double piz = GridVel[0] * etay + GridVel[1] * etax + GridVel[2] * thetaz;

      Jacobian_i[nDim + 1][0] += factor * (-pix * GridVel[0] + piy * GridVel[1] + piz * GridVel[2]);
      Jacobian_i[nDim + 1][1] += factor * pix;
      Jacobian_i[nDim + 1][2] += factor * piy;
      Jacobian_i[nDim + 1][3] += factor * piz;
    }
  }
}

void CNSSolver::BC_HeatFlux_Wall(CGeometry* geometry, CSolver**, CNumerics*, CNumerics*, CConfig* config,
                                 unsigned short val_marker) {
  BC_HeatFlux_Wall_Generic(geometry, config, val_marker, HEAT_FLUX);
}

void CNSSolver::BC_HeatTransfer_Wall(const CGeometry* geometry, const CConfig* config,
                                     const unsigned short val_marker) {
  BC_HeatFlux_Wall_Generic(geometry, config, val_marker, HEAT_TRANSFER);
}

void CNSSolver::BC_HeatFlux_Wall_Generic(const CGeometry* geometry, const CConfig* config, unsigned short val_marker,
                                         unsigned short kind_boundary) {
  /*--- Identify the boundary by string name and get the specified wall
   heat flux from config as well as the wall function treatment. ---*/

  const bool implicit = (config->GetKind_TimeIntScheme() == EULER_IMPLICIT);
  const auto Marker_Tag = config->GetMarker_All_TagBound(val_marker);
  /*--- Get the specified wall heat flux, temperature or heat transfer coefficient from config ---*/

  su2double Wall_HeatFlux = 0.0, Tinfinity = 0.0, Transfer_Coefficient = 0.0;

  if (kind_boundary == HEAT_FLUX) {
    Wall_HeatFlux = config->GetWall_HeatFlux(Marker_Tag) / config->GetHeat_Flux_Ref();
  } else if (kind_boundary == HEAT_TRANSFER) {
    /*--- The required heatflux will be computed for each iPoint individually based on local Temperature. ---*/
    Transfer_Coefficient = config->GetWall_HeatTransfer_Coefficient(Marker_Tag) * config->GetTemperature_Ref() /
                           config->GetHeat_Flux_Ref();
    Tinfinity = config->GetWall_HeatTransfer_Temperature(Marker_Tag) / config->GetTemperature_Ref();
  }

  //  Wall_Function = config->GetWallFunction_Treatment(Marker_Tag);
  //  if (Wall_Function != WALL_FUNCTION::NONE) {
  //    SU2_MPI::Error("Wall function treatment not implemented yet", CURRENT_FUNCTION);
  //  }

  /*--- Jacobian, initialized to zero if needed. ---*/
  su2double** Jacobian_i = nullptr;
  if ((dynamic_grid || (kind_boundary == HEAT_TRANSFER)) && implicit) {
    Jacobian_i = new su2double*[nVar];
    for (auto iVar = 0u; iVar < nVar; iVar++) Jacobian_i[iVar] = new su2double[nVar]();
  }

  /*--- Loop over all of the vertices on this boundary marker ---*/

  SU2_OMP_FOR_DYN(OMP_MIN_SIZE)
  for (auto iVertex = 0u; iVertex < geometry->nVertex[val_marker]; iVertex++) {
    const auto iPoint = geometry->vertex[val_marker][iVertex]->GetNode();

    /*--- Check if the node belongs to the domain (i.e, not a halo node) ---*/

    if (!geometry->nodes->GetDomain(iPoint)) continue;

    /*--- If it is a customizable patch, retrieve the specified wall heat flux. ---*/

    if (config->GetMarker_All_PyCustom(val_marker))
      Wall_HeatFlux = geometry->GetCustomBoundaryHeatFlux(val_marker, iVertex);
    else if (kind_boundary == HEAT_TRANSFER) {
      const su2double Twall = nodes->GetTemperature(iPoint);
      Wall_HeatFlux = Transfer_Coefficient * (Tinfinity - Twall);
    }

    /*--- Compute dual-grid area and boundary normal ---*/

    const auto Normal = geometry->vertex[val_marker][iVertex]->GetNormal();

    su2double Area = GeometryToolbox::Norm(nDim, Normal);

    su2double UnitNormal[MAXNDIM] = {0.0};
    for (auto iDim = 0u; iDim < nDim; iDim++) UnitNormal[iDim] = -Normal[iDim] / Area;

    /*--- Apply a weak boundary condition for the energy equation.
     Compute the residual due to the prescribed heat flux.
     The convective part will be zero if the grid is not moving. ---*/

    su2double Res_Conv = 0.0;
    su2double Res_Visc = Wall_HeatFlux * Area;

    /*--- Impose the value of the velocity as a strong boundary
     condition (Dirichlet). Fix the velocity and remove any
     contribution to the residual at this node. ---*/

    if (dynamic_grid) {
      nodes->SetVelocity_Old(iPoint, geometry->nodes->GetGridVel(iPoint));
    } else {
      su2double zero[MAXNDIM] = {0.0};
      nodes->SetVelocity_Old(iPoint, zero);
    }

    for (auto iDim = 0u; iDim < nDim; iDim++) LinSysRes(iPoint, iDim + 1) = 0.0;
    nodes->SetVel_ResTruncError_Zero(iPoint);

    /*--- If the wall is moving, there are additional residual contributions
     due to pressure (p v_wall.n) and shear stress (tau.v_wall.n). ---*/

    if (dynamic_grid) {
      if (implicit) {
        for (auto iVar = 0u; iVar < nVar; ++iVar) Jacobian_i[nDim + 1][iVar] = 0.0;
      }

      const auto Point_Normal = geometry->vertex[val_marker][iVertex]->GetNormal_Neighbor();

      AddDynamicGridResidualContribution(iPoint, Point_Normal, geometry, UnitNormal, Area,
                                         geometry->nodes->GetGridVel(iPoint), Jacobian_i, Res_Conv, Res_Visc);
    }

    /*--- Convective and viscous contributions to the residual at the wall ---*/

    LinSysRes(iPoint, nDim + 1) += Res_Conv - Res_Visc;

    /*--- Enforce the no-slip boundary condition in a strong way by
     modifying the velocity-rows of the Jacobian (1 on the diagonal).
     And add the contributions to the Jacobian due to energy. ---*/

    if (implicit) {
      if (kind_boundary == HEAT_TRANSFER) {
        /*--- It is necessary to zero the jacobian entries of the energy equation. ---*/
        if (!dynamic_grid)
          for (auto iVar = 0u; iVar < nVar; ++iVar) Jacobian_i[nDim + 1][iVar] = 0.0;

        const su2double oneOnRho = 1.0 / nodes->GetDensity(iPoint);
        const su2double oneOnCv = (Gamma - 1.0) / config->GetGas_ConstantND();
        const su2double Vel2 = nodes->GetVelocity2(iPoint);
        const su2double dTdrho = oneOnRho * (-Tinfinity + oneOnCv * 0.5 * Vel2);
        const su2double dTdrhoe = oneOnCv * oneOnRho;

        /*--- Total specific energy: e=c_v*T+1/2*v^2 => T=1/c_v(rho*e/rho - 1/2||rho v||^2/rho^2).
        Together with cv=R/(gamma-1) the following Jacobian contributions for the energy equation can be derived. ---*/
        Jacobian_i[nDim + 1][0] += Transfer_Coefficient * dTdrho * Area;

        for (unsigned short iDim = 0; iDim < nDim; iDim++)
          Jacobian_i[nDim + 1][iDim + 1] -= Transfer_Coefficient * dTdrhoe * nodes->GetVelocity(iPoint, iDim) * Area;

        Jacobian_i[nDim + 1][nDim + 1] += Transfer_Coefficient * dTdrhoe * Area;
      }
      if (dynamic_grid || (kind_boundary == HEAT_TRANSFER)) {
        Jacobian.AddBlock2Diag(iPoint, Jacobian_i);
      }

      for (auto iVar = 1u; iVar <= nDim; iVar++) {
        auto total_index = iPoint * nVar + iVar;
        Jacobian.DeleteValsRowi(total_index);
      }
    }
  }
  END_SU2_OMP_FOR

  if (Jacobian_i)
    for (auto iVar = 0u; iVar < nVar; iVar++) delete[] Jacobian_i[iVar];
  delete[] Jacobian_i;
}

su2double CNSSolver::GetCHTWallTemperature(const CConfig* config, unsigned short val_marker, unsigned long iVertex,
                                           su2double thermal_conductivity, su2double dist_ij, su2double There,
                                           su2double Temperature_Ref) const {
  /*--- Compute the normal gradient in temperature using Twall ---*/

  const su2double Tconjugate = GetConjugateHeatVariable(val_marker, iVertex, 0) / Temperature_Ref;

  su2double Twall = 0.0;

  if ((config->GetKind_CHT_Coupling() == CHT_COUPLING::AVERAGED_TEMPERATURE_NEUMANN_HEATFLUX) ||
      (config->GetKind_CHT_Coupling() == CHT_COUPLING::AVERAGED_TEMPERATURE_ROBIN_HEATFLUX)) {
    /*--- Compute wall temperature from both temperatures ---*/

    su2double HF_FactorHere = thermal_conductivity * config->GetViscosity_Ref() / dist_ij;
    su2double HF_FactorConjugate = GetConjugateHeatVariable(val_marker, iVertex, 2);

    Twall = (There * HF_FactorHere + Tconjugate * HF_FactorConjugate) / (HF_FactorHere + HF_FactorConjugate);
  } else if ((config->GetKind_CHT_Coupling() == CHT_COUPLING::DIRECT_TEMPERATURE_NEUMANN_HEATFLUX) ||
             (config->GetKind_CHT_Coupling() == CHT_COUPLING::DIRECT_TEMPERATURE_ROBIN_HEATFLUX)) {
    /*--- (Directly) Set wall temperature to conjugate temperature. ---*/

    Twall = Tconjugate;
  } else {
    SU2_MPI::Error("Unknown CHT coupling method.", CURRENT_FUNCTION);
  }

  return Twall;
}

void CNSSolver::BC_Isothermal_Wall_Generic_Blowing(CGeometry* geometry, CSolver** solver_container,
                                                   CNumerics* conv_numerics, CNumerics* visc_numerics, CConfig* config,
                                                   unsigned short val_marker, bool cht_mode) {
  unsigned short iDim;
  unsigned long iVertex, iPoint;
  su2double P_Total, T_Total, Velocity[3], Velocity2, H_Total, Temperature, Riemann, Pressure, Density, Energy,
      *Flow_Dir, Mach2, SoundSpeed2, SoundSpeed_Total2, Vel_Mag, alpha, aa, bb, cc, dd, Area, UnitNormal[3],
      blowingNormal[3];
  su2double *V_inlet, *V_domain;

  bool implicit = (config->GetKind_TimeIntScheme() == EULER_IMPLICIT);
  su2double Two_Gamma_M1 = 2.0 / Gamma_Minus_One;
  su2double Gas_Constant = config->GetGas_ConstantND();
  INLET_TYPE Kind_Inlet = config->GetKind_Inlet();
  string Marker_Tag = config->GetMarker_All_TagBound(val_marker);
  bool tkeNeeded =
      (config->GetKind_Turb_Model() == TURB_MODEL::SST) || (config->GetKind_Turb_Model() == TURB_MODEL::SST_SUST);
  su2double* Normal = new su2double[nDim];

  su2double VelMagnitude2_i, *turb_ke, StaticEnergy, Kappa, Chi, Enthalpy;

  /*---- Read the temperature to impose in the blowing region from file (if exists) ---*/
  /*---- (If it does not exist, do not impose temperature) ----*/
  ifstream infile;    
  if (Imposed_Temperature == 0) {
        infile.open("blowingTemperature.dat");
  	if(infile.fail())
    	{
       	 	Imposed_Temperature = -99.0;
		cout << "Free temperature in the blowing regions \n";
   	 }
  	else
    	{
       		infile >> Imposed_Temperature;
		cout << "Setting temperature in the blowing regions: " << Imposed_Temperature << "\n";
		infile.close(); 
    	}
  }
  /*--- Loop over all the vertices on this boundary marker ---*/

  SU2_OMP_FOR_DYN(OMP_MIN_SIZE)
  for (iVertex = 0; iVertex < geometry->nVertex[val_marker]; iVertex++) {

    /*--- Allocate the value at the inlet ---*/

    V_inlet = GetCharacPrimVar(val_marker, iVertex);

    iPoint = geometry->vertex[val_marker][iVertex]->GetNode();

    /*--- Check if the node belongs to the domain (i.e., not a halo node) ---*/

    if (geometry->nodes->GetDomain(iPoint)) {
      /*--- Normal vector for this vertex (negate for outward convention) ---*/

      geometry->vertex[val_marker][iVertex]->GetNormal(Normal);
      for (iDim = 0; iDim < nDim; iDim++) Normal[iDim] = -Normal[iDim];
      conv_numerics->SetNormal(Normal);

      Area = GeometryToolbox::Norm(nDim, Normal);
      for (iDim = 0; iDim < nDim; iDim++) {
        UnitNormal[iDim] = Normal[iDim] / Area;
        blowingNormal[iDim] = -Normal[iDim] / Area;
      }

      /*--- Retrieve solution at this boundary node ---*/

      V_domain = nodes->GetPrimitive(iPoint);

      /*--- Build the fictitious intlet state based on characteristics ---*/

      /*--- Subsonic inflow: there is one outgoing characteristic (u-c),
         therefore we can specify all but one state variable at the inlet.
         The outgoing Riemann invariant provides the final piece of info.
         Adapted from an original implementation in the Stanford University
         multi-block (SUmb) solver in the routine bcSubsonicInflow.f90
         written by Edwin van der Weide, last modified 04-20-2009. ---*/

      // get variables from marker blowing
      Density = config->GetBlowing_Density(Marker_Tag);      
      Vel_Mag = config->GetBlowing_VelocityMag(Marker_Tag); 
      //cout << "Density: " << Density << endl;
      //cout << "Vel Mag: " << Vel_Mag << endl;

      su2double* Velocity_Inf = config->GetVelocity_FreeStreamND();
      const su2double Vel_Infty_Mag = GeometryToolbox::Norm(nDim, Velocity_Inf);

      if (config->GetInlet_Profile_From_File()==false) {
        if (Kind_Inlet == VELOCITY_BLOW || Kind_Inlet == MASS_FLOW){
          Density = nodes->GetDensity(iPoint);
          Vel_Mag *= Vel_Infty_Mag;
        } 
        else if (Kind_Inlet == MASS_FLOW_BLOW){
          Density = nodes->GetDensity(iPoint);
          Vel_Mag /= Density;
        }
        else{
          cout << "Inlet Type not supported by Marker Blowing." << endl;
        }
      }  
      Flow_Dir = blowingNormal;
    
      
      if (config->GetInlet_Profile_From_File()) {
        if (Kind_Inlet == VELOCITY_BLOW || Kind_Inlet == MASS_FLOW){
          Density = Inlet_Ttotal[val_marker][iVertex];
          Density = nodes->GetDensity(iPoint);
          
	  //Pressure = nodes->GetPressure(iPoint);
          //SoundSpeed2 = Gamma * Pressure / V_domain[nDim + 2];
	  //Density = Pressure*Gamma/SoundSpeed2;

          Vel_Mag = Inlet_Ptotal[val_marker][iVertex];
          Vel_Mag *= Vel_Infty_Mag;
        }
        else if (Kind_Inlet == MASS_FLOW_BLOW){ 
          Density = Inlet_Ttotal[val_marker][iVertex];
          Density = nodes->GetDensity(iPoint);
          
	  //Pressure = nodes->GetPressure(iPoint);
          //SoundSpeed2 = Gamma * Pressure / V_domain[nDim + 2];
	  //Density = Pressure*Gamma/SoundSpeed2;

	  su2double Mass_Flow = Inlet_Ptotal[val_marker][iVertex];
          Vel_Mag = Mass_Flow/Density;
        } 
      }

      /*--- Non-dim. the inputs if necessary. ---*/

      Density /= config->GetDensity_Ref();
      Vel_Mag /= config->GetVelocity_Ref();

      /*--- Get primitives from current inlet state. ---*/

      for (iDim = 0; iDim < nDim; iDim++) Velocity[iDim] = nodes->GetVelocity(iPoint, iDim);
      Pressure = nodes->GetPressure(iPoint);
      SoundSpeed2 = Gamma * Pressure / V_domain[nDim + 2];

      /*--- Compute the acoustic Riemann invariant that is extrapolated
         from the domain interior. ---*/

      Riemann = Two_Gamma_M1 * sqrt(SoundSpeed2);
      for (iDim = 0; iDim < nDim; iDim++) Riemann += Velocity[iDim] * UnitNormal[iDim];

      /*--- Speed of sound squared for fictitious inlet state ---*/

      SoundSpeed2 = Riemann;
      for (iDim = 0; iDim < nDim; iDim++) SoundSpeed2 -= Vel_Mag * Flow_Dir[iDim] * UnitNormal[iDim];

      SoundSpeed2 = max(0.0, 0.5 * Gamma_Minus_One * SoundSpeed2);
      SoundSpeed2 = SoundSpeed2 * SoundSpeed2;

      /*--- Pressure for the fictitious inlet state ---*/

      Pressure = SoundSpeed2 * Density / Gamma;

      /*--- Energy for the fictitious inlet state ---*/

      Energy = Pressure / (Density * Gamma_Minus_One) + 0.5 * Vel_Mag * Vel_Mag;
      if (tkeNeeded) Energy += GetTke_Inf();


      if (Vel_Mag > 0 && Imposed_Temperature>0)  {                                 // XXX
      Temperature = Imposed_Temperature;
      Density = Pressure / (Gas_Constant * Temperature);  // XXX
      Energy =  Temperature * Gas_Constant / Gamma_Minus_One + 0.5 * Vel_Mag * Vel_Mag;
      }                                             // XXX


      // >>>
      // Energy = nodes->GetEnergy(iPoint);

      // VelMagnitude2_i = 0.0;
      // for (iDim = 0; iDim < nDim; iDim++) {
      //   //Velocity_i[iDim] = node[iPoint]->GetVelocity(iDim);
      //   //ProjVelocity_i += Velocity_i[iDim]*UnitNormal[iDim];
      //   VelMagnitude2_i += Velocity[iDim]*Velocity[iDim];
      // }

      // if (tkeNeeded) turb_ke = solver_container[TURB_SOL]->GetNodes()->GetSolution(iPoint);
      // StaticEnergy = Energy - 0.5 * VelMagnitude2_i  - turb_ke[0];
      // Energy = StaticEnergy + 0.5 * Vel_Mag * Vel_Mag + GetTke_Inf();

      // CFluidModel* auxFluidModel = nullptr;
      // auxFluidModel = new CIdealGas(1.4, config->GetGas_Constant());

      // auxFluidModel->SetTDState_rhoe(Density, StaticEnergy);
      // Kappa = auxFluidModel->GetdPde_rho() / Density;
      // Chi = auxFluidModel->GetdPdrho_e() - Kappa * StaticEnergy;
      // Pressure = auxFluidModel->GetPressure();
      // Enthalpy = Energy + Pressure/Density;
      // <<<

      /*--- Primitive variables, using the derived quantities ---*/

      V_inlet[0] = Pressure / (Gas_Constant * Density);
      for (iDim = 0; iDim < nDim; iDim++) V_inlet[iDim + 1] = Vel_Mag * Flow_Dir[iDim];
      V_inlet[nDim + 1] = Pressure;
      V_inlet[nDim + 2] = Density;
      V_inlet[nDim + 3] = Energy + Pressure / Density;
     
      /*--- Set various quantities in the solver class ---*/

      conv_numerics->SetPrimitive(V_domain, V_inlet);
      visc_numerics->SetPrimitive(V_domain, V_inlet);

      if (dynamic_grid)
        conv_numerics->SetGridVel(geometry->nodes->GetGridVel(iPoint), geometry->nodes->GetGridVel(iPoint));

      /*--- Compute the residual using an upwind scheme ---*/

      auto residual = conv_numerics->ComputeResidual(config);
      //auto v_residual = visc_numerics->ComputeResidual(config);

      /*--- Update residual value ---*/

      LinSysRes.AddBlock(iPoint, residual);
      //if (Density == -99) {
      //  LinSysRes.AddBlock(iPoint, v_residual);
      //}

      /*--- Jacobian contribution for implicit integration ---*/

      if (implicit) {
        Jacobian.AddBlock2Diag(iPoint, residual.jacobian_i);
      //  if (Density == -99) {
      //    Jacobian.AddBlock2Diag(iPoint, v_residual.jacobian_i);
      // }
      }

      su2double Solution[MAXNVAR] = {0.0};
      Solution[0] = Density;
      for (auto iDim = 0; iDim < nDim; iDim++) Solution[iDim + 1] = V_inlet[iDim + 1] * Density;
      Solution[nDim + 1] = Energy * Density;

      for (auto iDim = (1u - int((Vel_Mag > 0) && (Imposed_Temperature>0))); iDim < (nVar - 1+int((Vel_Mag > 0) && (Imposed_Temperature>0))); iDim++) {
        nodes->SetSolution_Old(iPoint, iDim, Solution[iDim]);
        LinSysRes(iPoint, iDim) = 0.0;  // blowingVelocity[iDim];
        // LinSysRes(iPoint, 1) = V_inlet[1];
        // LinSysRes(iPoint, 2) = V_inlet[2];
        nodes->SetVal_ResTruncError_Zero(iPoint, iDim);
      }

      for (auto iVar = (1u -int((Vel_Mag > 0) && (Imposed_Temperature>0))); iVar <= (nDim+int((Vel_Mag > 0) && (Imposed_Temperature>0))) ; iVar++) {
        auto total_index = iPoint * nVar + iVar;
        Jacobian.DeleteValsRowi(total_index);
      }

      // su2double Solution[MAXNVAR] = {0.0};
      // Solution[0] = Density;
      // for (auto iDim = 0; iDim < nDim; iDim++)
      //     Solution[iDim+1] = V_inlet[iDim+1]*Density;
      // Solution[nDim+1] = Energy*Density;

      // su2double V_blowing[nDim];
      // for (iDim = 0; iDim < nDim; iDim++)
      //       V_blowing[iDim] = Vel_Mag*Flow_Dir[iDim];

      // nodes->SetVelocity_Old(iPoint,V_blowing);
      // for (auto iDim = 0u; iDim < nDim; iDim++){
      //     LinSysRes(iPoint, iDim+1) = 0.0; // blowingVelocity[iDim]
      // }
      // nodes->SetVel_ResTruncError_Zero(iPoint);

      // for (auto iVar = 1u; iVar <= nDim; iVar++) {
      //     auto total_index = iPoint*nVar+iVar;
      //     Jacobian.DeleteValsRowi(total_index);
      // }

      // // >>> start test section
      // /*--- Apply a weak boundary condition for the energy equation.
      //  Compute the residual due to the prescribed heat flux.
      //  The convective part will be zero if the grid is not moving. ---*/
      // su2double Wall_HeatFlux = 0.0;
      // su2double Res_Conv = 0.0;
      // su2double Res_Visc = Wall_HeatFlux * Area;

      // /*--- Impose the value of the velocity as a strong boundary
      //  condition (Dirichlet). Fix the velocity and remove any
      //  contribution to the residual at this node. ---*/

      // nodes->SetVelocity_Old(iPoint, geometry->nodes->GetGridVel(iPoint));

      // for (auto iDim = 0u; iDim < nDim; iDim++)
      //   LinSysRes(iPoint, iDim+1) = 0.0;
      // nodes->SetVel_ResTruncError_Zero(iPoint);

      // /*--- If the wall is moving, there are additional residual contributions
      //  due to pressure (p v_wall.n) and shear stress (tau.v_wall.n). ---*/

      //   if (implicit) {
      //     for (auto iVar = 0u; iVar < nVar; ++iVar)
      //       Jacobian_i[nDim+1][iVar] = 0.0;
      //   }

      //   const auto Point_Normal = geometry->vertex[val_marker][iVertex]->GetNormal_Neighbor();

      //   AddDynamicGridResidualContribution(iPoint, Point_Normal, geometry, UnitNormal,
      //                                      Area, geometry->nodes->GetGridVel(iPoint),
      //                                      Jacobian_i, Res_Conv, Res_Visc);

      // /*--- Convective and viscous contributions to the residual at the wall ---*/

      // LinSysRes(iPoint, nDim+1) += Res_Conv - Res_Visc;

      //   for (auto iVar = 1u; iVar <= nDim; iVar++) {
      //     auto total_index = iPoint*nVar+iVar;
      //     Jacobian.DeleteValsRowi(total_index);
      //   }

      // // <<< end test section
    }
  }
  END_SU2_OMP_FOR

  /*--- Free locally allocated memory ---*/

  delete[] Normal;
}

void CNSSolver::BC_Isothermal_Wall_Generic(CGeometry* geometry, CSolver** solver_container, CNumerics* conv_numerics,
                                           CNumerics* visc_numerics, CConfig* config, unsigned short val_marker,
                                           bool cht_mode) {
  const bool implicit = (config->GetKind_TimeIntScheme() == EULER_IMPLICIT);
  const su2double Temperature_Ref = config->GetTemperature_Ref();
  const su2double Prandtl_Lam = config->GetPrandtl_Lam();
  const su2double Prandtl_Turb = config->GetPrandtl_Turb();
  const su2double Gas_Constant = config->GetGas_ConstantND();
  const su2double Cp = (Gamma / Gamma_Minus_One) * Gas_Constant;

  /*--- Identify the boundary and retrieve the specified wall temperature from
   the config (for non-CHT problems) as well as the wall function treatment. ---*/

  const auto Marker_Tag = config->GetMarker_All_TagBound(val_marker);
  su2double Twall = 0.0;
  if (!cht_mode) {
    Twall = config->GetIsothermal_Temperature(Marker_Tag) / Temperature_Ref;
  }

  //  Wall_Function = config->GetWallFunction_Treatment(Marker_Tag);
  //  if (Wall_Function != WALL_FUNCTION::NONE) {
  //    SU2_MPI::Error("Wall function treatment not implemented yet", CURRENT_FUNCTION);
  //  }

  su2double** Jacobian_i = nullptr;
  if (implicit) {
    Jacobian_i = new su2double*[nVar];
    for (auto iVar = 0u; iVar < nVar; iVar++) Jacobian_i[iVar] = new su2double[nVar]();
  }

  /*--- Loop over boundary points ---*/

  SU2_OMP_FOR_DYN(OMP_MIN_SIZE)
  for (auto iVertex = 0u; iVertex < geometry->nVertex[val_marker]; iVertex++) {
    const auto iPoint = geometry->vertex[val_marker][iVertex]->GetNode();

    if (!geometry->nodes->GetDomain(iPoint)) continue;

    /*--- Compute dual-grid area and boundary normal ---*/

    const auto Normal = geometry->vertex[val_marker][iVertex]->GetNormal();

    su2double Area = GeometryToolbox::Norm(nDim, Normal);

    su2double UnitNormal[MAXNDIM] = {0.0};
    for (auto iDim = 0u; iDim < nDim; iDim++) UnitNormal[iDim] = -Normal[iDim] / Area;

    /*--- Compute closest normal neighbor ---*/

    const auto Point_Normal = geometry->vertex[val_marker][iVertex]->GetNormal_Neighbor();

    /*--- Get coordinates of i & nearest normal and compute distance ---*/

    const auto Coord_i = geometry->nodes->GetCoord(iPoint);
    const auto Coord_j = geometry->nodes->GetCoord(Point_Normal);

    su2double dist_ij = GeometryToolbox::Distance(nDim, Coord_i, Coord_j);

    /*--- Store the corrected velocity at the wall which will
     be zero (v = 0), unless there is grid motion (v = u_wall)---*/

    if (dynamic_grid) {
      nodes->SetVelocity_Old(iPoint, geometry->nodes->GetGridVel(iPoint));
    } else {
      su2double zero[MAXNDIM] = {0.0};
      nodes->SetVelocity_Old(iPoint, zero);
    }

    for (auto iDim = 0u; iDim < nDim; iDim++) LinSysRes(iPoint, iDim + 1) = 0.0;
    nodes->SetVel_ResTruncError_Zero(iPoint);

    /*--- Get transport coefficients ---*/

    su2double laminar_viscosity = nodes->GetLaminarViscosity(iPoint);
    su2double eddy_viscosity = nodes->GetEddyViscosity(iPoint);
    su2double thermal_conductivity = Cp * (laminar_viscosity / Prandtl_Lam + eddy_viscosity / Prandtl_Turb);

    // work in progress on real-gases...
    // thermal_conductivity = nodes->GetThermalConductivity(iPoint);
    // Cp = nodes->GetSpecificHeatCp(iPoint);
    // thermal_conductivity += Cp*eddy_viscosity/Prandtl_Turb;

    /*--- If it is a customizable or CHT patch, retrieve the specified wall temperature. ---*/

    const su2double There = nodes->GetTemperature(Point_Normal);

    if (cht_mode) {
      Twall = GetCHTWallTemperature(config, val_marker, iVertex, dist_ij, thermal_conductivity, There, Temperature_Ref);
    } else if (config->GetMarker_All_PyCustom(val_marker)) {
      Twall = geometry->GetCustomBoundaryTemperature(val_marker, iVertex);
    }

    /*--- Compute the normal gradient in temperature using Twall ---*/

    su2double dTdn = -(There - Twall) / dist_ij;

    /*--- Apply a weak boundary condition for the energy equation.
     Compute the residual due to the prescribed heat flux. ---*/

    su2double Res_Conv = 0.0;
    su2double Res_Visc = thermal_conductivity * dTdn * Area;

    /*--- Calculate Jacobian for implicit time stepping ---*/

    if (implicit) {
      /*--- Add contributions to the Jacobian from the weak enforcement of the energy equations. ---*/

      su2double Density = nodes->GetDensity(iPoint);
      su2double Vel2 = GeometryToolbox::SquaredNorm(nDim, &nodes->GetPrimitive(iPoint)[1]);
      su2double dTdrho = 1.0 / Density * (-Twall + (Gamma - 1.0) / Gas_Constant * (Vel2 / 2.0));

      Jacobian_i[nDim + 1][0] = thermal_conductivity / dist_ij * dTdrho * Area;

      for (auto jDim = 0u; jDim < nDim; jDim++) Jacobian_i[nDim + 1][jDim + 1] = 0.0;

      Jacobian_i[nDim + 1][nDim + 1] = thermal_conductivity / dist_ij * (Gamma - 1.0) / (Gas_Constant * Density) * Area;
    }

    /*--- If the wall is moving, there are additional residual contributions
     due to pressure (p v_wall.n) and shear stress (tau.v_wall.n). ---*/

    if (dynamic_grid) {
      AddDynamicGridResidualContribution(iPoint, Point_Normal, geometry, UnitNormal, Area,
                                         geometry->nodes->GetGridVel(iPoint), Jacobian_i, Res_Conv, Res_Visc);
    }

    /*--- Convective and viscous contributions to the residual at the wall ---*/

    LinSysRes(iPoint, nDim + 1) += Res_Conv - Res_Visc;

    /*--- Enforce the no-slip boundary condition in a strong way by
     modifying the velocity-rows of the Jacobian (1 on the diagonal).
     And add the contributions to the Jacobian due to energy. ---*/

    if (implicit) {
      Jacobian.AddBlock2Diag(iPoint, Jacobian_i);

      for (auto iVar = 1u; iVar <= nDim; iVar++) {
        auto total_index = iPoint * nVar + iVar;
        Jacobian.DeleteValsRowi(total_index);
      }
    }
  }
  END_SU2_OMP_FOR

  if (Jacobian_i)
    for (auto iVar = 0u; iVar < nVar; iVar++) delete[] Jacobian_i[iVar];
  delete[] Jacobian_i;
}

void CNSSolver::BC_Isothermal_Wall(CGeometry* geometry, CSolver** solver_container, CNumerics* conv_numerics,
                                   CNumerics* visc_numerics, CConfig* config, unsigned short val_marker) {
  BC_Isothermal_Wall_Generic(geometry, solver_container, conv_numerics, visc_numerics, config, val_marker);
}

void CNSSolver::BC_Isothermal_Wall_Blowing(CGeometry* geometry, CSolver** solver_container, CNumerics* conv_numerics,
                                           CNumerics* visc_numerics, CConfig* config, unsigned short val_marker) {
  BC_Isothermal_Wall_Generic_Blowing(geometry, solver_container, conv_numerics, visc_numerics, config, val_marker);
}

void CNSSolver::BC_ConjugateHeat_Interface(CGeometry* geometry, CSolver** solver_container, CNumerics* conv_numerics,
                                           CConfig* config, unsigned short val_marker) {
  BC_Isothermal_Wall_Generic(geometry, solver_container, conv_numerics, nullptr, config, val_marker, true);
}

void CNSSolver::SetTau_Wall_WF(CGeometry* geometry, CSolver** solver_container, const CConfig* config) {
  /*---
   The wall function implemented herein is based on Nichols and Nelson, AIAA J. v32 n6 2004.
   ---*/

  unsigned long notConvergedCounter = 0; /*--- counts the number of wall cells that are not converged ---*/
  unsigned long smallYPlusCounter = 0;   /*--- counts the number of wall cells where y+ < 5 ---*/

  const su2double Gas_Constant = config->GetGas_ConstantND();
  const su2double Cp = (Gamma / Gamma_Minus_One) * Gas_Constant;
  const unsigned short max_iter = config->GetwallModel_MaxIter();
  const su2double relax = config->GetwallModel_RelFac();

  /*--- Compute the recovery factor
   * use Molecular (Laminar) Prandtl number (see Nichols & Nelson, nomenclature ) ---*/

  const su2double Recovery = pow(config->GetPrandtl_Lam(), (1.0 / 3.0));

  /*--- Typical constants from boundary layer theory ---*/

  const su2double kappa = config->GetwallModel_Kappa();
  const su2double B = config->GetwallModel_B();

  for (auto iMarker = 0u; iMarker < config->GetnMarker_All(); iMarker++) {
    if (!config->GetViscous_Wall(iMarker)) continue;

    /*--- Identify the boundary by string name ---*/

    const auto Marker_Tag = config->GetMarker_All_TagBound(iMarker);

    /*--- Jump to another BC if it is not wall function ---*/

    if (config->GetWallFunction_Treatment(Marker_Tag) != WALL_FUNCTIONS::STANDARD_FUNCTION) continue;

    /*--- Loop over all of the vertices on this boundary marker ---*/

    SU2_OMP_FOR_DYN(OMP_MIN_SIZE)
    for (auto iVertex = 0u; iVertex < geometry->nVertex[iMarker]; iVertex++) {
      const auto iPoint = geometry->vertex[iMarker][iVertex]->GetNode();
      const auto Point_Normal = geometry->vertex[iMarker][iVertex]->GetNormal_Neighbor();

      /*--- Check if the node belongs to the domain (i.e, not a halo node)
       *    and the neighbor is not part of the physical boundary ---*/

      if (!geometry->nodes->GetDomain(iPoint)) continue;

      /*--- Get coordinates of the current vertex and nearest normal point ---*/

      const auto Coord = geometry->nodes->GetCoord(iPoint);
      const auto Coord_Normal = geometry->nodes->GetCoord(Point_Normal);

      /*--- Compute dual-grid area and boundary normal ---*/

      const auto Normal = geometry->vertex[iMarker][iVertex]->GetNormal();

      const su2double Area = GeometryToolbox::Norm(nDim, Normal);

      su2double UnitNormal[MAXNDIM] = {0.0};
      for (auto iDim = 0u; iDim < nDim; iDim++) UnitNormal[iDim] = -Normal[iDim] / Area;

      /*--- Get the velocity, pressure, and temperature at the nearest
       (normal) interior point. ---*/

      su2double Vel[MAXNDIM] = {0.0};
      for (auto iDim = 0u; iDim < nDim; iDim++) Vel[iDim] = nodes->GetVelocity(Point_Normal, iDim);

      const su2double P_Normal = nodes->GetPressure(Point_Normal);
      const su2double T_Normal = nodes->GetTemperature(Point_Normal);

      /*--- Compute the wall-parallel velocity at first point off the wall ---*/

      const su2double VelNormal = GeometryToolbox::DotProduct(int(MAXNDIM), Vel, UnitNormal);

      su2double VelTang[MAXNDIM] = {0.0};
      for (auto iDim = 0u; iDim < nDim; iDim++) VelTang[iDim] = Vel[iDim] - VelNormal * UnitNormal[iDim];

      const su2double VelTangMod = GeometryToolbox::Norm(int(MAXNDIM), VelTang);

      /*--- Compute normal distance of the interior point from the wall ---*/

      su2double WallDist[MAXNDIM] = {0.0};
      GeometryToolbox::Distance(nDim, Coord, Coord_Normal, WallDist);

      const su2double WallDistMod = GeometryToolbox::Norm(int(MAXNDIM), WallDist);

      /*--- Initial value for wall temperature ---*/

      su2double q_w = 0.0;

      if (config->GetMarker_All_KindBC(iMarker) == HEAT_FLUX) {
        q_w = config->GetWall_HeatFlux(Marker_Tag) / config->GetHeat_Flux_Ref();
      }

      /*--- Extrapolate the pressure from the interior & compute the
       wall density using the equation of state ---*/

      /*--- Compressible formulation ---*/

      su2double T_Wall = nodes->GetTemperature(iPoint);
      const su2double P_Wall = P_Normal;
      su2double Density_Wall = P_Wall / (Gas_Constant * T_Wall);
      const su2double Lam_Visc_Normal = nodes->GetLaminarViscosity(Point_Normal);
      const su2double Conductivity_Wall = nodes->GetThermalConductivity(iPoint);

      /*--- Compute the shear stress at the wall in the regular fashion
       *    by using the stress tensor on the surface ---*/

      su2double tau[MAXNDIM][MAXNDIM] = {{0.0}};
      const su2double Lam_Visc_Wall = nodes->GetLaminarViscosity(iPoint);
      su2double Eddy_Visc_Wall = nodes->GetEddyViscosity(iPoint);

      CNumerics::ComputeStressTensor(nDim, tau, nodes->GetGradient_Primitive(iPoint) + 1, Lam_Visc_Wall);

      su2double TauTangent[MAXNDIM] = {0.0};
      GeometryToolbox::TangentProjection(nDim, tau, UnitNormal, TauTangent);

      const su2double WallShearStress = GeometryToolbox::Norm(int(MAXNDIM), TauTangent);

      /*--- Calculate the quantities from boundary layer theory and
       *    iteratively solve for a new wall shear stress. Use the current wall
       *    shear stress as a starting guess for the wall function. ---*/

      unsigned long counter = 0;
      su2double diff = 1.0;
      su2double U_Tau = max(1.0e-6, sqrt(WallShearStress / Density_Wall));
      /*--- Use minimum y+ as defined in the config, in case the routine below for computing y+ does not converge ---*/
      su2double Y_Plus = 0.99 * config->GetwallModel_MinYPlus();  // use clipping value as minimum

      const su2double Y_Plus_Start = Density_Wall * U_Tau * WallDistMod / Lam_Visc_Wall;

      /*--- Automatic switch off when y+ < "limit" according to Nichols & Nelson (2004) ---*/

      if (Y_Plus_Start < config->GetwallModel_MinYPlus()) {
        smallYPlusCounter++;
        continue;
      }

      /*--- Convergence criterium for the Newton solver, note that 1e-10 is too large ---*/
      const su2double tol = 1e-12;
      while (fabs(diff) > tol) {
        /*--- Friction velocity and u+ ---*/

        const su2double U_Plus = VelTangMod / U_Tau;

        /*--- Gamma, Beta, Q, and Phi, defined by Nichols & Nelson (2004) page 1110 ---*/

        const su2double Gam = Recovery * U_Tau * U_Tau / (2.0 * Cp * T_Wall);
        const su2double Beta = q_w * Lam_Visc_Wall / (Density_Wall * T_Wall * Conductivity_Wall * U_Tau);
        const su2double Q = sqrt(Beta * Beta + 4.0 * Gam);
        const su2double Phi = asin(-1.0 * Beta / Q);

        /*--- Crocco-Busemann equation for wall temperature (eq. 11 of Nichols and Nelson) ---*/
        /*--- update T_Wall due to aerodynamic heating, unless the wall is isothermal      ---*/

        if (config->GetMarker_All_KindBC(iMarker) != ISOTHERMAL) {
          const su2double denum = (1.0 + Beta * U_Plus - Gam * U_Plus * U_Plus);
          if (denum > EPS) {
            T_Wall = T_Normal / denum;
            nodes->SetTemperature(iPoint, T_Wall);
          } else {
            cout << "Warning: T_Wall < 0 " << endl;
          }
        }

        /*--- update of wall density using the wall temperature ---*/
        Density_Wall = P_Wall / (Gas_Constant * T_Wall);

        /*--- Y+ defined by White & Christoph (compressibility and heat transfer) negative value for (2.0*Gam*U_Plus -
         * Beta)/Q ---*/

        const su2double Y_Plus_White =
            exp((kappa / sqrt(Gam)) * (asin((2.0 * Gam * U_Plus - Beta) / Q) - Phi)) * exp(-1.0 * kappa * B);

        /*--- Spalding's universal form for the BL velocity with the
         *    outer velocity form of White & Christoph above. ---*/
        const su2double kUp = kappa * U_Plus;
        Y_Plus =
            U_Plus + Y_Plus_White - (exp(-1.0 * kappa * B) * (1.0 + kUp + 0.5 * kUp * kUp + kUp * kUp * kUp / 6.0));

        const su2double dypw_dyp =
            2.0 * Y_Plus_White * (kappa * sqrt(Gam) / Q) * sqrt(1.0 - pow(2.0 * Gam * U_Plus - Beta, 2.0) / (Q * Q));

        Eddy_Visc_Wall = Lam_Visc_Wall * (1.0 + dypw_dyp -
                                          kappa * exp(-1.0 * kappa * B) *
                                              (1.0 + kappa * U_Plus + kappa * kappa * U_Plus * U_Plus / 2.0) -
                                          Lam_Visc_Normal / Lam_Visc_Wall);
        Eddy_Visc_Wall = max(1.0e-6, Eddy_Visc_Wall);

        /* --- Define function for Newton method to zero --- */

        diff = (Density_Wall * U_Tau * WallDistMod / Lam_Visc_Wall) - Y_Plus;

        /* --- Gradient of function defined above --- */

        const su2double grad_diff = Density_Wall * WallDistMod / Lam_Visc_Wall + VelTangMod / (U_Tau * U_Tau) +
                                    kappa / (U_Tau * sqrt(Gam)) * asin(U_Plus * sqrt(Gam)) * Y_Plus_White -
                                    exp(-1.0 * B * kappa) *
                                        (0.5 * pow(VelTangMod * kappa / U_Tau, 3) + pow(VelTangMod * kappa / U_Tau, 2) +
                                         VelTangMod * kappa / U_Tau) /
                                        U_Tau;

        /* --- Newton Step --- */

        U_Tau = U_Tau - relax * (diff / grad_diff);

        counter++;
        if (counter > max_iter) {
          notConvergedCounter++;
          // use some safe values for convergence
          Y_Plus = 30.0;
          Eddy_Visc_Wall = 1.0;
          U_Tau = 1.0;
          break;
        }
      }

      /*--- Calculate an updated value for the wall shear stress
       *    using the y+ value, the definition of y+, and the definition of
       *    the friction velocity. ---*/

      YPlus[iMarker][iVertex] = Y_Plus;
      EddyViscWall[iMarker][iVertex] = Eddy_Visc_Wall;
      UTau[iMarker][iVertex] = U_Tau;

      const su2double Tau_Wall = (1.0 / Density_Wall) * pow(Y_Plus * Lam_Visc_Wall / WallDistMod, 2.0);

      /*--- Store this value for the wall shear stress at the node.  ---*/

      nodes->SetTau_Wall(iPoint, Tau_Wall);
    }
    END_SU2_OMP_FOR
  }

  if (config->GetComm_Level() == COMM_FULL) {
    static unsigned long globalCounter1, globalCounter2;

    ompMasterAssignBarrier(globalCounter1, 0, globalCounter2, 0);

    SU2_OMP_ATOMIC
    globalCounter1 += notConvergedCounter;

    SU2_OMP_ATOMIC
    globalCounter2 += smallYPlusCounter;

    SU2_OMP_BARRIER
    SU2_OMP_MASTER {
      SU2_MPI::Allreduce(&globalCounter1, &notConvergedCounter, 1, MPI_UNSIGNED_LONG, MPI_SUM, SU2_MPI::GetComm());
      SU2_MPI::Allreduce(&globalCounter2, &smallYPlusCounter, 1, MPI_UNSIGNED_LONG, MPI_SUM, SU2_MPI::GetComm());

      if (rank == MASTER_NODE) {
        if (notConvergedCounter)
          cout << "Warning: Computation of wall coefficients (y+) did not converge in " << notConvergedCounter
               << " points." << endl;

        if (smallYPlusCounter)
          cout << "Warning: y+ < 5.0 in " << smallYPlusCounter << " points, for which the wall model is not active."
               << endl;
      }
    }
    END_SU2_OMP_MASTER
  }
}
