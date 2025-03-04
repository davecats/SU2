/*!
 * \file CTurbSolver.cpp
 * \brief Main subrotuines of CTurbSolver class
 * \author F. Palacios, A. Bueno
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

#include "../../include/solvers/CTurbSolver.hpp"
#include "../../../Common/include/parallelization/omp_structure.hpp"
#include "../../../Common/include/toolboxes/geometry_toolbox.hpp"
#include "../../include/solvers/CScalarSolver.inl"

/*--- Explicit instantiation of the parent class of CTurbSolver. ---*/
template class CScalarSolver<CTurbVariable>;

CTurbSolver::CTurbSolver(bool conservative) : CScalarSolver<CTurbVariable>(conservative) { }

CTurbSolver::CTurbSolver(CGeometry* geometry, CConfig *config, bool conservative) : CScalarSolver<CTurbVariable>(geometry, config, conservative) { }

CTurbSolver::~CTurbSolver() {

  for (auto& mat : SlidingState) {
    for (auto ptr : mat) delete [] ptr;
  }

}

void CTurbSolver::BC_Riemann(CGeometry *geometry, CSolver **solver_container, CNumerics *conv_numerics, CNumerics *visc_numerics, CConfig *config, unsigned short val_marker) {

  string Marker_Tag         = config->GetMarker_All_TagBound(val_marker);

  switch(config->GetKind_Data_Riemann(Marker_Tag))
  {
  case TOTAL_CONDITIONS_PT: case STATIC_SUPERSONIC_INFLOW_PT: case STATIC_SUPERSONIC_INFLOW_PD: case DENSITY_VELOCITY:
    BC_Inlet(geometry, solver_container, conv_numerics, visc_numerics, config, val_marker);
    break;
  case STATIC_PRESSURE:
    BC_Outlet(geometry, solver_container, conv_numerics, visc_numerics, config, val_marker);
    break;
  }
}

void CTurbSolver::BC_TurboRiemann(CGeometry *geometry, CSolver **solver_container, CNumerics *conv_numerics, CNumerics *visc_numerics, CConfig *config, unsigned short val_marker) {

  string Marker_Tag         = config->GetMarker_All_TagBound(val_marker);

  switch(config->GetKind_Data_Riemann(Marker_Tag))
  {
  case TOTAL_CONDITIONS_PT: case STATIC_SUPERSONIC_INFLOW_PT: case STATIC_SUPERSONIC_INFLOW_PD: case DENSITY_VELOCITY:
    BC_Inlet_Turbo(geometry, solver_container, conv_numerics, visc_numerics, config, val_marker);
    break;
  case STATIC_PRESSURE:
    BC_Outlet(geometry, solver_container, conv_numerics, visc_numerics, config, val_marker);
    break;
  }
}


void CTurbSolver::BC_Giles(CGeometry *geometry, CSolver **solver_container, CNumerics *conv_numerics, CNumerics *visc_numerics, CConfig *config, unsigned short val_marker) {

  string Marker_Tag         = config->GetMarker_All_TagBound(val_marker);

  switch(config->GetKind_Data_Giles(Marker_Tag))
  {
  case TOTAL_CONDITIONS_PT:case TOTAL_CONDITIONS_PT_1D: case DENSITY_VELOCITY:
    BC_Inlet_Turbo(geometry, solver_container, conv_numerics, visc_numerics, config, val_marker);
    break;
  case MIXING_IN:
    if (config->GetBoolTurbMixingPlane()){
      BC_Inlet_MixingPlane(geometry, solver_container, conv_numerics, visc_numerics, config, val_marker);
    }
    else{
      BC_Inlet_Turbo(geometry, solver_container, conv_numerics, visc_numerics, config, val_marker);
    }
    break;

  case STATIC_PRESSURE: case MIXING_OUT: case STATIC_PRESSURE_1D: case RADIAL_EQUILIBRIUM:
    BC_Outlet(geometry, solver_container, conv_numerics, visc_numerics, config, val_marker);
    break;
  }
}

