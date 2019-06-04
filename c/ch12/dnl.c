static const char help[] =
"Solves doubly-nonlinear obstacle problems in 2D.  Option prefix dnl_.\n"
"The PDE (interior condition) of such problems has solution u(x,y):\n"
"       - div (u^q |grad(u+b)|^{p-2} grad(u+b)) = f\n"
"subject to a obstacle constraint\n"
"       u >= psi\n"
"FIXME Includes the steady-state, nonlinear ice sheet problem in 2D in which u=H\n"
"is ice thickness,  b  is bed elevation, and  s = H + b  is surface elevation:\n"
"       - div (D grad H) - div(W H^{n+2}) = m\n"
"The diffusivity D and pseudo-velocity W (Bueler, 2016) are from the\n"
"nonsliding shallow ice approximation (SIA) flux:\n"
"       D = Gamma H^{n+2} |grad H + grad b|^{n-1}\n"
"       W = - Gamma |grad H + grad b|^{n-1} grad b\n"
"The climatic mass balance f = m(x,y,H) is from one of two models.\n"
"Constants are  n >= 1  and Gamma = 2 A (rho g)^n / (n+2)  where A is the ice\n"
"softness.  The domain is square  (0,L)^2  with zero Dirichlet boundary conditions.\n"
"The equation is discretized by a Q1 structured-grid FVE method (Bueler, 2016).\n"
"Requires SNESVI (-snes_type vinewton{rsls|ssls}) because of constraint;\n"
"defaults to SSLS.\n\n";

/*
1. shows basic success with SSLS but DIVERGES AT LEVEL 4:
   mpiexec -n 4 ./ice -ice_verif -snes_converged_reason -snes_grid_sequence LEV

2. consider making CMB model smooth

3. add CMB to dump and create plotting script (.py)

4. using exact init shows convergence depends strongly on eps for fine grids:
    for LEV in 1 2 3 4 5; do ./ice -ice_verif -ice_exact_init -snes_converged_reason -ksp_type gmres -pc_type gamg -da_refine $LEV -ice_eps EPS; done
result:
  (a) works at all levels if EPS=0.005; last KSP somewhat constant but SNES iters growing
  (b) fails on level 3 if EPS=0.003,0.002

5. convergent and nearly optimal GMG in flops *but cheating with exact init*, and *avoiding -snes_grid_sequence* and *significant eps=0.01 regularization*:
    for LEV in 1 2 3 4 5 6 7 8; do ./ice -ice_verif -ice_exact_init -snes_converged_reason -ksp_type gmres -pc_type mg -da_refine $LEV -snes_type vinewtonrsls -ice_eps 0.01; done

6. visualizing -snes_grid_sequence:
    ./ice -ice_verif -snes_grid_sequence 2 -ice_eps 0.005 -snes_converged_reason -snes_monitor_solution draw
(was -snes_grid_sequence bug with periodic BCs? see PETSc issue #300)

8. even seems to work in parallel:
    mpiexec -n 4 ./ice -ice_verif -snes_grid_sequence 5 -ice_eps 0.005 -snes_converged_reason -snes_monitor_solution draw

9. same outcome with -ice_exact_init and -da_refine 5
    mpiexec -n 4 ./ice -ice_verif -da_refine 5 -ice_eps 0.005 -snes_converged_reason -snes_monitor_solution draw -ice_exact_init

10. unpredictable response to changing -snes_linesearch_type bt|l2|basic  (cp seems rarely to work)
*/

/* see comments on runtime stuff in icet/icet.c, the time-dependent version */

#include <petsc.h>
#include "icecmb.h"

typedef struct {
    double    secpera,    // number of seconds in a year
              L,          // spatial domain is (0,L) x (0,L)
              g,          // acceleration of gravity
              rho_ice,    // ice density
              n_ice,      // Glen exponent for SIA flux term
              A_ice,      // ice softness
              Gamma,      // coefficient for SIA flux term
              D0,         // representative value of diffusivity (used in regularizing D)
              eps,        // regularization parameter for diffusivity D
              delta,      // dimensionless regularization for slope in SIA formulas
              lambda;     // amount of upwinding; lambda=0 is none and lambda=1 is "full"
    PetscBool verif,      // use dome formulas if true
              check_admissible; // check admissibility at start of FormFunctionLocal()
    CMBModel  *cmb;       // defined in cmbmodel.h
} AppCtx;


