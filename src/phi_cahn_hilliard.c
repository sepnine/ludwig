/*****************************************************************************
 *
 *  phi_cahn_hilliard.c
 *
 *  The time evolution of the order parameter phi is described
 *  by the Cahn Hilliard equation
 *
 *     d_t phi + div (u phi - M grad mu - \hat{xi}) = 0.
 *
 *  The equation is solved here via finite difference. The velocity
 *  field u is assumed known from the hydrodynamic sector. M is the
 *  order parameter mobility. The chemical potential mu is set via
 *  the choice of free energy.
 *
 *  Random fluxes \hat{xi} can be included if required using the
 *  lattice noise generator with variance 2 M kT. The implementation
 *  is (broadly) based on that of Sumesh PT et al, Phys Rev E 84
 *  046709 (2011).
 *
 *  The important thing for the noise is that an expanded stencil
 *  is required for the diffusive fluxes, here via phi_ch_flux_mu2().
 *
 *  Lees-Edwards planes are allowed (but not with noise, at present).
 *  This requires fixes at the plane boudaries to get consistent
 *  fluxes.
 *
 *
 *  $Id$
 *
 *  Edinburgh Soft Matter and Statistical Physics Group and
 *  Edinburgh Parallel Computing Centre
 *
 *  Contributions:
 *  Thanks to Markus Gross, who hepled to validate the noise implemantation.
 *  Kevin Stratford (kevin@epcc.ed.ac.uk)
 *  (c) 2010 The University of Edinburgh
 *
 *****************************************************************************/

#include <assert.h>
#include <stdlib.h>
#include <math.h>

#include "pe.h"
#include "coords.h"
#include "control.h"
#include "leesedwards.h"
#include "advection.h"
#include "free_energy.h"
#include "physics.h"
#include "advection_s.h"
#include "advection_bcs.h"
#include "phi_cahn_hilliard.h"

static int phi_ch_flux_mu1(fe_t * fes, advflux_t * flux);
static int phi_ch_flux_mu2(fe_t * fes, double * fe, double * fw, double * fy, double * fz);
static int phi_ch_update_forward_step(field_t * phif, advflux_t * flux);

static int phi_ch_le_fix_fluxes(int nf, double * fe, double * fw);
static int phi_ch_le_fix_fluxes_parallel(int nf, double * fe, double * fw);
static int phi_ch_random_flux(noise_t * noise, double * fe, double * fw,
			      double * fy, double * fz);

/*****************************************************************************
 *
 *  phi_cahn_hilliard
 *
 *  Compute the fluxes (advective/diffusive) and compute the update
 *  to the order parameter field phi.
 *
 *  Conservation is ensured by face-flux uniqueness. However, in the
 *  x-direction, the fluxes at the east face and west face of a given
 *  cell must be handled spearately to take account of Lees Edwards
 *  boundaries.
 *
 *  hydro is allowed to be NULL, in which case the dynamics is
 *  just relaxational (no velocity field).
 *
 *  map is also allowed to be NULL, in which case there is no
 *  check for for the surface no flux condition. (This still
 *  may be relevant for diffusive fluxes only, so does not
 *  depend on hydrodynamics.)
 *
 *  The noise_t structure controls random fluxes, if required;
 *  it can be NULL in which case no noise.
 *
 *****************************************************************************/

int phi_cahn_hilliard(fe_t * fe, field_t * phi, hydro_t * hydro, map_t * map,
		      noise_t * noise) {
  int nf;
  int noise_phi = 0;
  advflux_t * fluxes = NULL;

  assert(fe);
  assert(phi);

  if (noise) noise_present(noise, NOISE_PHI, &noise_phi);

  field_nf(phi, &nf);
  assert(nf == 1);

  advflux_create(nf, &fluxes);

  /* Compute any advective fluxes first, then accumulate diffusive
   * and random fluxes. */

  if (hydro) {
    hydro_u_halo(hydro); /* Reposition to main to prevent repeat */
    hydro_lees_edwards(hydro); /* Repoistion to main ditto */
    advection_bcs_wall(phi);
    advection_x(fluxes, hydro, phi);
  }

  if (noise_phi) {
    phi_ch_flux_mu2(fe, fluxes->fe, fluxes->fw, fluxes->fy, fluxes->fz);
    phi_ch_random_flux(noise, fluxes->fe, fluxes->fw, fluxes->fy, fluxes->fz);
  }
  else {
    phi_ch_flux_mu1(fe, fluxes);
  }

  /* No flux boundaries (diffusive fluxes, and hydrodynamic, if present) */

  if (map) advection_bcs_no_normal_flux(nf, fluxes, map);

  phi_ch_le_fix_fluxes(nf, fluxes->fe, fluxes->fw);
  phi_ch_update_forward_step(phi, fluxes);

  advflux_free(fluxes);

  return 0;
}

