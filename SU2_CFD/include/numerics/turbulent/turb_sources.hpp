﻿/*!
 * \file turb_sources.hpp
 * \brief Delarations of numerics classes for integration of source
 *        terms in turbulence problems.
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

#pragma once

#include "../scalar/scalar_sources.hpp"

/*!
 * \class CSourcePieceWise_TurbSA
 * \brief Class for integrating the source terms of the Spalart-Allmaras turbulence model equation.
 * \ingroup SourceDiscr
 * \author A. Bueno.
 */
class CSourceBase_TurbSA : public CNumerics {
protected:
  su2double cv1_3;
  su2double k2;
  su2double cb1;
  su2double cw2;
  su2double ct3;
  su2double ct4;
  su2double cw3_6;
  su2double cb2_sigma;
  su2double sigma;
  su2double cb2;
  su2double cw1;
  su2double cr1;

  su2double Gamma_BC = 0.0;
  su2double intermittency;
  su2double Production, Destruction, CrossProduction;

  su2double Residual, *Jacobian_i;
private:
  su2double Jacobian_Buffer; /// Static storage for the Jacobian (which needs to be pointer for return type).

protected:
  const bool incompressible = false, rotating_frame = false;
  bool roughwall = false;

public:
  /*!
   * \brief Constructor of the class.
   * \param[in] val_nDim - Number of dimensions of the problem.
   * \param[in] val_nVar - Number of variables of the problem.
   * \param[in] config - Definition of the particular problem.
   */
  CSourceBase_TurbSA(unsigned short val_nDim, unsigned short val_nVar, const CConfig* config);

  /*!
   * \brief Residual for source term integration.
   * \param[in] intermittency_in - Value of the intermittency.
   */
  inline void SetIntermittency(su2double intermittency_in) final { intermittency = intermittency_in; }

  /*!
   * \brief Residual for source term integration.
   * \param[in] val_production - Value of the Production.
   */
  inline void SetProduction(su2double val_production) final { Production = val_production; }

  /*!
   * \brief Residual for source term integration.
   * \param[in] val_destruction - Value of the Destruction.
   */
  inline void SetDestruction(su2double val_destruction) final { Destruction = val_destruction; }

  /*!
   * \brief Residual for source term integration.
   * \param[in] val_crossproduction - Value of the CrossProduction.
   */
  inline void SetCrossProduction(su2double val_crossproduction) final { CrossProduction = val_crossproduction; }

  /*!
   * \brief ______________.
   */
  inline su2double GetProduction(void) const final { return Production; }

  /*!
   * \brief  Get the intermittency for the BC trans. model.
   * \return Value of the intermittency.
   */
  inline su2double GetGammaBC(void) const final { return Gamma_BC; }

  /*!
   * \brief  ______________.
   */
  inline su2double GetDestruction(void) const final { return Destruction; }

  /*!
   * \brief  ______________.
   */
  inline su2double GetCrossProduction(void) const final { return CrossProduction; }
};


/*!
 * \class CSourcePieceWise_TurbSA
 * \brief Class for integrating the source terms of the Spalart-Allmaras turbulence model equation.
 * \ingroup SourceDiscr
 * \author A. Bueno.
 */
class CSourcePieceWise_TurbSA final : public CSourceBase_TurbSA {
private:
  su2double nu, Ji, fv1, fv2, ft2, Omega, S, Shat, inv_Shat, dist_i_2, Ji_2, Ji_3, inv_k2_d2;
  su2double r, g, g_6, glim, fw;
  su2double norm2_Grad;
  su2double dfv1, dfv2, dShat;
  su2double dr, dg, dfw;
  unsigned short iDim;
  bool transition;
  bool axisymmetric;
  
public:
  /*!
   * \brief Constructor of the class.
   * \param[in] val_nDim - Number of dimensions of the problem.
   * \param[in] val_nVar - Number of variables of the problem.
   * \param[in] config - Definition of the particular problem.
   */
  CSourcePieceWise_TurbSA(unsigned short val_nDim, unsigned short val_nVar, const CConfig* config);

