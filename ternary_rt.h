/****************************************************************************
 *
 *  fe_ternary_rt.h
 *
 *  Edinburgh Soft Matter and Statistical Physics Group and
 *  Edinburgh Parallel Computing Centre
 *
 *  (c) 2010-2019 The University of Edinburgh
 *
 *  Contributing authors:
 *  Kevin Stratford (kevin@epcc.ed.ac.uk)
 *
 ****************************************************************************/
#ifndef LUDWIG_TERNARY_RT_H
#define LUDWIG_TERNARY_RT_H


#include "pe.h"
#include "runtime.h"
#include "ternary.h"

__host__ int fe_ternary_param_rt(pe_t * pe, rt_t * rt, fe_ternary_param_t * p);
__host__ int fe_ternary_phi_init_rt(pe_t * pe, rt_t * rt, fe_ternary_t * fe,
                                 field_t * phi);
__host__ int fe_ternary_psi_init_rt(pe_t * pe, rt_t * rt, fe_ternary_t * fe,
                                 field_t * phi);
#endif
