/*!
 * \file CIncNSSolver.cpp
 * \brief Main subroutines for solving Navier-Stokes incompressible flow.
 * \author F. Palacios, T. Economon
 * \version 7.1.0 "Blackbird"
 *
 * SU2 Project Website: https://su2code.github.io
 *
 * The SU2 Project is maintained by the SU2 Foundation
 * (http://su2foundation.org)
 *
 * Copyright 2012-2020, SU2 Contributors (cf. AUTHORS.md)
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

#include "../../include/solvers/CIncNSSolver.hpp"
#include "../../include/variables/CIncNSVariable.hpp"
#include "../../../Common/include/toolboxes/printing_toolbox.hpp"
#include "../../include/solvers/CFVMFlowSolverBase.inl"

/*--- Explicit instantiation of the parent class of CIncEulerSolver,
 *    to spread the compilation over two cpp files. ---*/
template class CFVMFlowSolverBase<CIncEulerVariable, INCOMPRESSIBLE>;


CIncNSSolver::CIncNSSolver(CGeometry *geometry, CConfig *config, unsigned short iMesh) :
  CIncEulerSolver(geometry, config, iMesh, true) {

  /*--- Read farfield conditions from config ---*/

  Viscosity_Inf   = config->GetViscosity_FreeStreamND();
  Tke_Inf         = config->GetTke_FreeStreamND();

  /*--- Initialize the secondary values for direct derivative approxiations ---*/

  switch (config->GetDirectDiff()) {
    case D_VISCOSITY:
      SU2_TYPE::SetDerivative(Viscosity_Inf, 1.0);
      break;
    default:
      break;
  }
}