  /*!
   * \brief Residual for source term integration.
   * \param[in] config - Definition of the particular problem.
   * \return A lightweight const-view (read-only) of the residual/flux and Jacobians.
   */
  ResidualType<> ComputeResidual(const CConfig* config) override;

};

/*!
 * \class CSourcePieceWise_TurbSA_COMP
 * \brief Class for integrating the source terms of the Spalart-Allmaras CC modification turbulence model equation.
 * \ingroup SourceDiscr
 * \author E.Molina, A. Bueno.
 * \version 7.2.1 "Blackbird"
 */
class CSourcePieceWise_TurbSA_COMP final : public CSourceBase_TurbSA {
private:
  su2double nu, Ji, fv1, fv2, ft2, Omega, S, Shat, inv_Shat, dist_i_2, Ji_2, Ji_3, inv_k2_d2;
  su2double r, g, g_6, glim, fw;
  su2double norm2_Grad;
  su2double dfv1, dfv2, dShat;
  su2double dr, dg, dfw;
  su2double aux_cc, CompCorrection, c5;
  unsigned short iDim, jDim;

public:
  /*!
   * \brief Constructor of the class.
   * \param[in] val_nDim - Number of dimensions of the problem.
   * \param[in] val_nVar - Number of variables of the problem.
   * \param[in] config - Definition of the particular problem.
   */
  CSourcePieceWise_TurbSA_COMP(unsigned short val_nDim, unsigned short val_nVar, const CConfig* config);

  /*!
   * \brief Residual for source term integration.
   * \param[in] config - Definition of the particular problem.
   * \return A lightweight const-view (read-only) of the residual/flux and Jacobians.
   */
  ResidualType<> ComputeResidual(const CConfig* config) override;

};

/*!
 * \class CSourcePieceWise_TurbSA_E
 * \brief Class for integrating the source terms of the Spalart-Allmaras Edwards modification turbulence model equation.
 * \ingroup SourceDiscr
 * \author E.Molina, A. Bueno.
 * \version 7.2.1 "Blackbird"
 */
class CSourcePieceWise_TurbSA_E final : public CSourceBase_TurbSA {
private:
  su2double nu, Ji, fv1, fv2, ft2, Omega, S, Shat, inv_Shat, dist_i_2, Ji_2, Ji_3, inv_k2_d2;
  su2double r, g, g_6, glim, fw;
  su2double norm2_Grad;
  su2double dfv1, dfv2, dShat;
  su2double dr, dg, dfw;
  su2double Sbar;

public:
  /*!
   * \brief Constructor of the class.
   * \param[in] val_nDim - Number of dimensions of the problem.
   * \param[in] val_nVar - Number of variables of the problem.
   * \param[in] config - Definition of the particular problem.
   */
  CSourcePieceWise_TurbSA_E(unsigned short val_nDim, unsigned short val_nVar, const CConfig* config);

  /*!
   * \brief Residual for source term integration.
   * \param[in] config - Definition of the particular problem.
   * \return A lightweight const-view (read-only) of the residual/flux and Jacobians.
   */
  ResidualType<> ComputeResidual(const CConfig* config) override;
};

/*!
 * \class CSourcePieceWise_TurbSA_E_COMP
 * \brief Class for integrating the source terms of the Spalart-Allmaras Edwards modification with CC turbulence model equation.
 * \ingroup SourceDiscr
 * \author E.Molina, A. Bueno.
 * \version 7.2.1 "Blackbird"
 */