// compute radius from center of (0,L) x (0,L)
double radialcoord(double x, double y, AppCtx *user) {
  const double xc = x - user->L/2.0,
               yc = y - user->L/2.0;
  return PetscSqrtReal(xc * xc + yc * yc);
}

double DomeCMB(double x, double y, AppCtx *user) {
  const double  domeR  = 750.0e3,  // radius of exact ice sheet (m)
                domeH0 = 3600.0,   // center thickness of exact ice sheet (m)
                n  = user->n_ice,
                pp = 1.0 / n,
                CC = user->Gamma * PetscPowReal(domeH0,2.0*n+2.0)
                         / PetscPowReal(2.0 * domeR * (1.0-1.0/n),n);
  double        r, s, tmp1, tmp2;
  r = radialcoord(x, y, user);
  // avoid singularities at center and margin
  if (r < 0.01)
      r = 0.01;
  if (r > domeR - 0.01)
      r = domeR - 0.01;
  s = r / domeR;
  tmp1 = PetscPowReal(s,pp) + PetscPowReal(1.0-s,pp) - 1.0;
  tmp2 = 2.0 * PetscPowReal(s,pp) + PetscPowReal(1.0-s,pp-1.0) * (1.0 - 2.0*s) - 1.0;
  return (CC / r) * PetscPowReal(tmp1,n-1.0) * tmp2;
}


PetscErrorCode DomeThicknessLocal(DMDALocalInfo *info, double **aH, AppCtx *user) {
  const double   domeR  = 750.0e3,  // radius of exact ice sheet (m)
                 domeH0 = 3600.0,   // center thickness of exact ice sheet (m)
                 n  = user->n_ice,
                 mm = 1.0 + 1.0 / n,
                 qq = n / (2.0 * n + 2.0),
                 CC = domeH0 / PetscPowReal(1.0 - 1.0 / n,qq),
                 dx = user->L / (double)(info->mx-1),
                 dy = user->L / (double)(info->my-1);
  double         x, y, r, s, tmp;
  int            j, k;

  PetscFunctionBeginUser;
  for (k=info->ys; k<info->ys+info->ym; k++) {
      y = k * dy;
      for (j=info->xs; j<info->xs+info->xm; j++) {
          x = j * dx;
          r = radialcoord(x, y, user);
          // avoid singularities at margin and center
          if (r > domeR - 0.01)
              aH[k][j] = 0.0;
          else {
              if (r < 0.01)
                  r = 0.01;
              s = r / domeR;
              tmp = mm * s - (1.0/n) + PetscPowReal(1.0-s,mm) - PetscPowReal(s,mm);
              aH[k][j] = CC * PetscPowReal(tmp,qq);
          }
      }
  }
  PetscFunctionReturn(0);
}

extern PetscErrorCode SetFromOptionsAppCtx(AppCtx*);
extern PetscErrorCode FormBedLocal(DMDALocalInfo*, int, double**, AppCtx*);
extern PetscErrorCode FormBounds(SNES,Vec,Vec);
extern PetscErrorCode FormFunctionLocal(DMDALocalInfo*, double**, double**, AppCtx*);