void CIncNSSolver::Preprocessing(CGeometry *geometry, CSolver **solver_container, CConfig *config, unsigned short iMesh,
                                 unsigned short iRKStep, unsigned short RunTime_EqSystem, bool Output) {

  const auto InnerIter = config->GetInnerIter();
  const bool muscl = config->GetMUSCL_Flow() && (iMesh == MESH_0);
  const bool center = (config->GetKind_ConvNumScheme_Flow() == SPACE_CENTERED);
  const bool limiter = (config->GetKind_SlopeLimit_Flow() != NO_LIMITER) && (InnerIter <= config->GetLimiterIter());
  const bool van_albada = (config->GetKind_SlopeLimit_Flow() == VAN_ALBADA_EDGE);
  const bool energy = config->GetEnergy_Equation();

  /*--- Common preprocessing steps (implemented by CEulerSolver) ---*/

  CommonPreprocessing(geometry, solver_container, config, iMesh, iRKStep, RunTime_EqSystem, Output);

  /*--- Compute gradient for MUSCL reconstruction ---*/

  if (config->GetReconstructionGradientRequired() && muscl && !center) {
    switch (config->GetKind_Gradient_Method_Recon()) {
      case GREEN_GAUSS:
        SetPrimitive_Gradient_GG(geometry, config, true); break;
      case LEAST_SQUARES:
      case WEIGHTED_LEAST_SQUARES:
        SetPrimitive_Gradient_LS(geometry, config, true); break;
      default: break;
    }
  }

  /*--- Compute gradient of the primitive variables ---*/

  if (config->GetKind_Gradient_Method() == GREEN_GAUSS) {
    SetPrimitive_Gradient_GG(geometry, config);
  }
  else if (config->GetKind_Gradient_Method() == WEIGHTED_LEAST_SQUARES) {
    SetPrimitive_Gradient_LS(geometry, config);
  }

  /*--- Compute the limiters ---*/

  if (muscl && !center && limiter && !van_albada && !Output) {
    SetPrimitive_Limiter(geometry, config);
  }

  ComputeVorticityAndStrainMag<1>(*config, iMesh);

  /*--- Compute recovered pressure and temperature for streamwise periodic flow ---*/
  if (config->GetKind_Streamwise_Periodic() != NONE) {

    /*--- Define and initialize helping variables ---*/
    su2double dot_product,
              Pressure_Recovered,
              Temperature_Recovered;

    su2double delta_p  = config->GetStreamwise_Periodic_PressureDrop() / config->GetPressure_Ref();

    /*--- Reference node on inlet periodic marker to compute relative distance along periodic translation vector. ---*/
    const su2double* ReferenceNode = geometry->GetStreamwise_Periodic_RefNode();

    /*--- Compute square of the distance between the 2 periodic surfaces. ---*/
    su2double norm2_translation = GeometryToolbox::SquaredNorm(nDim, config->GetPeriodic_Translation(0));

    /*--- Compute recoverd pressure and temperature for all points ---*/
    for (auto iPoint = 0ul; iPoint < nPoint; iPoint++) {

      /*--- First, compute helping terms based on relative distance (0,l) between periodic markers ---*/
      dot_product = 0.0;
      for (unsigned short iDim = 0; iDim < nDim; iDim++)
        dot_product += fabs( (geometry->nodes->GetCoord(iPoint,iDim) - ReferenceNode[iDim]) * config->GetPeriodic_Translation(0)[iDim]);

      /*--- Second, substract/add correction from reduced pressure/temperature to get recoverd pressure/temperature, TK:: added non-dimensionalization here - pres_ref=1 - how is pres_ref set? ---*/
      Pressure_Recovered = nodes->GetSolution(iPoint, 0) - delta_p / norm2_translation * dot_product;
      nodes->SetStreamwise_Periodic_RecoveredPressure(iPoint, Pressure_Recovered);

      /*--- 'InnerIter > 0' as otherwise MassFlow in the denominator would be zero ---*/
      if (energy && InnerIter > 0) {
        Temperature_Recovered  = nodes->GetSolution(iPoint, nDim+1);
        Temperature_Recovered += Streamwise_Periodic_IntegratedHeatFlow / (Streamwise_Periodic_MassFlow * nodes->GetSpecificHeatCp(iPoint) * norm2_translation) * dot_product;
        nodes->SetStreamwise_Periodic_RecoveredTemperature(iPoint, Temperature_Recovered);
      }
    }

    /*--- Compute the integrated Heatflux Q into the domain, and massflow over periodic markers ---*/
    GetStreamwise_Periodic_Properties(geometry, config, iMesh);
  } // if streamwise periodic  
}

void CIncNSSolver::Viscous_Residual(unsigned long iEdge, CGeometry *geometry, CSolver **solver_container,
                                    CNumerics *numerics, CConfig *config) {

  Viscous_Residual_impl(iEdge, geometry, solver_container, numerics, config);
}

unsigned long CIncNSSolver::SetPrimitive_Variables(CSolver **solver_container, const CConfig *config) {

  unsigned long iPoint, nonPhysicalPoints = 0;
  su2double eddy_visc = 0.0, turb_ke = 0.0, DES_LengthScale = 0.0;
  unsigned short turb_model = config->GetKind_Turb_Model();

  bool tkeNeeded = ((turb_model == SST) || (turb_model == SST_SUST));

  SU2_OMP_FOR_STAT(omp_chunk_size)
  for (iPoint = 0; iPoint < nPoint; iPoint++) {

    /*--- Retrieve the value of the kinetic energy (if needed) ---*/

    if (turb_model != NONE && solver_container[TURB_SOL] != nullptr) {
      eddy_visc = solver_container[TURB_SOL]->GetNodes()->GetmuT(iPoint);
      if (tkeNeeded) turb_ke = solver_container[TURB_SOL]->GetNodes()->GetSolution(iPoint,0);

      if (config->GetKind_HybridRANSLES() != NO_HYBRIDRANSLES){
        DES_LengthScale = solver_container[TURB_SOL]->GetNodes()->GetDES_LengthScale(iPoint);
      }
    }

    /*--- Incompressible flow, primitive variables --- */

    bool physical = static_cast<CIncNSVariable*>(nodes)->SetPrimVar(iPoint,eddy_visc, turb_ke, GetFluidModel());

    /* Check for non-realizable states for reporting. */

    if (!physical) nonPhysicalPoints++;

    /*--- Set the DES length scale ---*/

    nodes->SetDES_LengthScale(iPoint,DES_LengthScale);

  }

  return nonPhysicalPoints;

}