void CTurbSolver::BC_Fluid_Interface(CGeometry *geometry, CSolver **solver_container, CNumerics *conv_numerics,
                                     CNumerics *visc_numerics, CConfig *config) {

  const bool sst = (config->GetKind_Turb_Model() == TURB_MODEL::SST) || (config->GetKind_Turb_Model() == TURB_MODEL::SST_SUST);
  const auto nPrimVar = solver_container[FLOW_SOL]->GetnPrimVar();
  su2double *PrimVar_j = new su2double[nPrimVar];
  su2double solution_j[MAXNVAR] = {0.0};

  for (auto iMarker = 0u; iMarker < config->GetnMarker_All(); iMarker++) {

    if (config->GetMarker_All_KindBC(iMarker) != FLUID_INTERFACE) continue;

    SU2_OMP_FOR_STAT(OMP_MIN_SIZE)
    for (auto iVertex = 0u; iVertex < geometry->nVertex[iMarker]; iVertex++) {

      const auto iPoint = geometry->vertex[iMarker][iVertex]->GetNode();

      if (!geometry->nodes->GetDomain(iPoint)) continue;

      const auto Point_Normal = geometry->vertex[iMarker][iVertex]->GetNormal_Neighbor();
      const auto nDonorVertex = GetnSlidingStates(iMarker,iVertex);

      su2double Normal[MAXNDIM] = {0.0};
      for (auto iDim = 0u; iDim < nDim; iDim++)
        Normal[iDim] = -geometry->vertex[iMarker][iVertex]->GetNormal()[iDim];

      su2double* PrimVar_i = solver_container[FLOW_SOL]->GetNodes()->GetPrimitive(iPoint);

      auto Jacobian_i = Jacobian.GetBlock(iPoint,iPoint);

      /*--- Loop over the nDonorVertexes and compute the averaged flux ---*/

      for (auto jVertex = 0; jVertex < nDonorVertex; jVertex++) {

        for (auto iVar = 0u; iVar < nPrimVar; iVar++)
          PrimVar_j[iVar] = solver_container[FLOW_SOL]->GetSlidingState(iMarker, iVertex, iVar, jVertex);

        /*--- Get the weight computed in the interpolator class for the j-th donor vertex ---*/

        const su2double weight = solver_container[FLOW_SOL]->GetSlidingState(iMarker, iVertex, nPrimVar, jVertex);

        /*--- Set primitive variables ---*/

        conv_numerics->SetPrimitive( PrimVar_i, PrimVar_j );

        /*--- Set the turbulent variable states ---*/

        for (auto iVar = 0u; iVar < nVar; ++iVar)
          solution_j[iVar] = GetSlidingState(iMarker, iVertex, iVar, jVertex);

        conv_numerics->SetScalarVar(nodes->GetSolution(iPoint), solution_j);

        /*--- Set the normal vector ---*/

        conv_numerics->SetNormal(Normal);

        if (dynamic_grid)
          conv_numerics->SetGridVel(geometry->nodes->GetGridVel(iPoint), geometry->nodes->GetGridVel(iPoint));

        auto residual = conv_numerics->ComputeResidual(config);

        /*--- Accumulate the residuals to compute the average ---*/

        for (auto iVar = 0u; iVar < nVar; iVar++) {
          LinSysRes(iPoint,iVar) += weight*residual[iVar];
          for (auto jVar = 0u; jVar < nVar; jVar++)
            Jacobian_i[iVar*nVar+jVar] += SU2_TYPE::GetValue(weight*residual.jacobian_i[iVar][jVar]);
        }
      }

      /*--- Set the normal vector and the coordinates ---*/

      visc_numerics->SetNormal(Normal);
      su2double Coord_Reflected[MAXNDIM];
      GeometryToolbox::PointPointReflect(nDim, geometry->nodes->GetCoord(Point_Normal),
                                               geometry->nodes->GetCoord(iPoint), Coord_Reflected);
      visc_numerics->SetCoord(geometry->nodes->GetCoord(iPoint), Coord_Reflected);

      /*--- Primitive variables ---*/

      visc_numerics->SetPrimitive(PrimVar_i, PrimVar_j);

      /*--- Turbulent variables and their gradients ---*/

      visc_numerics->SetScalarVar(nodes->GetSolution(iPoint), solution_j);
      visc_numerics->SetScalarVarGradient(nodes->GetGradient(iPoint), nodes->GetGradient(iPoint));

      /*--- Menter's first blending function ---*/

      if(sst) visc_numerics->SetF1blending(nodes->GetF1blending(iPoint), nodes->GetF1blending(iPoint));

      /*--- Compute and update residual ---*/

      auto residual = visc_numerics->ComputeResidual(config);

      LinSysRes.SubtractBlock(iPoint, residual);

      /*--- Jacobian contribution for implicit integration ---*/

      Jacobian.SubtractBlock2Diag(iPoint, residual.jacobian_i);

    }
    END_SU2_OMP_FOR
  }

  delete [] PrimVar_j;

}