int main(int argc,char **argv) {
  PetscErrorCode      ierr;
  DM                  da;
  SNES                snes;
  KSP                 ksp;
  Vec                 H;
  AppCtx              user;
  CMBModel            cmb;
  PetscBool           exact_init = PETSC_FALSE, // initialize using dome exact solution
                      dump = PETSC_FALSE;       // dump state (H,b) in binary file ice_MXxMY.dat after solve
  DMDALocalInfo       info;
  double              **aH;
  SNESConvergedReason reason;
  int                 snesit,kspit;

  PetscInitialize(&argc,&argv,(char*)0,help);

  user.secpera    = 31556926.0;  // number of seconds in a year
  user.L          = 1800.0e3;    // m; compare domeR=750.0e3 radius
  user.g          = 9.81;        // m/s^2
  user.rho_ice    = 910.0;       // kg/m^3
  user.n_ice      = 3.0;         // Glen exponent
  user.A_ice      = 3.1689e-24;  // 1/(Pa^3 s); EISMINT I value
  user.D0         = 1.0;         // m^2 / s
  user.eps        = 0.001;
  user.delta      = 1.0e-4;
  user.lambda     = 0.25;
  user.verif      = PETSC_FALSE;
  user.check_admissible = PETSC_FALSE;
  user.cmb        = NULL;

  ierr = PetscOptionsBegin(PETSC_COMM_WORLD,"ice_","options to ice","");CHKERRQ(ierr);
  ierr = PetscOptionsReal(
      "-A", "set value of ice softness A in units Pa-3 s-1",
      "ice.c",user.A_ice,&user.A_ice,NULL);CHKERRQ(ierr);
  ierr = PetscOptionsBool(
      "-check_admissible", "check admissibility of iterate at start of residual evaluation FormFunctionLocal()",
      "ice.c",user.check_admissible,&user.check_admissible,NULL);CHKERRQ(ierr);
  ierr = PetscOptionsReal(
      "-D0", "representative value of diffusivity (used in regularizing D) in units m2 s-1",
      "ice.c",user.D0,&user.D0,NULL);CHKERRQ(ierr);
  ierr = PetscOptionsReal(
      "-delta", "dimensionless regularization for slope in SIA formulas",
      "ice.c",user.delta,&user.delta,NULL);CHKERRQ(ierr);
  ierr = PetscOptionsBool(
      "-dump", "save final state (H, b)",
      "ice.c",dump,&dump,NULL);CHKERRQ(ierr);
  ierr = PetscOptionsReal(
      "-eps", "dimensionless regularization for diffusivity D",
      "ice.c",user.eps,&user.eps,NULL);CHKERRQ(ierr);
  ierr = PetscOptionsBool(
      "-exact_init", "initialize with dome exact solution",
      "ice.c",exact_init,&exact_init,NULL);CHKERRQ(ierr);
  ierr = PetscOptionsReal(
      "-L", "side length of domain in meters",
      "ice.c",user.L,&user.L,NULL);CHKERRQ(ierr);
  ierr = PetscOptionsReal(
      "-lambda", "amount of upwinding; lambda=0 is none and lambda=1 is full",
      "ice.c",user.lambda,&user.lambda,NULL);CHKERRQ(ierr);
  ierr = PetscOptionsReal(
      "-n", "value of Glen exponent n",
      "ice.c",user.n_ice,&user.n_ice,NULL);CHKERRQ(ierr);
  if (user.n_ice <= 1.0) {
      SETERRQ1(PETSC_COMM_WORLD,1,
          "ERROR: n = %f not allowed ... n > 1.0 is required\n",user.n_ice);
  }
  ierr = PetscOptionsReal(
      "-rho", "ice density in units kg m3",
      "ice.c",user.rho_ice,&user.rho_ice,NULL);CHKERRQ(ierr);
  ierr = PetscOptionsBool(
      "-verif", "use dome exact solution for verification",
      "ice.c",user.verif,&user.verif,NULL);CHKERRQ(ierr);
  ierr = PetscOptionsEnd();CHKERRQ(ierr);

  // derived constant computed after other ice properties are set
  user.Gamma = 2.0 * PetscPowReal(user.rho_ice*user.g,user.n_ice) 
                   * user.A_ice / (user.n_ice+2.0);

  ierr = SetFromOptions_CMBModel(&cmb,user.secpera);
  user.cmb = &cmb;

  // DMDA for the cell-centered grid
  ierr = DMDACreate2d(PETSC_COMM_WORLD,
                      DM_BOUNDARY_NONE,DM_BOUNDARY_NONE,
                      DMDA_STENCIL_BOX,
                      5,5, PETSC_DECIDE,PETSC_DECIDE,
                      1, 1,        // dof=1, stencilwidth=1
                      NULL,NULL,&da);
  ierr = DMSetFromOptions(da); CHKERRQ(ierr);
  ierr = DMSetUp(da); CHKERRQ(ierr);  // this must be called BEFORE SetUniformCoordinates
  ierr = DMDASetUniformCoordinates(da,0.0,user.L,0.0,user.L,-1.0,-1.0);CHKERRQ(ierr);
  ierr = DMSetApplicationContext(da, &user);CHKERRQ(ierr);

  // create and configure the SNES to solve a NCP/VI at each step
  ierr = SNESCreate(PETSC_COMM_WORLD,&snes);CHKERRQ(ierr);
  ierr = SNESSetDM(snes,da);CHKERRQ(ierr);
  ierr = SNESSetApplicationContext(snes,&user);CHKERRQ(ierr);
  ierr = DMDASNESSetFunctionLocal(da,INSERT_VALUES,
               (DMDASNESFunction)FormFunctionLocal,&user); CHKERRQ(ierr);
  ierr = SNESSetType(snes,SNESVINEWTONSSLS); CHKERRQ(ierr);
  ierr = SNESVISetComputeVariableBounds(snes,&FormBounds); CHKERRQ(ierr);
  ierr = SNESSetFromOptions(snes);CHKERRQ(ierr);

  // set up initial iterate
  ierr = DMCreateGlobalVector(da,&H);CHKERRQ(ierr);
  ierr = PetscObjectSetName((PetscObject)H,"H"); CHKERRQ(ierr);
  if (exact_init) {
      ierr = DMDAGetLocalInfo(da,&info); CHKERRQ(ierr);
      ierr = DMDAVecGetArray(da,H,&aH); CHKERRQ(ierr);
      ierr = DomeThicknessLocal(&info,aH,&user); CHKERRQ(ierr);
      ierr = DMDAVecRestoreArray(da,H,&aH); CHKERRQ(ierr);
  } else {
      ierr = VecSet(H,0.0); CHKERRQ(ierr);
  }

  // solve
  ierr = SNESSolve(snes,NULL,H); CHKERRQ(ierr);
  ierr = SNESGetConvergedReason(snes,&reason); CHKERRQ(ierr);
  if (reason <= 0) {
      ierr = PetscPrintf(PETSC_COMM_WORLD,
          "WARNING: SNES not converged ... use -snes_converged_reason to check\n"); CHKERRQ(ierr);
  }

  // get solution & DM on fine grid (which may have changed) after solve
  ierr = VecDestroy(&H); CHKERRQ(ierr);
  ierr = DMDestroy(&da); CHKERRQ(ierr);
  ierr = SNESGetDM(snes,&da); CHKERRQ(ierr); /* do not destroy da */
  ierr = DMDAGetLocalInfo(da,&info); CHKERRQ(ierr);
  ierr = SNESGetSolution(snes,&H); CHKERRQ(ierr); /* do not destroy H */
  ierr = PetscObjectSetName((PetscObject)H,"H"); CHKERRQ(ierr);

  // compute performance measures; note utility of reporting last grid,
  //   last snesit/kspit when doing -snes_grid_sequence
  ierr = SNESGetIterationNumber(snes,&snesit); CHKERRQ(ierr);  // 
  ierr = SNESGetKSP(snes,&ksp); CHKERRQ(ierr);
  ierr = KSPGetIterationNumber(ksp,&kspit); CHKERRQ(ierr);
  ierr = PetscPrintf(PETSC_COMM_WORLD,
      "done on %d x %d grid ... SNES iters = %d, last KSP iters = %d\n",
      info.mx,info.my,snesit,kspit); CHKERRQ(ierr);

  // dump state (H,b) if requested
  if (dump) {
      char           filename[1024];
      PetscViewer    viewer;
      Vec            b;
      double         **ab;
      ierr = VecDuplicate(H,&b); CHKERRQ(ierr);
      ierr = PetscObjectSetName((PetscObject)b,"b"); CHKERRQ(ierr);
      if (user.verif) {
          ierr = VecSet(b,0.0); CHKERRQ(ierr);
      } else {
          ierr = DMDAVecGetArray(da,b,&ab); CHKERRQ(ierr);
          ierr = FormBedLocal(&info,0,ab,&user); CHKERRQ(ierr);
          ierr = DMDAVecRestoreArray(da,b,&ab); CHKERRQ(ierr);
      }
      ierr = sprintf(filename,"ice_%dx%d.dat",info.mx,info.my);
      ierr = PetscPrintf(PETSC_COMM_WORLD,"writing PETSC binary file %s ...\n",filename); CHKERRQ(ierr);
      ierr = PetscViewerBinaryOpen(PETSC_COMM_WORLD,filename,FILE_MODE_WRITE,&viewer); CHKERRQ(ierr);
      ierr = VecView(b,viewer); CHKERRQ(ierr);
      ierr = VecView(H,viewer); CHKERRQ(ierr);
      ierr = PetscViewerDestroy(&viewer); CHKERRQ(ierr);
      VecDestroy(&b);
  }

  // compute error in verification case
  if (user.verif) {
      Vec Hexact;
      double infnorm, onenorm;
      ierr = VecDuplicate(H,&Hexact); CHKERRQ(ierr);
      ierr = DMDAVecGetArray(da,Hexact,&aH); CHKERRQ(ierr);
      ierr = DomeThicknessLocal(&info,aH,&user); CHKERRQ(ierr);
      ierr = DMDAVecRestoreArray(da,Hexact,&aH); CHKERRQ(ierr);
      ierr = VecAXPY(H,-1.0,Hexact); CHKERRQ(ierr);    // H <- H + (-1.0) Hexact
      VecDestroy(&Hexact);
      ierr = VecNorm(H,NORM_INFINITY,&infnorm); CHKERRQ(ierr);
      ierr = VecNorm(H,NORM_1,&onenorm); CHKERRQ(ierr);
      ierr = PetscPrintf(PETSC_COMM_WORLD,
          "numerical errors: |H-Hexact|_inf = %.3f, |H-Hexact|_average = %.3f\n",
          infnorm,onenorm/(double)(info.mx*info.my)); CHKERRQ(ierr);
  }

  SNESDestroy(&snes);
  return PetscFinalize();
}