void CIncNSSolver::BC_Wall_Generic(const CGeometry *geometry, const CConfig *config,
                                   unsigned short val_marker, unsigned short kind_boundary) {

  const bool implicit = (config->GetKind_TimeIntScheme() == EULER_IMPLICIT);
  const bool energy = config->GetEnergy_Equation();

  /*--- Variable allocation for streamwise periodicity ---*/
  bool streamwise_periodic = (config->GetKind_Streamwise_Periodic() != NONE);
  bool streamwise_periodic_temperature = config->GetStreamwise_Periodic_Temperature();
  su2double Cp,
            thermal_conductivity,
            dot_product,
            scalar_factor;


  /*--- Identify the boundary by string name ---*/

  const auto Marker_Tag = config->GetMarker_All_TagBound(val_marker);

  /*--- Get the specified wall heat flux or temperature from config ---*/

  su2double Wall_HeatFlux = 0.0, Twall = 0.0;

  if (kind_boundary == HEAT_FLUX)
    Wall_HeatFlux = config->GetWall_HeatFlux(Marker_Tag)/config->GetHeat_Flux_Ref();
  else if (kind_boundary == ISOTHERMAL)
    Twall = config->GetIsothermal_Temperature(Marker_Tag)/config->GetTemperature_Ref();
  else
    SU2_MPI::Error("Unknown type of boundary condition", CURRENT_FUNCTION);

  /*--- Get wall function treatment from config. ---*/

  const auto Wall_Function = config->GetWallFunction_Treatment(Marker_Tag);
  if (Wall_Function != NO_WALL_FUNCTION)
    SU2_MPI::Error("Wall function treament not implemented yet", CURRENT_FUNCTION);

  /*--- Loop over all of the vertices on this boundary marker ---*/

  SU2_OMP_FOR_DYN(OMP_MIN_SIZE)
  for (auto iVertex = 0ul; iVertex < geometry->nVertex[val_marker]; iVertex++) {
    const auto iPoint = geometry->vertex[val_marker][iVertex]->GetNode();

    /*--- Check if the node belongs to the domain (i.e, not a halo node) ---*/

    if (!geometry->nodes->GetDomain(iPoint)) continue;

    /*--- Compute dual-grid area and boundary normal ---*/

    const auto Normal = geometry->vertex[val_marker][iVertex]->GetNormal();

    const su2double Area = GeometryToolbox::Norm(nDim, Normal);

    /*--- Impose the value of the velocity as a strong boundary
     condition (Dirichlet). Fix the velocity and remove any
     contribution to the residual at this node. ---*/

    if (dynamic_grid) {
      nodes->SetVelocity_Old(iPoint, geometry->nodes->GetGridVel(iPoint));
    } else {
      su2double zero[MAXNDIM] = {0.0};
      nodes->SetVelocity_Old(iPoint, zero);
    }

    for (unsigned short iDim = 0; iDim < nDim; iDim++)
      LinSysRes(iPoint, iDim+1) = 0.0;
    nodes->SetVel_ResTruncError_Zero(iPoint);

    /*--- Enforce the no-slip boundary condition in a strong way by
     modifying the velocity-rows of the Jacobian (1 on the diagonal). ---*/

    if (implicit) {
      for (unsigned short iVar = 1; iVar <= nDim; iVar++)
        Jacobian.DeleteValsRowi(iPoint*nVar+iVar);
    }

    if (!energy) continue;

    if (kind_boundary == HEAT_FLUX) {

      /*--- Apply a weak boundary condition for the energy equation.
      Compute the residual due to the prescribed heat flux. ---*/

      LinSysRes(iPoint, nDim+1) -= Wall_HeatFlux*Area;

      /*--- With streamwise periodic flow and heatflux walls an additional
              term is introduced in the boundary formulation ---*/
        if (streamwise_periodic && streamwise_periodic_temperature) {

          Cp = nodes->GetSpecificHeatCp(iPoint);
          thermal_conductivity = nodes->GetThermalConductivity(iPoint);

          /*--- Scalar factor of the residual contribution ---*/
          const su2double norm2_translation = GeometryToolbox::SquaredNorm(nDim, config->GetPeriodic_Translation(0));
          scalar_factor = Streamwise_Periodic_IntegratedHeatFlow*thermal_conductivity / (Streamwise_Periodic_MassFlow * Cp * norm2_translation);

          /*--- Dot product ---*/
          dot_product = GeometryToolbox::DotProduct(nDim, config->GetPeriodic_Translation(0), Normal);

          LinSysRes(iPoint, nDim+1) += scalar_factor*dot_product;
        } // if streamwise_periodic
    }
    else { // ISOTHERMAL

      auto Point_Normal = geometry->vertex[val_marker][iVertex]->GetNormal_Neighbor();

      /*--- Get coordinates of i & nearest normal and compute distance ---*/

      auto Coord_i = geometry->nodes->GetCoord(iPoint);
      auto Coord_j = geometry->nodes->GetCoord(Point_Normal);
      su2double Edge_Vector[MAXNDIM];
      GeometryToolbox::Distance(nDim, Coord_j, Coord_i, Edge_Vector);
      su2double dist_ij_2 = GeometryToolbox::SquaredNorm(nDim, Edge_Vector);
      su2double dist_ij = sqrt(dist_ij_2);

      /*--- Compute the normal gradient in temperature using Twall ---*/

      su2double dTdn = -(nodes->GetTemperature(Point_Normal) - Twall)/dist_ij;

      /*--- Get thermal conductivity ---*/

      su2double thermal_conductivity = nodes->GetThermalConductivity(iPoint);

      /*--- Apply a weak boundary condition for the energy equation.
      Compute the residual due to the prescribed heat flux. ---*/

      LinSysRes(iPoint, nDim+1) -= thermal_conductivity*dTdn*Area;

      /*--- Jacobian contribution for temperature equation. ---*/

      if (implicit) {
        su2double proj_vector_ij = 0.0;
        if (dist_ij_2 > 0.0)
          proj_vector_ij = GeometryToolbox::DotProduct(nDim, Edge_Vector, Normal) / dist_ij_2;
        Jacobian.AddVal2Diag(iPoint, nDim+1, thermal_conductivity*proj_vector_ij);
      }
    }
  }
}