/*****************************************************************************
 *
 *  phi_ch_flux_mu1
 *
 *  Accumulate [add to a previously computed advective flux] the
 *  'diffusive' contribution related to the chemical potential. It's
 *  computed everywhere regardless of fluid/solid status.
 *
 *  This is a two point stencil the in the chemical potential,
 *  and the mobility is constant.
 *
 *****************************************************************************/

static int phi_ch_flux_mu1(fe_t * fe, advflux_t * flux) {

  int nlocal[3];
  int ic, jc, kc;
  int index0, index1;
  int icm1, icp1;
  int nsites;
  double mu0, mu1;
  double mobility;

  assert(fe);
  assert(fe->func->mu);
  assert(flux);

  coords_nlocal(nlocal);
  assert(coords_nhalo() >= 2);
  nsites = le_nsites();

  physics_mobility(&mobility);

  for (ic = 1; ic <= nlocal[X]; ic++) {
    icm1 = le_index_real_to_buffer(ic, -1);
    icp1 = le_index_real_to_buffer(ic, +1);
    for (jc = 0; jc <= nlocal[Y]; jc++) {
      for (kc = 0; kc <= nlocal[Z]; kc++) {

	index0 = le_site_index(ic, jc, kc);

	fe->func->mu(fe, index0, &mu0);

	/* x-direction (between ic-1 and ic) */

	index1 = le_site_index(icm1, jc, kc);
	fe->func->mu(fe, index1, &mu1);
	flux->fw[addr_rank0(nsites, index0)] -= mobility*(mu0 - mu1);

	/* ...and between ic and ic+1 */

	index1 = le_site_index(icp1, jc, kc);
	fe->func->mu(fe, index1, &mu1);
	flux->fe[addr_rank0(nsites, index0)] -= mobility*(mu1 - mu0);

	/* y direction */

	index1 = le_site_index(ic, jc+1, kc);
	fe->func->mu(fe, index1, &mu1);
	flux->fy[addr_rank0(nsites, index0)] -= mobility*(mu1 - mu0);

	/* z direction */

	index1 = le_site_index(ic, jc, kc+1);
	fe->func->mu(fe, index1, &mu1);
	flux->fz[addr_rank0(nsites, index0)] -= mobility*(mu1 - mu0);

	/* Next site */
      }
    }
  }

  return 0;
}

/*****************************************************************************
 *
 *  phi_ch_flux_mu2
 *
 *  Accumulate [add to previously computed advective fluxes]
 *  diffusive fluxes related to the mobility.
 *
 *  This version is based on Sumesh et al to allow correct
 *  treatment of noise. The fluxes are calculated via
 *
 *  grad_x mu = 0.5*(mu(i+1) - mu(i-1)) etc
 *  flux_x(x + 1/2) = -0.5*M*(grad_x mu(i) + grad_x mu(i+1)) etc
 *
 *  In contrast to Sumesh et al., we don't have 'diagonal' fluxes
 *  yet. There are also no Lees Edwards planes yet.
 *
 *****************************************************************************/