PetscErrorCode FormBedLocal(DMDALocalInfo *info, int stencilwidth, double **ab, AppCtx *user) {
  int          j,k,r,s;
  const double dx = user->L / (double)(info->mx-1),
               dy = user->L / (double)(info->my-1),
               Z = PETSC_PI / user->L;
  double       x, y, b;
  // vaguely-random frequencies and coeffs generated by fiddling; see randbed.py
  const int    nc = 4,
               jc[4] = {1, 3, 6, 8},
               kc[4] = {1, 3, 4, 7};
  const double scalec = 750.0,
               C[4][4] = { { 2.00000000,  0.33000000, -0.55020034,  0.54495520},
                           { 0.50000000,  0.45014486,  0.60551833, -0.52250644},
                           { 0.93812068,  0.32638429, -0.24654812,  0.33887052},
                           { 0.17592361, -0.35496741,  0.22694547, -0.05280704} };
  PetscFunctionBeginUser;
  // go through owned portion of grid and compute  b(x,y)
  for (k = info->ys-stencilwidth; k < info->ys + info->ym+stencilwidth; k++) {
      y = k * dy;
      for (j = info->xs-stencilwidth; j < info->xs + info->xm+stencilwidth; j++) {
          if (j < 0 || j >= info->mx-1 || k < 0 || k >= info->my-1)
              continue;
          x = j * dx;
          // b(x,y) is sum of a few sines
          b = 0.0;
          for (r = 0; r < nc; r++) {
              for (s = 0; s < nc; s++) {
                  b += C[r][s] * sin(jc[r] * Z * x) * sin(kc[s] * Z * y);
              }
          }
          ab[k][j] = scalec * b;
      }
  }
  PetscFunctionReturn(0);
}