void CTurbSolver::LoadRestart(CGeometry** geometry, CSolver*** solver, CConfig* config, int val_iter,
                                           bool val_update_geo) {
  /*--- Restart the solution from file information ---*/

  unsigned short iVar, iMesh;
  unsigned long iPoint, index, iChildren, Point_Fine;
  su2double Area_Children, Area_Parent;
  const su2double* Solution_Fine = nullptr;

  string restart_filename = config->GetFilename(config->GetSolution_FileName(), "", val_iter);

  /*--- To make this routine safe to call in parallel most of it can only be executed by one thread. ---*/
  SU2_OMP_MASTER {
    /*--- Read the restart data from either an ASCII or binary SU2 file. ---*/

    if (config->GetRead_Binary_Restart()) {
      Read_SU2_Restart_Binary(geometry[MESH_0], config, restart_filename);
    } else {
      Read_SU2_Restart_ASCII(geometry[MESH_0], config, restart_filename);
    }

    /*--- Skip flow variables ---*/

    unsigned short skipVars = nDim + solver[MESH_0][FLOW_SOL]->GetnVar();

    /*--- Adjust the number of solution variables in the incompressible
     restart. We always carry a space in nVar for the energy equation in the
     mean flow solver, but we only write it to the restart if it is active.
     Therefore, we must reduce skipVars here if energy is inactive so that
     the turbulent variables are read correctly. ---*/

    bool incompressible = (config->GetKind_Regime() == ENUM_REGIME::INCOMPRESSIBLE);
    bool energy = config->GetEnergy_Equation();
    bool weakly_coupled_heat = config->GetWeakly_Coupled_Heat();

    if (incompressible && ((!energy) && (!weakly_coupled_heat))) skipVars--;

    /*--- Load data from the restart into correct containers. ---*/

    unsigned long counter = 0, iPoint_Global = 0;
    for (; iPoint_Global < geometry[MESH_0]->GetGlobal_nPointDomain(); iPoint_Global++) {
      /*--- Retrieve local index. If this node from the restart file lives
       on the current processor, we will load and instantiate the vars. ---*/

      auto iPoint_Local = geometry[MESH_0]->GetGlobal_to_Local_Point(iPoint_Global);

      if (iPoint_Local > -1) {
        /*--- We need to store this point's data, so jump to the correct
         offset in the buffer of data from the restart file and load it. ---*/

        index = counter * Restart_Vars[1] + skipVars;
        for (iVar = 0; iVar < nVar; ++iVar) nodes->SetSolution(iPoint_Local, iVar, Restart_Data[index + iVar]);

        /*--- Increment the overall counter for how many points have been loaded. ---*/
        counter++;
      }
    }

    /*--- Detect a wrong solution file ---*/

    if (counter != nPointDomain) {
      SU2_MPI::Error(string("The solution file ") + restart_filename + string(" doesn't match with the mesh file!\n") +
                         string("It could be empty lines at the end of the file."),
                     CURRENT_FUNCTION);
    }

  }  // end SU2_OMP_MASTER, pre and postprocessing are thread-safe.
  END_SU2_OMP_MASTER
  SU2_OMP_BARRIER

  /*--- MPI solution and compute the eddy viscosity ---*/

  solver[MESH_0][TURB_SOL]->InitiateComms(geometry[MESH_0], config, SOLUTION);
  solver[MESH_0][TURB_SOL]->CompleteComms(geometry[MESH_0], config, SOLUTION);

  solver[MESH_0][FLOW_SOL]->Preprocessing(geometry[MESH_0], solver[MESH_0], config, MESH_0, NO_RK_ITER,
                                          RUNTIME_FLOW_SYS, false);
  solver[MESH_0][TURB_SOL]->Postprocessing(geometry[MESH_0], solver[MESH_0], config, MESH_0);

  /*--- Interpolate the solution down to the coarse multigrid levels ---*/

  for (iMesh = 1; iMesh <= config->GetnMGLevels(); iMesh++) {
    SU2_OMP_FOR_STAT(omp_chunk_size)
    for (iPoint = 0; iPoint < geometry[iMesh]->GetnPoint(); iPoint++) {
      Area_Parent = geometry[iMesh]->nodes->GetVolume(iPoint);
      su2double Solution_Coarse[MAXNVAR] = {0.0};
      for (iChildren = 0; iChildren < geometry[iMesh]->nodes->GetnChildren_CV(iPoint); iChildren++) {
        Point_Fine = geometry[iMesh]->nodes->GetChildren_CV(iPoint, iChildren);
        Area_Children = geometry[iMesh - 1]->nodes->GetVolume(Point_Fine);
        Solution_Fine = solver[iMesh - 1][TURB_SOL]->GetNodes()->GetSolution(Point_Fine);
        for (iVar = 0; iVar < nVar; iVar++) {
          Solution_Coarse[iVar] += Solution_Fine[iVar] * Area_Children / Area_Parent;
        }
      }
      solver[iMesh][TURB_SOL]->GetNodes()->SetSolution(iPoint, Solution_Coarse);
    }
    END_SU2_OMP_FOR

    solver[iMesh][TURB_SOL]->InitiateComms(geometry[iMesh], config, SOLUTION);
    solver[iMesh][TURB_SOL]->CompleteComms(geometry[iMesh], config, SOLUTION);

    solver[iMesh][FLOW_SOL]->Preprocessing(geometry[iMesh], solver[iMesh], config, iMesh, NO_RK_ITER, RUNTIME_FLOW_SYS,
                                           false);
    solver[iMesh][TURB_SOL]->Postprocessing(geometry[iMesh], solver[iMesh], config, iMesh);
  }

  /*--- Go back to single threaded execution. ---*/
  SU2_OMP_MASTER {
    /*--- Delete the class memory that is used to load the restart. ---*/

    delete[] Restart_Vars;
    Restart_Vars = nullptr;
    delete[] Restart_Data;
    Restart_Data = nullptr;
  }
  END_SU2_OMP_MASTER
  SU2_OMP_BARRIER
}