static int phi_ch_flux_mu2(fe_t * fesymm, double * fe, double * fw,
			   double * fy,
			   double * fz) {
  int nhalo;
  int nsites;
  int nlocal[3];
  int ic, jc, kc;
  int index0;
  int xs, ys, zs;
  double mum2, mum1, mu00, mup1, mup2;
  double mobility;

  nhalo = coords_nhalo();
  nsites = le_nsites();
  coords_nlocal(nlocal);
  assert(nhalo >= 3);

  physics_mobility(&mobility);

  zs = 1;
  ys = (nlocal[Z] + 2*nhalo)*zs;
  xs = (nlocal[Y] + 2*nhalo)*ys;

  for (ic = 1; ic <= nlocal[X]; ic++) {
    for (jc = 0; jc <= nlocal[Y]; jc++) {
      for (kc = 0; kc <= nlocal[Z]; kc++) {

	index0 = coords_index(ic, jc, kc);
	fesymm->func->mu(fesymm, index0 - 2*xs, &mum2);
	fesymm->func->mu(fesymm, index0 - 1*xs, &mum1);
	fesymm->func->mu(fesymm, index0,        &mu00);
	fesymm->func->mu(fesymm, index0 + 1*xs, &mup1);
	fesymm->func->mu(fesymm, index0 + 2*xs, &mup2);

	/* x-direction (between ic-1 and ic) */

	fw[addr_rank0(nsites, index0)]
	  -= 0.25*mobility*(mup1 + mu00 - mum1 - mum2);

	/* ...and between ic and ic+1 */

	fe[addr_rank0(nsites, index0)]
	  -= 0.25*mobility*(mup2 + mup1 - mu00 - mum1);

	/* y direction between jc and jc+1 */

	fesymm->func->mu(fesymm, index0 - 1*ys, &mum1);
	fesymm->func->mu(fesymm, index0 + 1*ys, &mup1);
	fesymm->func->mu(fesymm, index0 + 2*ys, &mup2);

	fy[addr_rank0(nsites, index0)]
	  -= 0.25*mobility*(mup2 + mup1 - mu00 - mum1);

	/* z direction between kc and kc+1 */

	fesymm->func->mu(fesymm, index0 - 1*zs, &mum1);
	fesymm->func->mu(fesymm, index0 + 1*zs, &mup1);
	fesymm->func->mu(fesymm, index0 + 2*zs, &mup2);

	fz[addr_rank0(nsites, index0)]
	  -= 0.25*mobility*(mup2 + mup1 - mu00 - mum1);

	/* Next site */
      }
    }
  }

  return 0;
}

/*****************************************************************************
 *
 *  phi_ch_random_flux
 *
 *  This adds (repeat adds) the random contribution to the face
 *  fluxes (advective + diffusive) following Sumesh et al 2011.
 *
 *****************************************************************************/

static int phi_ch_random_flux(noise_t * noise, double * fe, double * fw,
			      double * fy, double * fz) {

  int ic, jc, kc, index0, index1;
  int nsites, nextra;
  int nlocal[3];
  int ia;

  double * rflux;
  double reap[3];
  double kt, mobility, var;

  assert(le_get_nplane_local() == 0);
  assert(coords_nhalo() >= 1);

  /* Variance of the noise from fluctuation dissipation relation */

  physics_kt(&kt);
  physics_mobility(&mobility);
  var = sqrt(2.0*kt*mobility);

  nsites = coords_nsites();
  rflux = (double *) malloc(3*nsites*sizeof(double));
  if (rflux == NULL) fatal("malloc(rflux) failed\n");

  coords_nlocal(nlocal);

  /* We go one site into the halo region to allow all the fluxes to
   * be comupted locally. */
  nextra = 1;

  for (ic = 1 - nextra; ic <= nlocal[X] + nextra; ic++) {
    for (jc = 1 - nextra; jc <= nlocal[Y] + nextra; jc++) {
      for (kc = 1 - nextra; kc <= nlocal[Z] + nextra; kc++) {

        index0 = coords_index(ic, jc, kc);
        noise_reap_n(noise, index0, 3, reap);

        for (ia = 0; ia < 3; ia++) {
          rflux[addr_rank1(nsites, 3, index0, ia)] = var*reap[ia];
        }

      }
    }
  }

  /* Now accumulate the mid-point fluxes */

  for (ic = 1; ic <= nlocal[X]; ic++) {
    for (jc = 0; jc <= nlocal[Y]; jc++) {
      for (kc = 0; kc <= nlocal[Z]; kc++) {

	index0 = coords_index(ic, jc, kc);

	/* x-direction */

	index1 = coords_index(ic-1, jc, kc);
	fw[addr_rank0(nsites, index0)]
	  += 0.5*(rflux[addr_rank1(nsites, 3, index0, X)] +
		  rflux[addr_rank1(nsites, 3, index1, X)]);

	index1 = coords_index(ic+1, jc, kc);
	fe[addr_rank0(nsites, index0)]
	  += 0.5*(rflux[addr_rank1(nsites, 3, index0, X)] +
		  rflux[addr_rank1(nsites, 3, index1, X)]);

	/* y direction */

	index1 = coords_index(ic, jc+1, kc);
	fy[addr_rank0(nsites, index0)]
	  += 0.5*(rflux[addr_rank1(nsites, 3, index0, Y)] +
		  rflux[addr_rank1(nsites, 3, index1, Y)]);

	/* z direction */

	index1 = coords_index(ic, jc, kc+1);
	fz[addr_rank0(nsites, index0)]
	  += 0.5*(rflux[addr_rank1(nsites, 3, index0, Z)] +
		  rflux[addr_rank1(nsites, 3, index1, Z)]);
      }
    }
  }

  free(rflux);

  return 0;
}

