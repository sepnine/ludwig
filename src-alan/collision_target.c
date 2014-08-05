
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "targetDP.h"
#include "pe.h"
#include "util.h"
#include "coords.h"
#include "physics.h"
#include "lattice.h"
#include "model.h"
#include "site_map.h"
#include "collision.h"
#include "fluctuations.h"

#include "phi.h"
#include "free_energy.h"
#include "phi_cahn_hilliard.h"

#include "control.h"
#include "propagation_ode.h"

static int nmodes_ = NVEL;               /* Modes to use in collsion stage */
static int nrelax_ = RELAXATION_M10;     /* [RELAXATION_M10|TRT|BGK] */
                                         /* Default is M10 */
static int isothermal_fluctuations_ = 0; /* Flag for noise. */

static double rtau_shear;       /* Inverse relaxation time for shear modes */
static double rtau_bulk;        /* Inverse relaxation time for bulk modes */
static double var_shear;        /* Variance for shear mode fluctuations */
static double var_bulk;         /* Variance for bulk mode fluctuations */
static double rtau_[NVEL];      /* Inverse relaxation times */
static double noise_var[NVEL];  /* Noise variances */

static fluctuations_t * fl_;

static void collision_multirelaxation(void);
static void collision_binary_lb(void);
static void collision_bgk(void);

static void fluctuations_off(double shat[3][3], double ghat[NVEL]);
       void collision_fluctuations(int index, double shat[3][3],
				   double ghat[NVEL]);

/* Constants*/

extern TARGET_CONST int tc_nSites;
extern TARGET_CONST double tc_rtau_shear;
extern TARGET_CONST double tc_rtau_bulk;
extern TARGET_CONST double tc_rtau[NVEL];
extern TARGET_CONST double tc_wv[NVEL];
extern TARGET_CONST double tc_ma[NVEL][NVEL];
extern TARGET_CONST double tc_mi[NVEL][NVEL];
extern TARGET_CONST int tc_cv[NVEL][3];
extern TARGET_CONST double tc_rtau2;
extern TARGET_CONST double tc_rcs2;
extern TARGET_CONST double tc_force_global[3];
extern TARGET_CONST double tc_d[3][3];
extern TARGET_CONST double tc_q[NVEL][3][3];