void CIncNSSolver::BC_HeatFlux_Wall(CGeometry *geometry, CSolver**, CNumerics*,
                                    CNumerics*, CConfig *config, unsigned short val_marker) {

  BC_Wall_Generic(geometry, config, val_marker, HEAT_FLUX);
}

void CIncNSSolver::BC_Isothermal_Wall(CGeometry *geometry, CSolver**, CNumerics*,
                                    CNumerics*, CConfig *config, unsigned short val_marker) {

  BC_Wall_Generic(geometry, config, val_marker, ISOTHERMAL);
}

void CIncNSSolver::BC_ConjugateHeat_Interface(CGeometry *geometry, CSolver **solver_container, CNumerics *conv_numerics,
                                              CConfig *config, unsigned short val_marker) {

  const su2double Temperature_Ref = config->GetTemperature_Ref();
  const bool implicit = (config->GetKind_TimeIntScheme() == EULER_IMPLICIT);
  const bool energy = config->GetEnergy_Equation();

  /*--- Identify the boundary ---*/

  const auto Marker_Tag = config->GetMarker_All_TagBound(val_marker);

  /*--- Retrieve the specified wall function treatment.---*/

  const auto Wall_Function = config->GetWallFunction_Treatment(Marker_Tag);
  if (Wall_Function != NO_WALL_FUNCTION) {
    SU2_MPI::Error("Wall function treament not implemented yet", CURRENT_FUNCTION);
  }

  /*--- Loop over boundary points ---*/

  SU2_OMP_FOR_DYN(OMP_MIN_SIZE)
  for (auto iVertex = 0ul; iVertex < geometry->nVertex[val_marker]; iVertex++) {

    auto iPoint = geometry->vertex[val_marker][iVertex]->GetNode();

    if (!geometry->nodes->GetDomain(iPoint)) continue;

    /*--- Impose the value of the velocity as a strong boundary
     condition (Dirichlet). Fix the velocity and remove any
     contribution to the residual at this node. ---*/

    if (dynamic_grid) {
      nodes->SetVelocity_Old(iPoint, geometry->nodes->GetGridVel(iPoint));
    } else {
      su2double zero[MAXNDIM] = {0.0};
      nodes->SetVelocity_Old(iPoint, zero);
    }

    for (unsigned short iDim = 0; iDim < nDim; iDim++)
      LinSysRes(iPoint, iDim+1) = 0.0;
    nodes->SetVel_ResTruncError_Zero(iPoint);

    /*--- Enforce the no-slip boundary condition in a strong way by
     modifying the velocity-rows of the Jacobian (1 on the diagonal). ---*/

    if (implicit) {
      for (unsigned short iVar = 1; iVar <= nDim; iVar++)
        Jacobian.DeleteValsRowi(iPoint*nVar+iVar);
      if (energy) Jacobian.DeleteValsRowi(iPoint*nVar+nDim+1);
    }

    if (!energy) continue;

    su2double Tconjugate = GetConjugateHeatVariable(val_marker, iVertex, 0) / Temperature_Ref;
    su2double Twall = 0.0;

    if ((config->GetKind_CHT_Coupling() == AVERAGED_TEMPERATURE_NEUMANN_HEATFLUX) ||
        (config->GetKind_CHT_Coupling() == AVERAGED_TEMPERATURE_ROBIN_HEATFLUX)) {

      /*--- Compute closest normal neighbor ---*/

      auto Point_Normal = geometry->vertex[val_marker][iVertex]->GetNormal_Neighbor();

      /*--- Get coordinates of i & nearest normal and compute distance ---*/

      auto Coord_i = geometry->nodes->GetCoord(iPoint);
      auto Coord_j = geometry->nodes->GetCoord(Point_Normal);
      su2double dist_ij = GeometryToolbox::Distance(nDim, Coord_j, Coord_i);

      /*--- Compute wall temperature from both temperatures ---*/

      su2double thermal_conductivity = nodes->GetThermalConductivity(iPoint);
      su2double There = nodes->GetTemperature(Point_Normal);
      su2double HF_FactorHere = thermal_conductivity*config->GetViscosity_Ref()/dist_ij;
      su2double HF_FactorConjugate = GetConjugateHeatVariable(val_marker, iVertex, 2);

      Twall = (There*HF_FactorHere + Tconjugate*HF_FactorConjugate)/(HF_FactorHere + HF_FactorConjugate);
    }
    else if ((config->GetKind_CHT_Coupling() == DIRECT_TEMPERATURE_NEUMANN_HEATFLUX) ||
             (config->GetKind_CHT_Coupling() == DIRECT_TEMPERATURE_ROBIN_HEATFLUX)) {

      /*--- (Directly) Set wall temperature to conjugate temperature. ---*/

      Twall = Tconjugate;
    }
    else {
      SU2_MPI::Error("Unknown CHT coupling method.", CURRENT_FUNCTION);
    }

    /*--- Strong imposition of the temperature on the fluid zone. ---*/

    LinSysRes(iPoint, nDim+1) = 0.0;
    nodes->SetSolution_Old(iPoint, nDim+1, Twall);
    nodes->SetEnergy_ResTruncError_Zero(iPoint);
  }
}