/*****************************************************************************
 *
 *  phi_ch_le_fix_fluxes
 *
 *  Owing to the non-linear calculation for the fluxes,
 *  the LE-interpolated phi field doesn't give a unique
 *  east-west face flux.
 *
 *  This ensures uniqueness, by averaging the relevant
 *  contributions from each side of the plane.
 *
 *  I've retained nop here, as these functions might be useful
 *  for general cases.
 *
 *****************************************************************************/

static int phi_ch_le_fix_fluxes(int nf, double * fe, double * fw) {

  int nlocal[3]; /* Local system size */
  int ip;        /* Index of the plane */
  int ic;        /* Index x location in real system */
  int jc, kc, n;
  int index, index1;
  int nbuffer;

  double dy;     /* Displacement for current plane */
  double fr;     /* Fractional displacement */
  double t;      /* Time */
  int jdy;       /* Integral part of displacement */
  int j1, j2;    /* j values in real system to interpolate between */

  double * bufferw;
  double * buffere;

  int get_step(void);

  if (cart_size(Y) > 1) {
    /* Parallel */
    phi_ch_le_fix_fluxes_parallel(nf, fe, fw);
  }
  else {
    /* Can do it directly */

    coords_nlocal(nlocal);

    nbuffer = nf*nlocal[Y]*nlocal[Z];
    buffere = (double *) malloc(nbuffer*sizeof(double));
    bufferw = (double *) malloc(nbuffer*sizeof(double));
    if (buffere == NULL) fatal("malloc(buffere) failed\n");
    if (bufferw == NULL) fatal("malloc(bufferw) failed\n");

    for (ip = 0; ip < le_get_nplane_local(); ip++) {

      /* -1.0 as zero required for first step; a 'feature' to
       * maintain the regression tests */

      t = 1.0*get_step() - 1.0;

      ic = le_plane_location(ip);

      /* Looking up */
      dy = +t*le_plane_uy(t);
      dy = fmod(dy, L(Y));
      jdy = floor(dy);
      fr  = dy - jdy;

      for (jc = 1; jc <= nlocal[Y]; jc++) {

	j1 = 1 + (jc - jdy - 2 + 2*nlocal[Y]) % nlocal[Y];
	j2 = 1 + j1 % nlocal[Y];

	for (kc = 1; kc <= nlocal[Z]; kc++) {
	  for (n = 0; n < nf; n++) {
	    /* This could be replaced by just count++ (check) to addr buffer */
	    index = nf*(nlocal[Z]*(jc-1) + (kc-1)) + n;

	    bufferw[index] = fr*fw[addr_rank1(le_nsites(), nf, le_site_index(ic+1,j1,kc), n)]
	      + (1.0-fr)*fw[addr_rank1(le_nsites(), nf, le_site_index(ic+1,j2,kc), n)];
	  }
	}
      }


      /* Looking down */

      dy = -t*le_plane_uy(t);
      dy = fmod(dy, L(Y));
      jdy = floor(dy);
      fr  = dy - jdy;

      for (jc = 1; jc <= nlocal[Y]; jc++) {

	j1 = 1 + (jc - jdy - 2 + 2*nlocal[Y]) % nlocal[Y];
	j2 = 1 + j1 % nlocal[Y];

	for (kc = 1; kc <= nlocal[Z]; kc++) {
	  for (n = 0; n < nf; n++) {
	    index = nf*(nlocal[Z]*(jc-1) + (kc-1)) + n;
	    buffere[index] = fr*fe[addr_rank1(le_nsites(), nf, le_site_index(ic,j1,kc), n)]
	      + (1.0-fr)*fe[addr_rank1(le_nsites(), nf, le_site_index(ic,j2,kc), n)];
	  }
	}
      }

      /* Now average the fluxes. */

      for (jc = 1; jc <= nlocal[Y]; jc++) {
	for (kc = 1; kc <= nlocal[Z]; kc++) {
	  for (n = 0; n < nf; n++) {
	    index = nf*le_site_index(ic,jc,kc) + n;
	    index1 = nf*(nlocal[Z]*(jc-1) + (kc-1)) + n;

	    index = addr_rank1(le_nsites(), nf, le_site_index(ic,jc,kc), n);
	    fe[index] = 0.5*(fe[index] + bufferw[index1]);
	    index = addr_rank1(le_nsites(), nf, le_site_index(ic+1,jc,kc), n);
	    fw[index] = 0.5*(fw[index] + buffere[index1]);
	  }
	}
      }

      /* Next plane */
    }

    free(bufferw);
    free(buffere);
  }

  return 0;
}