TARGET void collision_binary_lb_site( double* __restrict__ f_t, 
			   const double* __restrict__ force_t, 
			   double* __restrict__ velocity_t, 
			   const int baseIndex){

  int       p, m;                    /* velocity index */
  int       i, j;                    /* summed over indices ("alphabeta") */

  double    mode[NVEL*NILP];              /* Modes; hydrodynamic + ghost */
  double    rho[NILP], rrho[NILP];               /* Density, reciprocal density */
  double    uloc[3*NILP];                    /* Velocity */
  double    s[3][3*NILP];                 /* Stress */
  double    seq[3][3*NILP];               /* equilibrium stress */
  double    shat[3][3*NILP];              /* random stress */
  double    ghat[NVEL*NILP];              /* noise for ghosts */

  double    force[3*NILP];                /* External force */
  double    tr_s[NILP], tr_seq[NILP];

  const double   r3     = (1.0/3.0);

  double    phi[NILP], jdotc[NILP], sphidotq[NILP];    /* modes */
  double    jphi[3*NILP];
  double    sth[3][3*NILP], sphi[3][3*NILP];
  double    mu[NILP];                      /* Chemical potential */
  //  double    mobility;
  const double r2rcs4 = 4.5;         /* The constant 1 / 2 c_s^4 */

  //double (* chemical_potential)(const int index, const int nop);
  //void   (* chemical_stress)(const int index, double s[3][3]);
  
#define NDIST 2 //for binary collision
  
  //TEMP
  int vecIndex=0;
  
  double floc[NVEL*NDIST*NILP];
  
#define ILPIDX(instrn) (instrn)*NILP+vecIndex 


  //TO DO HACK

  for (i = 0; i < 3; i++) {
    for (j = 0; j < 3; j++) {
      TARGET_ILP(vecIndex) shat[i][ILPIDX(j)] = 0.0;
    }
  }
  
  for (i = NHYDRO; i < NVEL; i++) {
    TARGET_ILP(vecIndex) ghat[ILPIDX(i)] = 0.0;
  }

  
  /* load data */

  for(p = 0; p < NVEL; p++) {
    for(m = 0; m < NDIST; m++) {
      TARGET_ILP(vecIndex) floc[ILPIDX(NVEL*m+p)] = 
	f_t[tc_nSites*NDIST*p + tc_nSites*m + baseIndex + vecIndex];
    }
  }

  
  for (m = 0; m < nmodes_; m++) {
    TARGET_ILP(vecIndex) mode[ILPIDX(m)] = 0.0;
    for (p = 0; p < NVEL; p++) {
      TARGET_ILP(vecIndex) mode[ILPIDX(m)] += floc[ILPIDX(p)]*tc_ma[m][p];
    }
    
  }
  

  /* For convenience, write out the physical modes. */
  
  TARGET_ILP(vecIndex) rho[vecIndex] = mode[ILPIDX(0)];
  for (i = 0; i < 3; i++) {
    TARGET_ILP(vecIndex) uloc[ILPIDX(i)] = mode[ILPIDX(1 + i)];
  }

  TARGET_ILP(vecIndex){
    s[X][ILPIDX(X)] = mode[ILPIDX(4)];
    s[X][ILPIDX(Y)] = mode[ILPIDX(5)];
    s[X][ILPIDX(Z)] = mode[ILPIDX(6)];
    s[Y][ILPIDX(X)] = s[X][ILPIDX(Y)];
    s[Y][ILPIDX(Y)] = mode[ILPIDX(7)];
    s[Y][ILPIDX(Z)] = mode[ILPIDX(8)];
    s[Z][ILPIDX(X)] = s[X][ILPIDX(Z)];
    s[Z][ILPIDX(Y)] = s[Y][ILPIDX(Z)];
    s[Z][ILPIDX(Z)] = mode[ILPIDX(9)];  
  }
  /* Compute the local velocity, taking account of any body force */
  
  TARGET_ILP(vecIndex) rrho[vecIndex] = 1.0/rho[vecIndex] ;
  


  
  for (i = 0; i < 3; i++) {	

    //TODO NEED TO TRANSPOSE FORCE, VELOCITY

    TARGET_ILP(vecIndex){
      force[ILPIDX(i)] = (tc_force_global[i] + force_t[(baseIndex+vecIndex)*3+i]);
      uloc[ILPIDX(i)] = rrho[vecIndex]*(uloc[ILPIDX(i)] + 0.5*force[ILPIDX(i)]);  
    }
  }
  
  //      hydrodynamics_set_velocity(baseIndex, u);
  for (i = 0; i < 3; i++) 
    TARGET_ILP(vecIndex) velocity_t[(baseIndex+vecIndex)*3+i]=uloc[ILPIDX(i)];
  
  
  /* Compute the thermodynamic component of the stress */
  //HACK
  for(i=0;i<3;i++)
    for(j=0;j<3;j++)
      TARGET_ILP(vecIndex) sth[i][ILPIDX(j)]=0;
  
  //chemical_stress(baseIndex, sth);
  
  /* Relax stress with different shear and bulk viscosity */
  
  TARGET_ILP(vecIndex) tr_s[vecIndex]  = 0.0;
  TARGET_ILP(vecIndex) tr_seq[vecIndex]  = 0.0;
  
  for (i = 0; i < 3; i++) {
    /* Set equilibrium stress, which includes thermodynamic part */
    for (j = 0; j < 3; j++) {
      TARGET_ILP(vecIndex)
	seq[i][ILPIDX(j)] = rho[vecIndex]*uloc[ILPIDX(i)]*uloc[ILPIDX(j)] + sth[i][ILPIDX(j)];
    }
    /* Compute trace */
    TARGET_ILP(vecIndex){
      tr_s[vecIndex]   += s[i][ILPIDX(i)];
      tr_seq[vecIndex] += seq[i][ILPIDX(i)];
    }
  }
  
  /* Form traceless parts */
  for (i = 0; i < 3; i++) {
    TARGET_ILP(vecIndex){
      s[i][ILPIDX(i)]   -= r3*tr_s[vecIndex];
      seq[i][ILPIDX(i)] -= r3*tr_seq[vecIndex];
    }
  }
  
  /* Relax each mode */
  TARGET_ILP(vecIndex) tr_s[vecIndex] = tr_s[vecIndex] - tc_rtau_bulk*(tr_s[vecIndex]
						  - tr_seq[vecIndex]);
  
  for (i = 0; i < 3; i++) {
    for (j = 0; j < 3; j++) {
      TARGET_ILP(vecIndex){
	s[i][ILPIDX(j)] -= tc_rtau_shear*(s[i][ILPIDX(j)] - seq[i][ILPIDX(j)]);
	s[i][ILPIDX(j)] += tc_d[i][j]*r3*tr_s[vecIndex];
	
	/* Correction from body force (assumes equal relaxation times) */
	
	s[i][ILPIDX(j)] += (2.0-tc_rtau_shear)*(uloc[ILPIDX(i)]*force[ILPIDX(j)] + force[ILPIDX(i)]*uloc[ILPIDX(j)]);
	shat[i][ILPIDX(j)] = 0.0;
      }
    }
  }
  


  //if (isothermal_fluctuations_) {
  //	collision_fluctuations(baseIndex, shat, ghat);
  //}
  
  /* Now reset the hydrodynamic modes to post-collision values */
  
  TARGET_ILP(vecIndex){
    mode[ILPIDX(1)] = mode[ILPIDX(1)] + force[ILPIDX(X)];    /* Conserved if no force */
    mode[ILPIDX(2)] = mode[ILPIDX(2)] + force[ILPIDX(Y)];    /* Conserved if no force */
    mode[ILPIDX(3)] = mode[ILPIDX(3)] + force[ILPIDX(Z)];    /* Conserved if no force */
    mode[ILPIDX(4)] = s[X][ILPIDX(X)] + shat[X][ILPIDX(X)];
    mode[ILPIDX(5)] = s[X][ILPIDX(Y)] + shat[X][ILPIDX(Y)];
    mode[ILPIDX(6)] = s[X][ILPIDX(Z)] + shat[X][ILPIDX(Z)];
    mode[ILPIDX(7)] = s[Y][ILPIDX(Y)] + shat[Y][ILPIDX(Y)];
    mode[ILPIDX(8)] = s[Y][ILPIDX(Z)] + shat[Y][ILPIDX(Z)];
    mode[ILPIDX(9)] = s[Z][ILPIDX(Z)] + shat[Z][ILPIDX(Z)];
  }
  
  
  /* Ghost modes are relaxed toward zero equilibrium. */
  
  for (m = NHYDRO; m < nmodes_; m++) {
    TARGET_ILP(vecIndex)
      mode[ILPIDX(m)] = mode[ILPIDX(m)] 
      - tc_rtau[ILPIDX(m)]*(mode[ILPIDX(m)] - 0.0) + ghat[ILPIDX(m)];
  }
  
  
  /* Project post-collision modes back onto the distribution */
  /* matrix multiplication for full SIMD vector */

  double ftmp[NILP];
  
  for (p = 0; p < NVEL; p++) {
    TARGET_ILP(vecIndex) ftmp[vecIndex] = 0.0;
    for (m = 0; m < nmodes_; m++) {
      TARGET_ILP(vecIndex) ftmp[vecIndex] += tc_mi[p][m]*mode[ILPIDX(m)];
    }
    TARGET_ILP(vecIndex) f_t[tc_nSites*NDIST*p + baseIndex + vecIndex] = ftmp[vecIndex];
  }
  
  
  /* Now, the order parameter distribution */
  //HACK
  TARGET_ILP(vecIndex){
    phi[vecIndex]=0;
    mu[vecIndex]=0;
  }

  //phi =  phi_t[baseIndex];;
  //mu = chemical_potential(baseIndex, 0);
  
  TARGET_ILP(vecIndex){
    jphi[ILPIDX(X)] = 0.0;
    jphi[ILPIDX(Y)] = 0.0;
    jphi[ILPIDX(Z)] = 0.0;
  }
  for (p = 1; p < NVEL; p++) {
    for (i = 0; i < 3; i++) {
      TARGET_ILP(vecIndex) jphi[ILPIDX(i)] += floc[ILPIDX(NVEL+p)]*tc_cv[p][i];
    }
  }
  
  
  /* Relax order parameters modes. See the comments above. */
  
  for (i = 0; i < 3; i++) {
    for (j = 0; j < 3; j++) {
      TARGET_ILP(vecIndex) sphi[i][ILPIDX(j)] = phi[vecIndex]*uloc[ILPIDX(i)]
	*uloc[ILPIDX(j)] + mu[vecIndex]*tc_d[i][j];
      /* sphi[i][j] = phi*uloc[i]*uloc[j] + cs2*mobility*mu*d_[i][j];*/
    }
    TARGET_ILP(vecIndex) jphi[ILPIDX(i)] = jphi[ILPIDX(i)] 
      - tc_rtau2*(jphi[ILPIDX(i)] - phi[vecIndex]*uloc[ILPIDX(i)]);
    /* jphi[i] = phi*uloc[i];*/
  }
  
  /* Now update the distribution */
  
  for (p = 0; p < NVEL; p++) {
    
    int dp0 = (p == 0);
    TARGET_ILP(vecIndex){
      jdotc[vecIndex]    = 0.0;
      sphidotq[vecIndex] = 0.0;
    }
    
    for (i = 0; i < 3; i++) {
      TARGET_ILP(vecIndex) jdotc[vecIndex] += jphi[ILPIDX(i)]*tc_cv[p][i];
      for (j = 0; j < 3; j++) {
	TARGET_ILP(vecIndex) sphidotq[vecIndex] += sphi[i][ILPIDX(j)]*tc_q[p][i][j];
      }
    }
    
    /* Project all this back to the distributions. The magic
     * here is to move phi into the non-propagating distribution. */
    
    TARGET_ILP(vecIndex) f_t[tc_nSites*NDIST*p + tc_nSites + baseIndex + vecIndex] = 
      tc_wv[p]*(jdotc[vecIndex]*tc_rcs2 + sphidotq[vecIndex]*r2rcs4) + phi[vecIndex]*dp0;
  }
  
  return;
  
}



TARGET_ENTRY void collision_binary_lb_lattice( double* __restrict__ f_t, 
				  const double* __restrict__ force_t, 
					       double* __restrict__ velocity_t,
					       const int nSites){

  int tpIndex;
  TARGET_TLP(tpIndex,nSites)
    {
	
  	collision_binary_lb_site( f_t, force_t, velocity_t,tpIndex);

    }

}

