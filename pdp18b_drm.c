/* pdp18b_drm.c: drum/fixed head disk simulator

   Copyright (c) 1993-2000, Robert M Supnik

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
   ROBERT M SUPNIK BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
   IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

   Except as contained in this notice, the name of Robert M Supnik shall not
   be used in advertising or otherwise to promote the sale, use or other dealings
   in this Software without prior written authorization from Robert M Supnik.

   drm		(PDP-7) Type 24 serial drum
		(PDP-9) RM09 serial drum

   14-Apr-99	RMS	Changed t_addr to unsigned
*/

#include "pdp18b_defs.h"
#include <math.h>

/* Constants */

#define DRM_NUMWDS	256				/* words/sector */
#define DRM_NUMSC	2				/* sectors/track */
#define DRM_NUMTR	256				/* tracks/drum */
#define DRM_NUMDK	1				/* drum/controller */
#define DRM_NUMWDT	(DRM_NUMWDS * DRM_NUMSC)	/* words/track */
#define DRM_SIZE	(DRM_NUMDK * DRM_NUMTR * DRM_NUMWDT) /* words/drum */
#define DRM_SMASK	((DRM_NUMTR * DRM_NUMSC) - 1)	/* sector mask */

/* Parameters in the unit descriptor */

#define FUNC		u4				/* function */
#define DRM_READ	000				/* read */
#define DRM_WRITE	040				/* write */

#define GET_POS(x)	((int) fmod (sim_gtime() / ((double) (x)), \
			((double) DRM_NUMWDT)))

extern int32 M[];
extern int32 int_req;
extern UNIT cpu_unit;
int32 drm_da = 0;					/* track address */
int32 drm_ma = 0;					/* memory address */
int32 drm_err = 0;					/* error flag */
int32 drm_wlk = 0;					/* write lock */
int32 drm_time = 10;					/* inter-word time */
int32 drm_stopioe = 1;					/* stop on error */
t_stat drm_svc (UNIT *uptr);
t_stat drm_reset (DEVICE *dptr);
t_stat drm_boot (int32 unitno);
extern t_stat sim_activate (UNIT *uptr, int32 delay);
extern t_stat sim_cancel (UNIT *uptr);

/* DRM data structures

   drm_dev	DRM device descriptor
   drm_unit	DRM unit descriptor
   drm_reg	DRM register list
*/

UNIT drm_unit =
	{ UDATA (&drm_svc, UNIT_FIX+UNIT_ATTABLE+UNIT_BUFABLE+UNIT_MUSTBUF,
	DRM_SIZE) };

REG drm_reg[] = {
	{ ORDATA (DA, drm_da, 9) },
	{ ORDATA (MA, drm_ma, 15) },
	{ FLDATA (INT, int_req, INT_V_DRM) },
	{ FLDATA (DONE, int_req, INT_V_DRM) },
	{ FLDATA (ERR, drm_err, 0) },
	{ ORDATA (WLK, drm_wlk, 32) },
	{ DRDATA (TIME, drm_time, 24), REG_NZ + PV_LEFT },
	{ FLDATA (STOP_IOE, drm_stopioe, 0) },
	{ NULL }  };

DEVICE drm_dev = {
	"DRM", &drm_unit, drm_reg, NULL,
	1, 8, 20, 1, 8, 18,
	NULL, NULL, &drm_reset,
	&drm_boot, NULL, NULL };

/* IOT routines */

int32 drm60 (int32 pulse, int32 AC)
{
if ((pulse & 027) == 06) {				/* DRLR, DRLW */
	drm_ma = AC & ADDRMASK;				/* load mem addr */
	drm_unit.FUNC = pulse & 040;  }			/* save function */
return AC;
}

int32 drm61 (int32 pulse, int32 AC)
{
int32 t;

if (pulse == 001) return (int_req & INT_DRM)? IOT_SKP + AC: AC; /* DRSF */
if (pulse == 002) {					/* DRCF */
	int_req = int_req & ~INT_DRM;			/* clear done */
	drm_err = 0;  }					/* clear error */
if (pulse == 006) {					/* DRSS */
	drm_da = AC & DRM_SMASK;			/* load sector # */
	int_req = int_req & ~INT_DRM;			/* clear done */
	drm_err = 0;					/* clear error */
	t = ((drm_da % DRM_NUMSC) * DRM_NUMWDS) - GET_POS (drm_time);
	if (t < 0) t = t + DRM_NUMWDT;			/* wrap around? */
	sim_activate (&drm_unit, t * drm_time);  }	/* schedule op */
return AC;
}

int32 drm62 (int32 pulse, int32 AC)
{
int32 t;

if (pulse == 001) return (drm_err)? AC: IOT_SKP + AC;	/* DRSN */
if (pulse == 004) {					/* DRCS */
	int_req = int_req & ~INT_DRM;			/* clear done */
	drm_err = 0;					/* clear error */
	t = ((drm_da % DRM_NUMSC) * DRM_NUMWDS) - GET_POS (drm_time);
	if (t < 0) t = t + DRM_NUMWDT;			/* wrap around? */
	sim_activate (&drm_unit, t * drm_time);  }	/* schedule op */
return AC;
}

/* Unit service

   This code assumes the entire drum is buffered.
*/

t_stat drm_svc (UNIT *uptr)
{
int32 i;
t_addr da;

if ((uptr -> flags & UNIT_BUF) == 0) {			/* not buf? abort */
	drm_err = 1;					/* set error */
	int_req = int_req | INT_DRM;			/* set done */
	return IORETURN (drm_stopioe, SCPE_UNATT);  }

da = drm_da * DRM_NUMWDS;				/* compute dev addr */
for (i = 0; i < DRM_NUMWDS; i++, da++) {		/* do transfer */
	if (uptr -> FUNC == DRM_READ) {
		if (MEM_ADDR_OK (drm_ma))		/* read, check nxm */
			M[drm_ma] = *(((int32 *) uptr -> filebuf) + da);  }
	else {	if ((drm_wlk >> (drm_da >> 4)) & 1) drm_err = 1;
		else {	*(((int32 *) uptr -> filebuf) + da) = M[drm_ma];
			if (da >= uptr -> hwmark)
				uptr -> hwmark = da + 1;  }  }
	drm_ma = (drm_ma + 1) & ADDRMASK;  }		/* incr mem addr */
drm_da = (drm_da + 1) & DRM_SMASK;			/* incr dev addr */
int_req = int_req | INT_DRM;				/* set done */
return SCPE_OK;
}

/* Reset routine */

t_stat drm_reset (DEVICE *dptr)
{
drm_ma = drm_ma = drm_err = 0;
int_req = int_req & ~INT_DRM;
sim_cancel (&drm_unit);
return SCPE_OK;
}

/* IORS routine */

int32 drm_iors (void)
{
return ((int_req & INT_DRM)? IOS_DRM: 0);
}

/* Bootstrap routine */

#define BOOT_START 02000
#define BOOT_LEN (sizeof (boot_rom) / sizeof (int))

static const int32 boot_rom[] = {
	0750000,		/* CLA			; dev, mem addr */
	0706006,		/* DRLR			; load ma */
	0706106,		/* DRSS			; load da, start */
	0706101,		/* DRSF			; wait for done */
	0602003,		/* JMP .-1
	0600000			/* JMP 0		; enter boot */
};

t_stat drm_boot (int32 unitno)
{
int32 i;
extern int32 saved_PC;

for (i = 0; i < BOOT_LEN; i++) M[BOOT_START + i] = boot_rom[i];
saved_PC = BOOT_START;
return SCPE_OK;
}
