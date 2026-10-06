#include "h2_fit.h"
#include "voltage_vector.h"
#include <math.h>

void H2Fit_Add(H2Fit *f, float theta, float error)
{
  if (f->invalid) return;
  if (!isfinite(theta) || !isfinite(error) ||
      (f->count && fabsf(error-f->previous_error)>3.141592654f)) {
    f->invalid=true; return;
  }
  float s,c;
  VoltageVector_SinCos(2.0f*theta,&s,&c);
  double x=c,y=s,e=error;
  f->x+=x; f->y+=y; f->xx+=x*x; f->xy+=x*y; f->yy+=y*y;
  f->e+=e; f->xe+=x*e; f->ye+=y*e;
  f->previous_error=error; f->count++;
}

bool H2Fit_Solve(const H2Fit *f, H2FitResult *result)
{
  if (!f || !result || f->invalid || f->count<3U) return false;
  /* Divide by N so the pivot threshold is independent of sample count. */
  double n=f->count;
  double m[3][4]={{1.0,f->x/n,f->y/n,f->e/n},
                  {f->x/n,f->xx/n,f->xy/n,f->xe/n},
                  {f->y/n,f->xy/n,f->yy/n,f->ye/n}};
  for (unsigned i=0;i<3;i++) for (unsigned j=0;j<4;j++)
    if (!isfinite(m[i][j])) return false;
  for (unsigned col=0;col<3;col++) {
    unsigned pivot=col;
    for (unsigned row=col+1;row<3;row++)
      if (fabs(m[row][col])>fabs(m[pivot][col])) pivot=row;
    if (fabs(m[pivot][col])<1e-8) return false;
    for (unsigned j=col;j<4;j++) {
      double tmp=m[col][j]; m[col][j]=m[pivot][j]; m[pivot][j]=tmp;
    }
    double divisor=m[col][col];
    for (unsigned j=col;j<4;j++) m[col][j]/=divisor;
    for (unsigned row=0;row<3;row++) if (row!=col) {
      double factor=m[row][col];
      for (unsigned j=col;j<4;j++) m[row][j]-=factor*m[col][j];
    }
  }
  H2FitResult fitted={(float)m[0][3],(float)m[1][3],(float)m[2][3]};
  if (!isfinite(fitted.c) || !isfinite(fitted.a) || !isfinite(fitted.b) ||
      !isfinite(hypotf(fitted.a,fitted.b))) return false;
  *result=fitted;
  return true;
}