//  for call-back: tell SNESVI (variational inequality) that we want
//    0.0 <= H < +infinity
PetscErrorCode FormBounds(SNES snes, Vec Xl, Vec Xu) {
  PetscErrorCode ierr;
  PetscFunctionBeginUser;
  ierr = VecSet(Xl,0.0); CHKERRQ(ierr);
  ierr = VecSet(Xu,PETSC_INFINITY); CHKERRQ(ierr);
  PetscFunctionReturn(0);
}


// value of gradient at a point
typedef struct {
    double x,y;
} Grad;

/* We factor the SIA flux as
    q = - H^{n+2} sigma(|grad s|) grad s
where sigma is the slope-dependent part
    sigma(z) = Gamma z^{n-1}.
Also
    D = H^{n+2} sigma(|grad s|)
so that q = - D grad s.  */
static double sigma(Grad gH, Grad gb, const AppCtx *user) {
    const double sx = gH.x + gb.x,
                 sy = gH.y + gb.y,
                 slopesqr = sx * sx + sy * sy + user->delta * user->delta;
    return user->Gamma * PetscPowReal(slopesqr,(user->n_ice-1.0)/2);
}

/* Pseudo-velocity from bed slope:  W = - sigma * grad b. */
static Grad W(double sigma, Grad gb) {
    Grad W;
    W.x = - sigma * gb.x;
    W.y = - sigma * gb.y;
    return W;
}