class CSourcePieceWise_TurbSA_E_COMP : public CSourceBase_TurbSA {
private:
  su2double nu, Ji, fv1, fv2, ft2, Omega, S, Shat, inv_Shat, dist_i_2, Ji_2, Ji_3, inv_k2_d2;
  su2double r, g, g_6, glim, fw;
  su2double norm2_Grad;
  su2double dfv1, dfv2, dShat;
  su2double dr, dg, dfw;
  su2double Sbar;
  su2double aux_cc, CompCorrection, c5;
  unsigned short jDim;

public:
  /*!
   * \brief Constructor of the class.
   * \param[in] val_nDim - Number of dimensions of the problem.
   * \param[in] val_nVar - Number of variables of the problem.
   * \param[in] config - Definition of the particular problem.
   */
  CSourcePieceWise_TurbSA_E_COMP(unsigned short val_nDim, unsigned short val_nVar, const CConfig* config);

  /*!
   * \brief Residual for source term integration.
   * \param[in] config - Definition of the particular problem.
   * \return A lightweight const-view (read-only) of the residual/flux and Jacobians.
   */
  ResidualType<> ComputeResidual(const CConfig* config) override;
};

/*!
 * \class CSourcePieceWise_TurbSA_Neg
 * \brief Class for integrating the source terms of the Spalart-Allmaras turbulence model equation.
 * \ingroup SourceDiscr
 * \author F. Palacios
 */
class CSourcePieceWise_TurbSA_Neg : public CSourceBase_TurbSA {
private:
  su2double nu, Ji, fv1, fv2, ft2, Omega, S, Shat, inv_Shat, dist_i_2, Ji_2, Ji_3, inv_k2_d2;
  su2double r, g, g_6, glim, fw;
  su2double norm2_Grad;
  su2double dfv1, dfv2, dShat;
  su2double dr, dg, dfw;

public:
  /*!
   * \brief Constructor of the class.
   * \param[in] val_nDim - Number of dimensions of the problem.
   * \param[in] val_nVar - Number of variables of the problem.
   * \param[in] config - Definition of the particular problem.
   */
  CSourcePieceWise_TurbSA_Neg(unsigned short val_nDim, unsigned short val_nVar, const CConfig* config);

  /*!
   * \brief Residual for source term integration.
   * \param[in] config - Definition of the particular problem.
   * \return A lightweight const-view (read-only) of the residual/flux and Jacobians.
   */
  ResidualType<> ComputeResidual(const CConfig* config) override;

};

/*!
 * \class CSourcePieceWise_TurbSST
 * \brief Class for integrating the source terms of the Menter SST turbulence model equations.
 * \ingroup SourceDiscr
 * \author A. Campos.
 */
class CSourcePieceWise_TurbSST final : public CNumerics {
private:
  su2double F1_i,
  F1_j,
  F2_i,
  F2_j;

  su2double alfa_1,
  alfa_2,
  beta_1,
  beta_2,
  sigma_k_1,
  sigma_k_2,
  sigma_w_1,
  sigma_w_2,
  beta_star,
  a1;

  su2double CDkw_i, CDkw_j;

  su2double kAmb, omegaAmb;

  su2double Residual[2],
  *Jacobian_i[2] = {nullptr},
  Jacobian_Buffer[4] = {0.0}; /// Static storage for the Jacobian (which needs to be pointer for return type).

  bool incompressible;
  bool sustaining_terms;
  bool axisymmetric;

  /*!
   * \brief A virtual member. Get strain magnitude based on perturbed reynolds stress matrix
   * \param[in] turb_ke: turbulent kinetic energy of the node
   */
  void SetPerturbedStrainMag(su2double turb_ke);