/*****************************************************************************
 *
 *  phi_ch_le_fix_fluxes_parallel
 *
 *  Parallel version of the above, where we need to communicate to
 *  get hold of the appropriate fluxes.
 *
 *****************************************************************************/


static int phi_ch_le_fix_fluxes_parallel(int nf, double * fe, double * fw) {

  int      nhalo;
  int      nlocal[3];      /* Local system size */
  int      noffset[3];     /* Local starting offset */
  int ip;                  /* Index of the plane */
  int ic;                  /* Index x location in real system */
  int jc, kc, j1, j2;
  int n, n1, n2;
  int index;
  double dy;               /* Displacement for current transforamtion */
  double fre, frw;         /* Fractional displacements */
  double t;                /* Time */
  int jdy;                 /* Integral part of displacement */

  /* Messages */

  int nsend;               /* N send data */
  int nrecv;               /* N recv data */
  int nrank_s[3];          /* send ranks */
  int nrank_r[3];          /* recv ranks */
  const int tag0 = 1254;
  const int tag1 = 1255;

  double * sbufe = NULL;   /* Send buffer */
  double * sbufw = NULL;   /* Send buffer */
  double * rbufe = NULL;   /* Interpolation buffer */
  double * rbufw = NULL;

  MPI_Comm    le_comm;
  MPI_Request rreq[4], sreq[4];
  MPI_Status  status[4];

  nhalo = coords_nhalo();
  coords_nlocal(nlocal);
  coords_nlocal_offset(noffset);

  le_comm = le_communicator();

  /* Allocate the temporary buffer */

  nsend = nf*nlocal[Y]*(nlocal[Z] + 2*nhalo);
  nrecv = nf*(nlocal[Y] + 1)*(nlocal[Z] + 2*nhalo);

  sbufe = (double *) malloc(nsend*sizeof(double));
  sbufw = (double *) malloc(nsend*sizeof(double));

  if (sbufe == NULL) fatal("malloc(sbufe) failed\n");
  if (sbufw == NULL) fatal("malloc(sbufw) failed\n");

  rbufe = (double *) malloc(nrecv*sizeof(double));
  rbufw = (double *) malloc(nrecv*sizeof(double));

  if (rbufe == NULL) fatal("malloc(rbufe) failed\n");
  if (rbufw == NULL) fatal("malloc(rbufw) failed\n");

  /* -1.0 as zero required for fisrt step; this is a 'feature'
   * to ensure the regression tests stay te same */

  t = 1.0*get_step() - 1.0;

  /* One round of communication for each plane */

  for (ip = 0; ip < le_get_nplane_local(); ip++) {

    ic = le_plane_location(ip);

    /* Work out the displacement-dependent quantities */

    dy = +t*le_plane_uy(t);
    dy = fmod(dy, L(Y));
    jdy = floor(dy);
    frw  = dy - jdy;

    /* First (global) j1 required is j1 = (noffset[Y] + 1) - jdy - 1.
     * Modular arithmetic ensures 1 <= j1 <= N_total(Y). */

    jc = noffset[Y] + 1;
    j1 = 1 + (jc - jdy - 2 + 2*N_total(Y)) % N_total(Y);
    assert(j1 > 0);
    assert(j1 <= N_total(Y));

    le_jstart_to_ranks(j1, nrank_s, nrank_r);

    /* Local quantities: given a local starting index j2, we receive
     * n1 + n2 sites into the buffer, and send n1 sites starting with
     * j2, and the remaining n2 sites from starting position 1. */

    j2 = 1 + (j1 - 1) % nlocal[Y];
    assert(j2 > 0);
    assert(j2 <= nlocal[Y]);

    n1 = nf*(nlocal[Y] - j2 + 1)*(nlocal[Z] + 2*nhalo);
    n2 = nf*j2*(nlocal[Z] + 2*nhalo);

    /* Post receives, sends (the wait is later). */

    MPI_Irecv(rbufw,    n1, MPI_DOUBLE, nrank_r[0], tag0, le_comm, rreq);
    MPI_Irecv(rbufw+n1, n2, MPI_DOUBLE, nrank_r[1], tag1, le_comm, rreq + 1);

    /* Load send buffer from fw */
    /* (ic+1,j2,1-nhalo) and (ic+1,1,1-nhalo) */

    for (jc = 1; jc <= nlocal[Y]; jc++) {
      for (kc = 1 - nhalo; kc <= nlocal[Z] + nhalo; kc++) {
	index = le_site_index(ic+1, jc, kc);
	for (n = 0; n < nf; n++) {
	  j1 = nf*(jc - 1)*(nlocal[Z] + 2*nhalo) + nf*(kc + nhalo - 1) + n;
	  assert(j1 >= 0 && j1 < nsend);
	  sbufw[j1] = fw[addr_rank1(le_nsites(), nf, index, n)];
	}
      }
    }

    j1 = (j2 - 1)*nf*(nlocal[Z] + 2*nhalo);
    MPI_Issend(sbufw + j1, n1, MPI_DOUBLE, nrank_s[0], tag0, le_comm, sreq);
    MPI_Issend(sbufw     , n2, MPI_DOUBLE, nrank_s[1], tag1, le_comm, sreq+1);

    /* OTHER WAY */

    kc = 1 - nhalo;

    dy = -t*le_plane_uy(t);
    dy = fmod(dy, L(Y));
    jdy = floor(dy);
    fre  = dy - jdy;

    /* First (global) j1 required is j1 = (noffset[Y] + 1) - jdy - 1.
     * Modular arithmetic ensures 1 <= j1 <= N_total(Y). */

    jc = noffset[Y] + 1;
    j1 = 1 + (jc - jdy - 2 + 2*N_total(Y)) % N_total(Y);

    le_jstart_to_ranks(j1, nrank_s, nrank_r);

    /* Local quantities: given a local starting index j2, we receive
     * n1 + n2 sites into the buffer, and send n1 sites starting with
     * j2, and the remaining n2 sites from starting position nhalo. */

    j2 = 1 + (j1 - 1) % nlocal[Y];

    n1 = nf*(nlocal[Y] - j2 + 1)*(nlocal[Z] + 2*nhalo);
    n2 = nf*j2*(nlocal[Z] + 2*nhalo);

    /* Post new receives, sends, and wait for whole lot to finish. */

    MPI_Irecv(rbufe,    n1, MPI_DOUBLE, nrank_r[0], tag0, le_comm, rreq + 2);
    MPI_Irecv(rbufe+n1, n2, MPI_DOUBLE, nrank_r[1], tag1, le_comm, rreq + 3);

    /* Load send buffer from fe */

    for (jc = 1; jc <= nlocal[Y]; jc++) {
      for (kc = 1 - nhalo; kc <= nlocal[Z] + nhalo; kc++) {
	index = le_site_index(ic, jc, kc);
	for (n = 0; n < nf; n++) {
	  j1 = (jc - 1)*nf*(nlocal[Z] + 2*nhalo) + nf*(kc + nhalo - 1) + n;
	  assert(j1 >= 0 && jc < nsend);
	  sbufe[j1] = fe[addr_rank1(le_nsites(), nf, index, n)];
	}
      }
    }

    j1 = (j2 - 1)*nf*(nlocal[Z] + 2*nhalo);
    MPI_Issend(sbufe + j1, n1, MPI_DOUBLE, nrank_s[0], tag0, le_comm, sreq+2);
    MPI_Issend(sbufe     , n2, MPI_DOUBLE, nrank_s[1], tag1, le_comm, sreq+3);

    MPI_Waitall(4, rreq, status);

    /* Now we've done all the communication, we can update the fluxes
     * using the average of the local value and interpolated buffer
     * value. */

    for (jc = 1; jc <= nlocal[Y]; jc++) {
      j1 = (jc - 1    )*(nlocal[Z] + 2*nhalo);
      j2 = (jc - 1 + 1)*(nlocal[Z] + 2*nhalo);
      for (kc = 1; kc <= nlocal[Z]; kc++) {
	for (n = 0; n < nf; n++) {
	  index = le_site_index(ic,jc,kc);
	  fe[addr_rank1(le_nsites(), nf, index, n)]
	    = 0.5*(fe[addr_rank1(le_nsites(), nf, index, n)]
		   + frw*rbufw[nf*(j1 + kc+nhalo-1) + n]
		   + (1.0-frw)*rbufw[nf*(j2 + kc+nhalo-1) + n]);
	  index = le_site_index(ic+1,jc,kc);
	  fw[addr_rank1(le_nsites(), nf, index, n)]
	    = 0.5*(fw[addr_rank1(le_nsites(), nf, index, n)]
		   + fre*rbufe[nf*(j1 + kc+nhalo-1) + n]
		   + (1.0-fre)*rbufe[nf*(j2 + kc+nhalo-1) + n]);
	}
      }
    }

    /* Clear the sends */
    MPI_Waitall(4, sreq, status);

    /* Next plane */
  }

  free(sbufw);
  free(sbufe);
  free(rbufw);
  free(rbufe);

  return 0;
}

