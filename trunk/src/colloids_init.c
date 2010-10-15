/*****************************************************************************
 *
 *  colloids_init.c
 *
 *  A very simple initialisation routine which can be used for
 *  small numbers of particles, which are placed at random.
 *  If there are any collisions in the result, a fatal error
 *  is issued.
 *
 *  Anything more complex should be organised separately and
 *  initialised from file.
 *
 *  $Id: colloids_init.c,v 1.2 2010-10-15 12:40:02 kevin Exp $
 *
 *  Edinburgh Soft Matter and Statistical Physics Group and
 *  Edinburgh Parallel Computing Centre
 *
 *  Kevin Stratford (kevin@epcc.ed.ac.uk)
 *  (c) 2010 The University of Edinburgh
 *
 *****************************************************************************/

#include <math.h>
#include <assert.h>

#include "pe.h"
#include "ran.h"
#include "coords.h"
#include "colloids.h"
#include "colloids_halo.h"
#include "colloids_init.h"

static void colloids_init_check_state(double hmax);
static void colloids_init_random_set(int n, double a0, double ah, double amax);

/*****************************************************************************
 *
 *  colloids_init_random
 *
 *  Run the initialisation with a total of np particles.
 *
 *****************************************************************************/

void colloids_init_random(int np, double a0, double ah, double dh) {

  double amax;
  double hmax;

  /* Assume maximum size set by ah and small separation */
  amax = ah + 0.5*dh;
  hmax = 2.0*ah + dh;

  colloids_init_random_set(np, a0, ah, amax);
  colloids_halo_state();
  colloids_init_check_state(hmax);

  colloids_ntotal_set();

  return;
}

/*****************************************************************************
 *
 *  colloids_init_random_set
 *
 *  Initialise a fixed number of particles in random positions.
 *  This is serial, and does not prevent collisions.
 *
 *****************************************************************************/

static void colloids_init_random_set(int npart, double a0, double ah,
				     double amax) {
  int n;
  double r0[3];
  double lex[3];
  colloid_t * pc;

  /* If boundaries are not perioidic, some of the volume must be excluded */
  lex[X] = amax*(1.0 - is_periodic(X));
  lex[Y] = amax*(1.0 - is_periodic(Y));
  lex[Z] = amax*(1.0 - is_periodic(Z));

  for (n = 1; n <= npart; n++) {
    r0[X] = Lmin(X) + lex[X] + ran_serial_uniform()*(L(X) - 2.0*lex[X]);
    r0[Y] = Lmin(Y) + lex[Y] + ran_serial_uniform()*(L(Y) - 2.0*lex[Y]);
    r0[Z] = Lmin(Z) + lex[Z] + ran_serial_uniform()*(L(Z) - 2.0*lex[Z]);
    pc = colloid_add_local(n, r0);
    if (pc) {
      pc->s.a0 = a0;
      pc->s.ah = ah;
    }
  }

  return;
}

/*****************************************************************************
 *
 *  colloids_init_check_state
 *
 *  Check there are no hard sphere overlaps with centre-centre
 *  separation < dhmax.
 *
 *****************************************************************************/

static void colloids_init_check_state(double hmax) {

  int noverlap_local;
  int noverlap;
  int ic, jc, kc, id, jd, kd, dx, dy, dz;
  double hh;
  double r12[3];

  colloid_t * p_c1;
  colloid_t * p_c2;

  noverlap_local = 0;

  for (ic = 1; ic <= Ncell(X); ic++) {
    for (jc = 1; jc <= Ncell(Y); jc++) {
      for (kc = 1; kc <= Ncell(Z); kc++) {

	p_c1 = colloids_cell_list(ic, jc, kc);

	while (p_c1) {
	  for (dx = -1; dx <= +1; dx++) {
	    for (dy = -1; dy <= +1; dy++) {
	      for (dz = -1; dz <= +1; dz++) {

		id = ic + dx;
		jd = jc + dy;
		kd = kc + dz;
		p_c2 = colloids_cell_list(id, jd, kd);

		while (p_c2) {
		  if (p_c2 != p_c1) {
		    coords_minimum_distance(p_c1->s.r, p_c2->s.r, r12);
		    hh = r12[X]*r12[X] + r12[Y]*r12[Y] + r12[Z]*r12[Z];
		    if (hh < hmax*hmax) noverlap_local += 1;
		  }
		  /* Next colloid c2 */
		  p_c2 = p_c2->next;
		}
		/* Next search cell */
	      }
	    }
	  }
	  /* Next colloid c1 */
	  p_c1 = p_c1->next;
	}
	/* Next cell */
      }
    }
  }

  MPI_Allreduce(&noverlap_local, &noverlap, 1, MPI_INT, MPI_SUM, pe_comm());

  if (noverlap > 0) {
    info("This appears to include at least one hard sphere overlap.\n");
    info("Please check the colloid parameters and try again\n");
    fatal("Stop.\n");
  }

  return;
}