  /*!
   * \brief Add contribution due to axisymmetric formulation to 2D residual
   */
  inline void ResidualAxisymmetric(su2double alfa_blended, su2double zeta){

    if (Coord_i[1] < EPS) return;

    su2double yinv, rhov, k, w;
    su2double sigma_k_i, sigma_w_i;
    su2double pk_axi, pw_axi, cdk_axi, cdw_axi;

    AD::SetPreaccIn(Coord_i[1]);

    yinv = 1.0/Coord_i[1];
    rhov = Density_i*V_i[2];
    k = ScalarVar_i[0];
    w = ScalarVar_i[1];

    /*--- Compute blended constants ---*/
    sigma_k_i = F1_i*sigma_k_1+(1.0-F1_i)*sigma_k_2;
    sigma_w_i = F1_i*sigma_w_1+(1.0-F1_i)*sigma_w_2;

    /*--- Production ---*/
    pk_axi = max(0.0,2.0/3.0*rhov*k*(2.0/zeta*(yinv*V_i[2]-PrimVar_Grad_i[2][1]-PrimVar_Grad_i[1][0])-1.0));
    pw_axi = alfa_blended*zeta/k*pk_axi;

    /*--- Convection-Diffusion ---*/
    cdk_axi = rhov*k-(Laminar_Viscosity_i+sigma_k_i*Eddy_Viscosity_i)*ScalarVar_Grad_i[0][1];
    cdw_axi = rhov*w-(Laminar_Viscosity_i+sigma_w_i*Eddy_Viscosity_i)*ScalarVar_Grad_i[1][1];

    /*--- Add terms to the residuals ---*/
    Residual[0] += yinv*Volume*(pk_axi-cdk_axi);
    Residual[1] += yinv*Volume*(pw_axi-cdw_axi);

  }

public:
  /*!
   * \brief Constructor of the class.
   * \param[in] val_nDim - Number of dimensions of the problem.
   * \param[in] val_nVar - Number of variables of the problem.
   * \param[in] config - Definition of the particular problem.
   */
  CSourcePieceWise_TurbSST(unsigned short val_nDim, unsigned short val_nVar, const su2double* constants,
                           su2double val_kine_Inf, su2double val_omega_Inf, const CConfig* config);

  /*!
   * \brief Set the value of the first blending function.
   * \param[in] val_F1_i - Value of the first blending function at point i.
   * \param[in] val_F1_j - Value of the first blending function at point j.
   */
  inline void SetF1blending(su2double val_F1_i, su2double val_F1_j) override {
    F1_i = val_F1_i;
    F1_j = val_F1_j;
  }

  /*!
   * \brief Set the value of the second blending function.
   * \param[in] val_F2_i - Value of the second blending function at point i.
   * \param[in] val_F2_j - Value of the second blending function at point j.
   */
  inline void SetF2blending(su2double val_F2_i, su2double val_F2_j) override {
    F2_i = val_F2_i;
    F2_j = val_F2_j;
  }

  /*!
   * \brief Set the value of the cross diffusion for the SST model.
   * \param[in] val_CDkw_i - Value of the cross diffusion at point i.
   * \param[in] val_CDkw_j - Value of the cross diffusion at point j.
   */
  inline void SetCrossDiff(su2double val_CDkw_i, su2double val_CDkw_j) override {
    CDkw_i = val_CDkw_i;
    CDkw_j = val_CDkw_j;
  }

  /*!
   * \brief Residual for source term integration.
   * \param[in] config - Definition of the particular problem.
   * \return A lightweight const-view (read-only) of the residual/flux and Jacobians.
   */
  ResidualType<> ComputeResidual(const CConfig* config) override;

};


// >>>
/*!
 * \class CSourcePieceWise_TurbSST_new
 * \brief Class for integrating the source terms of the Menter SST turbulence model equations.
 * \ingroup SourceDiscr
 * \author A. Campos.
 */
class CSourcePieceWise_TurbSST_new final : public CNumerics {
private:
  su2double F1_i,
  F1_j,
  F2_i,
  F2_j;

  su2double alfa_1,
  alfa_2,
  beta_1,
  beta_2,
  sigma_k_1,
  sigma_k_2,
  sigma_w_1,
  sigma_w_2,
  beta_star,
  a1;

  su2double CDkw_i, CDkw_j;

  su2double kAmb, omegaAmb;

  su2double Residual[2],
  *Jacobian_i[2] = {nullptr},
  Jacobian_Buffer[4] = {0.0}; /// Static storage for the Jacobian (which needs to be pointer for return type).

  bool incompressible;
  bool sustaining_terms;
  bool axisymmetric;