/*****************************************************************************
 *
 *  phi_ch_update_forward_step
 *
 *  Update phi_site at each site in turn via the divergence of the
 *  fluxes. This is an Euler forward step:
 *
 *  phi new = phi old - dt*(flux_out - flux_in)
 *
 *  The time step is the LB time step dt = 1. All sites are processed
 *  to include solid-stored values in the case of Langmuir-Hinshelwood.
 *  It also avoids a conditional on solid/fluid status.
 *
 *****************************************************************************/

static int phi_ch_update_forward_step(field_t * phif, advflux_t * flux) {

  int nlocal[3];
  int ic, jc, kc, index;
  int ys;
  double wz = 1.0;
  double phi;

  assert(phif);
  assert(flux);

  coords_nlocal(nlocal);
  ys = nlocal[Z] + 2*coords_nhalo();

  /* In 2-d systems need to eliminate the z fluxes (no chemical
   * potential computed in halo region for 2d_5pt_fluid) */
  if (nlocal[Z] == 1) wz = 0.0;

  for (ic = 1; ic <= nlocal[X]; ic++) {
    for (jc = 1; jc <= nlocal[Y]; jc++) {
      for (kc = 1; kc <= nlocal[Z]; kc++) {

	index = coords_index(ic, jc, kc);

	field_scalar(phif, index, &phi);
	phi -= (+ flux->fe[addr_rank0(le_nsites(), index)]
		- flux->fw[addr_rank0(le_nsites(), index)]
		+ flux->fy[addr_rank0(le_nsites(), index)]
		- flux->fy[addr_rank0(le_nsites(), index - ys)]
		+ wz*flux->fz[addr_rank0(le_nsites(), index)]
		- wz*flux->fz[addr_rank0(le_nsites(), index - 1)]);

	field_scalar_set(phif, index, phi);
      }
    }
  }

  return 0;
}