void CTurbSolver::Impose_Fixed_Values(const CGeometry *geometry, const CConfig *config){

  /*  /*--- Far-field flow state quantities and initialization. ---*/
  su2double rhoInf, muLamInf, Intensity, viscRatio, muT_Inf;

  rhoInf    = config->GetDensity_FreeStreamND();
  const su2double* VeloInf = config->GetVelocity_FreeStreamND();
  muLamInf  = config->GetViscosity_FreeStreamND();
  Intensity = config->GetTurbulenceIntensity_FreeStream();
  viscRatio = config->GetTurb2LamViscRatio_FreeStream();

  su2double VelMag2 = GeometryToolbox::SquaredNorm(nDim, VeloInf);

  su2double kine_Inf  = 3.0/2.0*(VelMag2*Intensity*Intensity);
  su2double omega_Inf = rhoInf*kine_Inf/(muLamInf*viscRatio);

  Solution_Inf[0] = kine_Inf;
  Solution_Inf[1] = omega_Inf;

  if(config->GetKind_Trans_Model() == TS){
    const bool implicit = (config->GetKind_TimeIntScheme() == EULER_IMPLICIT);

    SU2_OMP_FOR_DYN(omp_chunk_size)
    for (unsigned long iPoint = 0; iPoint < nPointDomain; iPoint++) {
      
      su2double *Coord = geometry->nodes->GetCoord(iPoint);
      su2double trans_pos_x = config->GetTransTS_Param(0);
      if(Coord[0] < (trans_pos_x-0.01)) {
        //Solution_Inf[0] = 1e-10;  
        //Solution_Inf[1] = omega_Inf; 
        
        nodes->SetSolution_Old(iPoint, Solution_Inf);
        nodes->SetSolution(iPoint, Solution_Inf);
        LinSysRes.SetBlock_Zero(iPoint);
        if (implicit) {
          /*--- Change rows of the Jacobian (includes 1 in the diagonal) ---*/
          for(unsigned long iVar=0; iVar<nVar; iVar++)
            Jacobian.DeleteValsRowi(iPoint*nVar+iVar);
        }

        /*--- Set the solution values and zero the residual ---*/
//        nodes->SetSolution_Old(iPoint, 0, Solution_Inf[0]);
//        nodes->SetSolution(iPoint, 0, Solution_Inf[0]);
        //cout << "Solution_0: " << Solution_Inf[0] << endl;        
        //cout << "Solution_1: " << Solution_Inf[1] << endl;

//        for (unsigned short iDim = 0; iDim < nDim; iDim++)
//          LinSysRes(iPoint, iDim+1) = 0.0;
        //LinSysRes(iPoint, 0) = 0.0;
        //LinSysRes.SetBlock_Zero(iPoint);
        //if (implicit) {
        //  for (unsigned long iVar = 0; iVar <= nVar; iVar++) {
            //total_index = iPoint*nVar+iVar;
        //    Jacobian.DeleteValsRowi(iPoint*nVar+iVar);
        //Jacobian.DeleteValsRowi(iPoint);
        //  }
       // }

        //LinSysRes.SetBlock_Zero(iPoint);
//        if (implicit) {
         /*--- Change rows of the Jacobian (includes 1 in the diagonal) ---*/
//          for(unsigned long iVar=0; iVar<nVar; iVar++)
//            Jacobian.DeleteValsRowi(iPoint*nVar+iVar);
//        } 
      }   
    } 
    END_SU2_OMP_FOR   
  }
    
   
  
  /*--- Check whether turbulence quantities are fixed to far-field values on a half-plane. ---*/
  if(config->GetTurb_Fixed_Values()){

    const bool implicit = (config->GetKind_TimeIntScheme() == EULER_IMPLICIT);

    /*--- Form normalized far-field velocity ---*/
    const su2double* velocity_inf = config->GetVelocity_FreeStreamND();
    su2double velmag_inf = GeometryToolbox::Norm(nDim, velocity_inf);
    if(velmag_inf==0)
      SU2_MPI::Error("Far-field velocity is zero, cannot fix turbulence quantities to inflow values.", CURRENT_FUNCTION);
    su2double unit_velocity_inf[MAXNDIM];
    for(unsigned short iDim=0; iDim<nDim; iDim++)
      unit_velocity_inf[iDim] = velocity_inf[iDim] / velmag_inf;

    SU2_OMP_FOR_DYN(omp_chunk_size)
    for (unsigned long iPoint = 0; iPoint < nPointDomain; iPoint++) {
      if( GeometryToolbox::DotProduct(nDim, geometry->nodes->GetCoord(iPoint), unit_velocity_inf)
        < config->GetTurb_Fixed_Values_MaxScalarProd() ) {
        /*--- Set the solution values and zero the residual ---*/
        nodes->SetSolution_Old(iPoint, Solution_Inf);
        nodes->SetSolution(iPoint, Solution_Inf);
        LinSysRes.SetBlock_Zero(iPoint);
        if (implicit) {
          /*--- Change rows of the Jacobian (includes 1 in the diagonal) ---*/
          for(unsigned long iVar=0; iVar<nVar; iVar++)
            Jacobian.DeleteValsRowi(iPoint*nVar+iVar);
        }
      }
    }
    END_SU2_OMP_FOR
  }

}