  /*!
   * \brief A virtual member. Get strain magnitude based on perturbed reynolds stress matrix
   * \param[in] turb_ke: turbulent kinetic energy of the node
   */
  void SetPerturbedStrainMag(su2double turb_ke);

  /*!
   * \brief Add contribution due to axisymmetric formulation to 2D residual
   */
  inline void ResidualAxisymmetric(su2double alfa_blended, su2double zeta){

    if (Coord_i[1] < EPS) return;

    su2double yinv, rhov, k, w;
    su2double sigma_k_i, sigma_w_i;
    su2double pk_axi, pw_axi, cdk_axi, cdw_axi;

    AD::SetPreaccIn(Coord_i[1]);

    yinv = 1.0/Coord_i[1];
    rhov = Density_i*V_i[2];
    k = ScalarVar_i[0];
    w = ScalarVar_i[1];

    /*--- Compute blended constants ---*/
    sigma_k_i = F1_i*sigma_k_1+(1.0-F1_i)*sigma_k_2;
    sigma_w_i = F1_i*sigma_w_1+(1.0-F1_i)*sigma_w_2;

    /*--- Production ---*/
    pk_axi = max(0.0,2.0/3.0*rhov*k*(2.0/zeta*(yinv*V_i[2]-PrimVar_Grad_i[2][1]-PrimVar_Grad_i[1][0])-1.0));
    pw_axi = alfa_blended*zeta/k*pk_axi;

    /*--- Convection-Diffusion ---*/
    cdk_axi = rhov*k-(Laminar_Viscosity_i+sigma_k_i*Eddy_Viscosity_i)*ScalarVar_Grad_i[0][1];
    cdw_axi = rhov*w-(Laminar_Viscosity_i+sigma_w_i*Eddy_Viscosity_i)*ScalarVar_Grad_i[1][1];

    /*--- Add terms to the residuals ---*/
    Residual[0] += yinv*Volume*(pk_axi-cdk_axi);
    Residual[1] += yinv*Volume*(pw_axi-cdw_axi);

  }

public:
  /*!
   * \brief Constructor of the class.
   * \param[in] val_nDim - Number of dimensions of the problem.
   * \param[in] val_nVar - Number of variables of the problem.
   * \param[in] config - Definition of the particular problem.
   */
  CSourcePieceWise_TurbSST_new(unsigned short val_nDim, unsigned short val_nVar, const su2double* constants,
                           su2double val_kine_Inf, su2double val_omega_Inf, const CConfig* config, su2double Coord_Pos_x, su2double Coord_Pos_y);

  /*!
   * \brief Set the value of the first blending function.
   * \param[in] val_F1_i - Value of the first blending function at point i.
   * \param[in] val_F1_j - Value of the first blending function at point j.
   */
  inline void SetF1blending(su2double val_F1_i, su2double val_F1_j) override {
    F1_i = val_F1_i;
    F1_j = val_F1_j;
  }

  /*!
   * \brief Set the value of the second blending function.
   * \param[in] val_F2_i - Value of the second blending function at point i.
   * \param[in] val_F2_j - Value of the second blending function at point j.
   */
  inline void SetF2blending(su2double val_F2_i, su2double val_F2_j) override {
    F2_i = val_F2_i;
    F2_j = val_F2_j;
  }

  /*!
   * \brief Set the value of the cross diffusion for the SST model.
   * \param[in] val_CDkw_i - Value of the cross diffusion at point i.
   * \param[in] val_CDkw_j - Value of the cross diffusion at point j.
   */
  inline void SetCrossDiff(su2double val_CDkw_i, su2double val_CDkw_j) override {
    CDkw_i = val_CDkw_i;
    CDkw_j = val_CDkw_j;
  }

  /*!
   * \brief Residual for source term integration.
   * \param[in] config - Definition of the particular problem.
   * \return A lightweight const-view (read-only) of the residual/flux and Jacobians.
   */
  ResidualType<> ComputeResidual(const CConfig* config, su2double Coord_Pos_x, su2double Coord_Pos_y) override;

};
// <<<