/* DCS = diffusivity from the continuation scheme:
     D(eps) = (1-eps) sigma H^{n+2} + eps D_0
so D(1)=D_0 and D(0)=sigma H^{n+2}. */
static double DCS(double sigma, double H, const AppCtx *user) {
  return (1.0 - user->eps) * sigma * PetscPowReal(PetscAbsReal(H),user->n_ice+2.0)
         + user->eps * user->D0;
}

/* Flux component from the non-sliding SIA on a general bed. */
PetscErrorCode SIAflux(Grad gH, Grad gb, double H, double Hup, PetscBool xdir,
                       double *D, double *q, const AppCtx *user) {
  const double mysig = sigma(gH,gb,user),
               myD   = DCS(mysig,H,user);
  const Grad   myW   = W(mysig,gb);
  PetscFunctionBeginUser;
  if (D) {
      *D = myD;
  }
  if (xdir && q) {
      *q = - myD * gH.x + myW.x * PetscPowReal(PetscAbsReal(Hup),user->n_ice+2.0);
  } else {
      *q = - myD * gH.y + myW.y * PetscPowReal(PetscAbsReal(Hup),user->n_ice+2.0);
  }
  PetscFunctionReturn(0);
}

// gradients of weights for Q^1 interpolant
static const double gx[4] = {-1.0,  1.0, 1.0, -1.0},
                    gy[4] = {-1.0, -1.0, 1.0,  1.0};

static double fieldatpt(double xi, double eta, double f[4]) {
  // weights for Q^1 interpolant
  double x[4] = { 1.0-xi,      xi,  xi, 1.0-xi},
         y[4] = {1.0-eta, 1.0-eta, eta,    eta};
  return   x[0] * y[0] * f[0] + x[1] * y[1] * f[1]
         + x[2] * y[2] * f[2] + x[3] * y[3] * f[3];
}

static double fieldatptArray(int u, int v, double xi, double eta, double **f) {
  double ff[4] = {f[v][u], f[v][u+1], f[v+1][u+1], f[v+1][u]};
  return fieldatpt(xi,eta,ff);
}

static Grad gradfatpt(double xi, double eta, double dx, double dy, double f[4]) {
  Grad gradf;
  double x[4] = { 1.0-xi,      xi,  xi, 1.0-xi},
         y[4] = {1.0-eta, 1.0-eta, eta,    eta};
  gradf.x =   gx[0] * y[0] * f[0] + gx[1] * y[1] * f[1]
            + gx[2] * y[2] * f[2] + gx[3] * y[3] * f[3];
  gradf.y =    x[0] *gy[0] * f[0] +  x[1] *gy[1] * f[1]
            +  x[2] *gy[2] * f[2] +  x[3] *gy[3] * f[3];
  gradf.x /= dx;
  gradf.y /= dy;
  return gradf;
}

static Grad gradfatptArray(int u, int v, double xi, double eta, double dx, double dy,
                           double **f) {
  double ff[4] = {f[v][u], f[v][u+1], f[v+1][u+1], f[v+1][u]};
  return gradfatpt(xi,eta,dx,dy,ff);
}

