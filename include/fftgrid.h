#ifndef _fftgrid_h
#define _fftgrid_h

#include <stdio.h>
#include "typedefs.h"
#include "fftw.h"
#include "rfftw.h"
#ifdef USE_MPI
#include "mpi.h"
#include "rfftw_mpi.h"
#endif
#include "complex.h"
#include "network.h"

/* Use FFTW */

typedef t_complex t_fft_c;
typedef real      t_fft_r;

#define INDEX(i,j,k)             ((i)*la12+(j)*la2+(k))      


typedef struct {
  int local_nx,local_x_start,local_ny_after_transpose;
  int local_y_start_after_transpose,total_local_size;
} t_parfft;

typedef struct {
    t_fft_r *ptr;
    t_fft_r *localptr;
    t_fft_r *workspace;    
    int      nx,ny,nz,la2r,la2c,la12r,la12c;
  int      nptr,nxyz;
    rfftwnd_plan     plan_fw;
    rfftwnd_plan     plan_bw;
#ifdef USE_MPI
    rfftwnd_mpi_plan plan_mpi_fw;
    rfftwnd_mpi_plan plan_mpi_bw;
    t_parfft         pfft;
#endif
} t_fftgrid;

extern t_fftgrid *mk_fftgrid(FILE *fp,bool bParallel,int nx,int ny,
			     int nz,bool bOptFFT);
/* Create an FFT grid (1 Dimensional), to be indexed by the INDEX macro 
 * Setup FFTW plans and extract local sizes for the grid.
 * If the file pointer is given, information is printed to it.
 */

extern void done_fftgrid(t_fftgrid *grid);
/* And throw it away again */

extern void gmxfft3D(t_fftgrid *grid,int dir,t_commrec *cr);
/* Do the FFT, direction may be either 
 * FFTW_FORWARD (sign -1) for real -> complex transform 
 * FFTW_BACKWARD (sign 1) for complex -> real transform
 */
 
extern void clear_fftgrid(t_fftgrid *grid);
/* Set it to zero */

extern void unpack_fftgrid(t_fftgrid *grid,int *nx,int *ny,int *nz,
			   int *la2, int *la12,bool bReal, t_fft_r **ptr);

/* Get the values for the constants into local copies */




/************************************************************************
 * 
 * For backward compatibility (for testing the ewald code vs. PPPM etc)
 * some old grid routines are retained here.
 *
 ************************************************************************/
 
extern real ***mk_rgrid(int nx,int ny,int nz);

extern void free_rgrid(real ***grid,int nx,int ny);

extern real print_rgrid(FILE *fp,char *title,int nx,int ny,int nz,
			real ***grid);

extern void print_rgrid_pdb(char *fn,int nx,int ny,int nz,real ***grid);

extern t_complex ***mk_cgrid(int nx,int ny,int nz);

extern void free_cgrid(t_complex ***grid,int nx,int ny);

extern t_complex print_cgrid(FILE *fp,char *title,int nx,int ny,int nz,
			   t_complex ***grid);

extern void clear_cgrid(int nx,int ny,int nz,t_complex ***grid);

extern void clear_rgrid(int nx,int ny,int nz,real ***grid);

#endif