// indexing of the 8 quadrature points along the boundary of the control volume in M*
// point s=0,...,7 is in element (j,k) = (j+je[s],k+ke[s])
static const int  je[8] = {0,  0, -1, -1, -1, -1,  0,  0},
                  ke[8] = {0,  0,  0,  0, -1, -1, -1, -1},
                  ce[8] = {0,  3,  1,  0,  2,  1,  3,  2};

// direction of flux at 4 points in each element
static const PetscBool xdire[4] = {PETSC_TRUE, PETSC_FALSE, PETSC_TRUE, PETSC_FALSE};

// local (element-wise) coords of quadrature points for M*
static const double locx[4] = {  0.5, 0.75,  0.5, 0.25},
                    locy[4] = { 0.25,  0.5, 0.75,  0.5};


/* FormFunctionLocal  =  call-back by SNES using DMDA info.

Evaluates residual FF on local process patch:
   FF_{j,k} = \int_{\partial V_{j,k}} \mathbf{q} \cdot \mathbf{n}
              - m_{j,k} \Delta x \Delta y
where V_{j,k} is the control volume centered at (x_j,y_k).

Regarding indexing locations along the boundary of the control volume where
flux is evaluated, this figure shows the control volume centered at (x_j,y_k)
and the four elements it meets.  Quadrature uses 8 points on the boundary of
the control volume, numbered s=0,...,7:

     -------------------
    |         |         |
    |    ..2..|..1..    |
    |   3:    |    :0   |
  k |--------- ---------|
    |   4:    |    :7   |
    |    ..5..|..6..    |
    |         |         |
     -------------------
              j

Regarding flux-component indexing on the element indexed by (j,k) node,
the value  (aqquad[c])[k][j] for c=0,1,2,3 is an x-component at "*" and
a y-component at "%"; note (x_j,y_k) is lower-left corner:

     -------------------
    |         :         |
    |         *2        |
    |    3    :    1    |
    |....%.... ....%....|
    |         :         |
    |         *0        |
    |         :         |
    @-------------------
  (j,k)

*/
PetscErrorCode FormFunctionLocal(DMDALocalInfo *info, double **aHin,
                                        double **FF, AppCtx *user) {
  PetscErrorCode  ierr;
  const double    dx = user->L / (double)(info->mx-1),
                  dy = user->L / (double)(info->my-1);
  // coefficients of quadrature evaluations along the boundary of the control volume in M*
  const double    coeff[8] = {dy/2, dx/2, dx/2, -dy/2, -dy/2, -dx/2, -dx/2, dy/2};
  const PetscBool upwind = (user->lambda > 0.0);
  const double    upmin = (1.0 - user->lambda) * 0.5,
                  upmax = (1.0 + user->lambda) * 0.5;
  int             c, j, k, s;
  double          H, Hup, lxup, lyup, **aqquad[4], **ab, **aH, DSIA_ckj, qSIA_ckj,
                  M, x, y;
  Grad            gH, gb;
  Vec             qquad[4], Hcopy, b;

  PetscFunctionBeginUser;

  // copy and set boundary conditions to zero
  ierr = DMGetLocalVector(info->da, &Hcopy); CHKERRQ(ierr);
  ierr = DMDAVecGetArray(info->da,Hcopy,&aH); CHKERRQ(ierr);
  for (k = info->ys-1; k <= info->ys + info->ym; k++) {
      for (j = info->xs-1; j <= info->xs + info->xm; j++) {
          if (j < 0 || j > info->mx-1 || k < 0 || k > info->my-1)
              continue;
          if (user->check_admissible && aHin[k][j] < 0.0) {
              SETERRQ3(PETSC_COMM_WORLD,1,
                       "ERROR: non-admissible value H[k][j] = %.3e < 0.0 at j,k = %d,%d\n",
                       aHin[k][j],j,k);
          }
          if (j == 0 || j == info->mx-1 || k == 0 || k == info->my-1) {
              if (j >= info->xs && j < info->xs+info->xm && k >= info->ys && k < info->ys+info->ym)
                  FF[k][j] = aHin[k][j];   // FIXME scaling?
              aH[k][j] = 0.0;
          } else
              aH[k][j] = aHin[k][j];
      }
  }

  // get bed elevation b(x,y) on this grid
  ierr = DMGetLocalVector(info->da, &b); CHKERRQ(ierr);
  ierr = DMDAVecGetArray(info->da,b,&ab); CHKERRQ(ierr);
  if (user->verif) {
      ierr = VecSet(b,0.0); CHKERRQ(ierr);
  } else {
      ierr = FormBedLocal(info,1,ab,user); CHKERRQ(ierr);  // get stencil width
  }

  // working space for fluxes; see text for face location of flux evaluation
  for (c = 0; c < 4; c++) {
      ierr = DMGetLocalVector(info->da, &(qquad[c])); CHKERRQ(ierr);
      ierr = DMDAVecGetArray(info->da,qquad[c],&(aqquad[c])); CHKERRQ(ierr);
  }

  // loop over locally-owned elements, including ghosts, to get fluxes q at
  // c = 0,1,2,3 points in element;  note start at (xs-1,ys-1)
  for (k = info->ys-1; k < info->ys + info->ym; k++) {
      for (j = info->xs-1; j < info->xs + info->xm; j++) {
          if (j < 0 || j >= info->mx-1 || k < 0 || k >= info->my-1)
              continue;
          for (c=0; c<4; c++) {
              H  = fieldatptArray(j,k,locx[c],locy[c],aH);
              gH = gradfatptArray(j,k,locx[c],locy[c],dx,dy,aH);
              gb = gradfatptArray(j,k,locx[c],locy[c],dx,dy,ab);
              if (upwind) {
                  if (xdire[c] == PETSC_TRUE) {
                      lxup = (gb.x <= 0.0) ? upmin : upmax;
                      lyup = locy[c];
                  } else {
                      lxup = locx[c];
                      lyup = (gb.y <= 0.0) ? upmin : upmax;
                  }
                  Hup = fieldatptArray(j,k,lxup,lyup,aH);
              } else
                  Hup = H;
              ierr = SIAflux(gH,gb,H,Hup,xdire[c],
                             &DSIA_ckj,&qSIA_ckj,user); CHKERRQ(ierr);
              aqquad[c][k][j] = qSIA_ckj;
          }
      }
  }

  // loop over nodes, not including ghosts, to get function F(H) from quadature over
  // s = 0,1,...,7 points on boundary of control volume (rectangle) around node
  for (k=info->ys; k<info->ys+info->ym; k++) {
      for (j=info->xs; j<info->xs+info->xm; j++) {
          if (j == 0 || j == info->mx-1 || k == 0 || k == info->my-1)
              continue;
          // climatic mass balance
          if (user->verif) {
              x = j * dx;
              y = k * dy;
              M = DomeCMB(x,y,user);
          } else {
              M = M_CMBModel(user->cmb,ab[k][j] + aH[k][j]);  // s=b+H is surface elevation
          }
          FF[k][j] = - M * dx * dy;
          // now add integral over control volume boundary using two
          // quadrature points on each side
          for (s=0; s<8; s++)
              FF[k][j] += coeff[s] * aqquad[ce[s]][k+ke[s]][j+je[s]];
      }
  }


  // restore working space and bed
  for (c = 0; c < 4; c++) {
      ierr = DMDAVecRestoreArray(info->da,qquad[c],&(aqquad[c])); CHKERRQ(ierr);
      ierr = DMRestoreLocalVector(info->da, &(qquad[c])); CHKERRQ(ierr);
  }
  ierr = DMDAVecRestoreArray(info->da,Hcopy,&aH); CHKERRQ(ierr);
  ierr = DMRestoreLocalVector(info->da, &Hcopy); CHKERRQ(ierr);
  ierr = DMDAVecRestoreArray(info->da,b,&ab); CHKERRQ(ierr);
  ierr = DMRestoreLocalVector(info->da, &b); CHKERRQ(ierr);
  PetscFunctionReturn(0);
}
