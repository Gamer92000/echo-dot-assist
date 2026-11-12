/* Native recording-level temporal training. No vendor assets or audio in source.
 * 25 lanes, ages9..23, 3-age probability mean; complete recordings are examples.
 * Float BFGS initialization, then exact int8 forward / STE / Adam. TRAIN loss
 * alone selects checkpoints; caller must freeze before reading held-out files.
 */
#include "pryon_qat.h"
#include <math.h>
#include <stdint.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* exp/log/log1p exactly as the Echo's bionic libm computes them (FreeBSD msun
 * lineage), so training reproduces the device bit for bit. glibc's versions
 * differ from the device's in the last bit (7.5% of exp results), and the
 * optimizer amplifies that. Verified against 240,006 inputs captured from the
 * real Echo and from the firmware's libm.so under QEMU; see docs/re-pryon-training.md.
 * Same operation order as msun; build with -ffp-contract=off.
 *
 * Derived from FreeBSD msun e_exp.c, e_log.c and s_log1p.c:
 *   Copyright (C) 2004 by Sun Microsystems, Inc. All rights reserved.
 *   Permission to use, copy, modify, and distribute this software is freely
 *   granted, provided that this notice is preserved. */
static inline int32_t pq_hi(double x) { uint64_t b; memcpy(&b, &x, 8); return (int32_t)(b >> 32); }
static inline uint32_t pq_lo(double x) { uint64_t b; memcpy(&b, &x, 8); return (uint32_t)b; }
static inline double pq_words(int32_t hi, uint32_t lo) { uint64_t b = ((uint64_t)(uint32_t)hi << 32) | lo; double d; memcpy(&d, &b, 8); return d; }
static inline double pq_with_hi(double x, int32_t hi) { return pq_words(hi, pq_lo(x)); }

double pq_exp(double x)
{
    static const double halF[2] = {0.5, -0.5}, o_threshold = 7.09782712893383973096e+02,
        u_threshold = -7.45133219101941108420e+02,
        ln2HI[2] = {6.93147180369123816490e-01, -6.93147180369123816490e-01},
        ln2LO[2] = {1.90821492927058770002e-10, -1.90821492927058770002e-10},
        invln2 = 1.44269504088896338700e+00,
        P1 = 1.66666666666666019037e-01, P2 = -2.77777777770155933842e-03,
        P3 = 6.61375632143793436117e-05, P4 = -1.65339022054652515390e-06,
        P5 = 4.13813679705723846039e-08, twom1000 = 9.33263618503218878990e-302;
    double y, hi = 0.0, lo = 0.0, c, t, twopk;
    int32_t k = 0, xsb; uint32_t hx;
    hx = (uint32_t)pq_hi(x); xsb = (int32_t)((hx >> 31) & 1); hx &= 0x7fffffff;
    if (hx >= 0x40862E42) {
        if (hx >= 0x7ff00000) {
            if (((hx & 0xfffff) | pq_lo(x)) != 0) return x + x;
            return xsb == 0 ? x : 0.0;
        }
        if (x > o_threshold) return INFINITY;
        if (x < u_threshold) return 0.0;
    }
    if (hx > 0x3fd62e42) {
        if (hx < 0x3FF0A2B2) { hi = x - ln2HI[xsb]; lo = ln2LO[xsb]; k = 1 - xsb - xsb; }
        else { k = (int)(invln2 * x + halF[xsb]); t = k; hi = x - t * ln2HI[0]; lo = t * ln2LO[0]; }
        x = hi - lo;
    } else if (hx < 0x3e300000) {
        return 1.0 + x;
    } else k = 0;
    t = x * x;
    if (k >= -1021) twopk = pq_words((int32_t)(0x3ff00000u + ((uint32_t)k << 20)), 0);
    else twopk = pq_words((int32_t)(0x3ff00000u + ((uint32_t)(k + 1000) << 20)), 0);
    c = x - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
    if (k == 0) return 1.0 - ((x * c) / (c - 2.0) - x);
    y = 1.0 - ((lo - (x * c) / (2.0 - c)) - hi);
    if (k >= -1021) {
        if (k == 1024) return y * 2.0 * 8.98846567431158e307; /* 0x1p1023 */
        return y * twopk;
    }
    return y * twopk * twom1000;
}

double pq_log(double x)
{
    static const double ln2_hi = 6.93147180369123816490e-01, ln2_lo = 1.90821492927058770002e-10,
        two54 = 1.80143985094819840000e+16,
        Lg1 = 6.666666666666735130e-01, Lg2 = 3.999999999940941908e-01, Lg3 = 2.857142874366239149e-01,
        Lg4 = 2.222219843214978396e-01, Lg5 = 1.818357216161805012e-01, Lg6 = 1.531383769920937332e-01,
        Lg7 = 1.479819860511658591e-01;
    double hfsq, f, s, z, R, w, t1, t2, dk; int32_t k = 0, hx, i, j; uint32_t lx;
    hx = pq_hi(x); lx = pq_lo(x);
    if (hx < 0x00100000) {
        if (((hx & 0x7fffffff) | (int32_t)lx) == 0) return -INFINITY;
        if (hx < 0) return NAN;
        k -= 54; x *= two54; hx = pq_hi(x);
    }
    if (hx >= 0x7ff00000) return x + x;
    k += (hx >> 20) - 1023; hx &= 0x000fffff;
    i = (hx + 0x95f64) & 0x100000;
    x = pq_with_hi(x, hx | (i ^ 0x3ff00000));
    k += (i >> 20); f = x - 1.0;
    if ((0x000fffff & (2 + hx)) < 3) {
        if (f == 0.0) { if (k == 0) return 0.0; dk = (double)k; return dk * ln2_hi + dk * ln2_lo; }
        R = f * f * (0.5 - 0.33333333333333333 * f);
        if (k == 0) return f - R;
        dk = (double)k; return dk * ln2_hi - ((R - dk * ln2_lo) - f);
    }
    s = f / (2.0 + f); dk = (double)k; z = s * s; i = hx - 0x6147a; w = z * z; j = 0x6b851 - hx;
    t1 = w * (Lg2 + w * (Lg4 + w * Lg6)); t2 = z * (Lg1 + w * (Lg3 + w * (Lg5 + w * Lg7)));
    i |= j; R = t2 + t1;
    if (i > 0) {
        hfsq = 0.5 * f * f;
        if (k == 0) return f - (hfsq - s * (hfsq + R));
        return dk * ln2_hi - ((hfsq - (s * (hfsq + R) + dk * ln2_lo)) - f);
    }
    if (k == 0) return f - s * (f - R);
    return dk * ln2_hi - ((s * (f - R) - dk * ln2_lo) - f);
}

double pq_log1p(double x)
{
    static const double ln2_hi = 6.93147180369123816490e-01, ln2_lo = 1.90821492927058770002e-10,
        two54 = 1.80143985094819840000e+16,
        Lp1 = 6.666666666666735130e-01, Lp2 = 3.999999999940941908e-01, Lp3 = 2.857142874366239149e-01,
        Lp4 = 2.222219843214978396e-01, Lp5 = 1.818357216161805012e-01, Lp6 = 1.531383769920937332e-01,
        Lp7 = 1.479819860511658591e-01;
    double hfsq, f = 0, c = 0, s, z, R, u; int32_t k, hx, hu = 0, ax;
    hx = pq_hi(x); ax = hx & 0x7fffffff;
    k = 1;
    if (hx < 0x3FDA827A) {
        if (ax >= 0x3ff00000) { if (x == -1.0) return -INFINITY; return NAN; }
        if (ax < 0x3e200000) {
            if (two54 + x > 0.0 && ax < 0x3c900000) return x;
            return x - x * x * 0.5;
        }
        if (hx > 0 || hx <= (int32_t)0xbfd2bec4) { k = 0; f = x; hu = 1; }
    }
    if (hx >= 0x7ff00000) return x + x;
    if (k != 0) {
        if (hx < 0x43400000) {
            u = 1.0 + x; hu = pq_hi(u);
            k = (hu >> 20) - 1023;
            c = (k > 0) ? 1.0 - (u - x) : x - (u - 1.0);
            c /= u;
        } else { u = x; hu = pq_hi(u); k = (hu >> 20) - 1023; c = 0; }
        hu &= 0x000fffff;
        if (hu < 0x6a09e) u = pq_with_hi(u, hu | 0x3ff00000);
        else { k += 1; u = pq_with_hi(u, hu | 0x3fe00000); hu = (0x00100000 - hu) >> 2; }
        f = u - 1.0;
    }
    hfsq = 0.5 * f * f;
    if (hu == 0) {
        if (f == 0.0) { if (k == 0) return 0.0; c += k * ln2_lo; return k * ln2_hi + c; }
        R = hfsq * (1.0 - 0.66666666666666666 * f);
        if (k == 0) return f - R;
        return k * ln2_hi - ((R - (k * ln2_lo + c)) - f);
    }
    s = f / (2.0 + f); z = s * s;
    R = z * (Lp1 + z * (Lp2 + z * (Lp3 + z * (Lp4 + z * (Lp5 + z * (Lp6 + z * Lp7))))));
    if (k == 0) return f - (hfsq - s * (hfsq + R));
    return k * ln2_hi - ((hfsq - (s * (hfsq + R) + (k * ln2_lo + c))) - f);
}

static double sigmoid(double x) { if (x >= 0) return 1/(1+pq_exp(-x)); double a=pq_exp(x); return a/(1+a); }
static double ladd(double a, double b) { double m=fmax(a,b); return m+pq_log(pq_exp(a-m)+pq_exp(b-m)); }
static int lane(int t, int age, int j) { return t*25+(t-age-j+1+50)%25; }

void pq_quantize(const float *w, int8_t *q, float *scale, int32_t *zero, int32_t *sum)
{
    double lo=0,hi=0;
    for(int k=0;k<PQ_K;k++){lo=fmin(lo,w[k]);hi=fmax(hi,w[k]);}
    double s=fmax((hi-lo)/255.,1e-8),z=fmax(-128,fmin(127,nearbyint(-128-lo/s)));
    *scale=(float)s;*zero=(int32_t)z;*sum=0;
    for(int k=0;k<PQ_K;k++){q[k]=(int8_t)fmax(-128,fmin(127,nearbyint(w[k]/s+z)));*sum+=q[k];}
}

float pq_logit(const struct pq_clip *c,int r,const int8_t *w,float sw,int32_t zw,float bias)
{
    if(c->constant[r]){
        double a=0;for(int k=0;k<PQ_K;k++)a+=(double)c->x[r*PQ_K+k]*((w[k]-zw)*(double)sw);
        return (float)a+bias;
    }
    int64_t a=0;for(int k=0;k<PQ_K;k++)a+=(int64_t)c->q[r*PQ_K+k]*(w[k]-zw);
    float factor=c->scale[r]*sw;
    return (float)a*factor+bias;
}

int pq_prepare(struct pq_clip *c,const struct pq_head *h)
{
    if(c->cycles<=30||c->cycles>4096||!c->x)return -1;
    size_t rows=(size_t)c->cycles*25;
    c->q=calloc(rows*PQ_K,sizeof *c->q);c->scale=calloc(rows,sizeof *c->scale);
    c->constant=calloc(rows,1);c->base_float=calloc(rows,sizeof(double));c->base_quant=calloc(rows,sizeof(double));
    if(!c->q||!c->scale||!c->constant||!c->base_float||!c->base_quant)return -1;
    /* Native startup rows may be NaN; none is used by temporal_windows(t>=30). */
    for(size_t r=30*25;r<rows;r++){
        float lo=c->x[r*PQ_K],hi=lo;
        for(int k=0;k<PQ_K;k++){
            float x=c->x[r*PQ_K+k];if(!isfinite(x))return -1;lo=fminf(lo,x);hi=fmaxf(hi,x);
        }
        float s=(hi-lo)/127.f;c->constant[r]=(s==0);if(s==0)s=1;c->scale[r]=s;
        float inv=1.f/s,z=nearbyintf(-lo*inv);
        for(int k=0;k<PQ_K;k++){
            float v=fminf(nearbyintf(c->x[r*PQ_K+k]*inv)+z,127.f)-z;
            if(!isfinite(v)||fabsf(v)>INT32_MAX/2)return -1;c->q[r*PQ_K+k]=(int32_t)v;
        }
        double base[2];
        for(int j=0;j<2;j++){
            int cls=j?2:0;double a=h->bias[cls];
            for(int k=0;k<PQ_K;k++)a+=(double)c->x[r*PQ_K+k]*(h->q[cls][k]-h->zero[cls])*(double)h->scale[cls];
            base[j]=a;
        }
        c->base_float[r]=ladd(base[0],base[1]);
        c->base_quant[r]=ladd(pq_logit(c,(int)r,h->q[0],h->scale[0],h->zero[0],h->bias[0]),
                                    pq_logit(c,(int)r,h->q[2],h->scale[2],h->zero[2],h->bias[2]));
    }
    return 0;
}

void pq_release(struct pq_clip *c)
{free(c->x);free(c->q);free(c->scale);free(c->constant);free(c->base_float);free(c->base_quant);memset(c,0,sizeof *c);}

int pq_normalize(struct pq_fit *f,struct pq_clip *c,int n)
{
    double sums[PQ_K]={0},sq[PQ_K]={0};size_t count=0;
    for(int i=0;i<n;i++)for(int t=30;t<c[i].cycles;t++)for(int age=9;age<=23;age++)for(int j=0;j<3;j++){
        const float *x=c[i].x+lane(t,age,j)*PQ_K;count++;
        for(int k=0;k<PQ_K;k++){sums[k]+=x[k];sq[k]+=(double)x[k]*x[k];}
    }
    if(!count)return -1;
    for(int k=0;k<PQ_K;k++){f->mean[k]=sums[k]/count;f->std[k]=fmax(sqrt(fmax(0,sq[k]/count-f->mean[k]*f->mean[k])),1e-3);}
    return 0;
}

/* Gradient features are computed from unique ring rows, not expanded windows.
 * This avoids the desktop implementation's duplicated [cycles,15,3,100] arrays. */
double pq_evaluate(const struct pq_fit *f,struct pq_clip *c,int n,const int *ids,int m,
                   const double *theta,int quantized,double *gradient)
{
    double w[PQ_K],b=theta[PQ_K];float wf[PQ_K];
    for(int k=0;k<PQ_K;k++){w[k]=theta[k]/f->std[k];wf[k]=(float)w[k];b-=f->mean[k]*w[k];}
    int8_t qw[PQ_K];float sw;int32_t zw,sum;pq_quantize(wf,qw,&sw,&zw,&sum);
    if(gradient)memset(gradient,0,PQ_D*sizeof(double));
    double loss=0;
    for(int item=0;item<m;item++){
        int index=ids?ids[item]:item;if(index<0||index>=n)return INFINITY;
        struct pq_clip *a=c+index;int rows=a->cycles*25;
        double *p=calloc((size_t)rows*2,sizeof(double));if(!p)return INFINITY;
        double *d=p+rows;
        for(int r=30*25;r<rows;r++){
            double logit=b;
            if(quantized)logit=pq_logit(a,r,qw,sw,zw,(float)b);
            else for(int k=0;k<PQ_K;k++)logit+=(double)a->x[r*PQ_K+k]*w[k];
            p[r]=sigmoid(logit-(quantized?a->base_quant[r]:a->base_float[r]));
        }
        double peak=-1;int peak_t=0,peak_age=0,windows=(a->cycles-30)*15;
        for(int t=30;t<a->cycles;t++)for(int age=9;age<=23;age++){
            double s=0;for(int j=0;j<3;j++)s+=p[lane(t,age,j)]/3.;
            if(s>peak){peak=s;peak_t=t;peak_age=age;}
        }
        int hard=quantized&&!a->label;double normal=0,score=peak;
        if(!hard){
            for(int t=30;t<a->cycles;t++)for(int age=9;age<=23;age++){
                double s=0;for(int j=0;j<3;j++)s+=p[lane(t,age,j)]/3.;normal+=pq_exp((s-peak)/.02);
            }
            score=peak+.02*(pq_log(normal)-pq_log(windows));
        }
        score=fmax(1e-12,fmin(1-1e-12,score));
        loss+=(-a->label*pq_log(score)-(1-a->label)*pq_log1p(-score))/m;
        if(gradient){
            double derivative=(score-a->label)/(score*(1-score)*m);
            for(int t=30;t<a->cycles;t++)for(int age=9;age<=23;age++){
                if(hard&&(t!=peak_t||age!=peak_age))continue;
                double alpha=1;
                if(!hard){double s=0;for(int j=0;j<3;j++)s+=p[lane(t,age,j)]/3.;alpha=pq_exp((s-peak)/.02)/normal;}
                for(int j=0;j<3;j++)d[lane(t,age,j)]+=derivative*alpha/3.;
            }
            for(int r=30*25;r<rows;r++)if(d[r]){
                double coeff=d[r]*p[r]*(1-p[r]);gradient[PQ_K]+=coeff;
                for(int k=0;k<PQ_K;k++){
                    double x=quantized&&!a->constant[r]?(double)a->q[r*PQ_K+k]*a->scale[r]:a->x[r*PQ_K+k];
                    gradient[k]+=coeff*(x-f->mean[k])/f->std[k];
                }
            }
        }
        free(p);
    }
    for(int k=0;k<PQ_K;k++){double delta=theta[k]-f->prior[k];loss+=.5e-5*delta*delta;if(gradient)gradient[k]+=1e-5*delta;}
    return loss;
}

static double dot(const double *a,const double *b)
{double s=0;for(int k=0;k<PQ_D;k++)s+=a[k]*b[k];return s;}
static uint32_t random32(uint32_t *s){*s^=*s<<13;*s^=*s>>17;*s^=*s<<5;return *s;}

int pq_train(struct pq_fit *f,struct pq_clip *c,int n,int steps,float *outw,float *outb,double *best_loss)
{
    if(n<2||steps<1||steps>100000)return -1;
    double theta[PQ_D],g[PQ_D],trial[PQ_D],ng[PQ_D];
    const struct pq_head *h=f->head;theta[PQ_K]=h->bias[1];
    for(int k=0;k<PQ_K;k++){double w=(h->q[1][k]-h->zero[1])*(double)h->scale[1];theta[k]=w*f->std[k];theta[PQ_K]+=w*f->mean[k];}
    memcpy(f->prior,theta,sizeof theta);
    double *H=calloc(PQ_D*PQ_D,sizeof(double));if(!H)return -1;
    for(int k=0;k<PQ_D;k++)H[k*PQ_D+k]=1;
    double loss=pq_evaluate(f,c,n,NULL,n,theta,0,g);
    for(int it=0;it<1400;it++){
        double direction[PQ_D]={0};
        for(int k=0;k<PQ_D;k++)for(int j=0;j<PQ_D;j++)direction[k]-=H[k*PQ_D+j]*g[j];
        double slope=dot(direction,g);
        if(!isfinite(loss)||!isfinite(slope)){free(H);return -1;}
        if(dot(g,g)<1e-10)break;
        if(slope>=0){memset(H,0,PQ_D*PQ_D*sizeof(double));for(int k=0;k<PQ_D;k++){H[k*PQ_D+k]=1;direction[k]=-g[k];}slope=-dot(g,g);}
        double alpha=1,next=INFINITY;
        for(int line=0;line<35;line++){
            for(int k=0;k<PQ_D;k++)trial[k]=theta[k]+alpha*direction[k];
            next=pq_evaluate(f,c,n,NULL,n,trial,0,NULL);
            if(isfinite(next)&&next<=loss+1e-4*alpha*slope)break;alpha*=.5;
        }
        if(!isfinite(next)||next>loss+1e-4*alpha*slope)break;
        pq_evaluate(f,c,n,NULL,n,trial,0,ng);
        double s[PQ_D],y[PQ_D],hy[PQ_D]={0};
        for(int k=0;k<PQ_D;k++){s[k]=trial[k]-theta[k];y[k]=ng[k]-g[k];}
        for(int k=0;k<PQ_D;k++)for(int j=0;j<PQ_D;j++)hy[k]+=H[k*PQ_D+j]*y[j];
        double ys=dot(y,s),yhy=dot(y,hy);
        if(ys>1e-12)for(int k=0;k<PQ_D;k++)for(int j=0;j<PQ_D;j++)H[k*PQ_D+j]+=((ys+yhy)/(ys*ys))*s[k]*s[j]-(hy[k]*s[j]+s[k]*hy[j])/ys;
        double change=loss-next;loss=next;memcpy(theta,trial,sizeof theta);memcpy(g,ng,sizeof g);
        if(it%100==0){printf("{\"phase\":\"initialization\",\"step\":%d,\"loss\":%.9g}\n",it,loss);fflush(stdout);}
        if(change<1e-10*fmax(1,fabs(loss)))break;
    }
    free(H);
    /* Quantized optimizer uses the float initialization as its regularization prior. */
    memcpy(f->prior,theta,sizeof theta);
    double m1[PQ_D]={0},m2[PQ_D]={0},best[PQ_D],best_value=INFINITY,p1=1,p2=1;
    uint32_t rng=103;int classes[2][4096],count[2]={0};
    if(n>4096)return -1;for(int i=0;i<n;i++)classes[c[i].label][count[c[i].label]++]=i;
    if(!count[0]||!count[1])return -1;
    for(int step=0;step<=steps;step++){
        if(step%50==0||step==steps){
            loss=pq_evaluate(f,c,n,NULL,n,theta,1,NULL);if(!isfinite(loss))return -1;
            if(loss<best_value){best_value=loss;memcpy(best,theta,sizeof best);}
            if(step%100==0||step==steps){printf("{\"phase\":\"qat\",\"step\":%d,\"loss\":%.9g,\"best_loss\":%.9g}\n",step,loss,best_value);fflush(stdout);}
        }
        if(step==steps)break;
        int ids[8];for(int j=0;j<8;j++){int cls=j/4;ids[j]=classes[cls][random32(&rng)%(uint32_t)count[cls]];}
        pq_evaluate(f,c,n,ids,8,theta,1,g);double norm=sqrt(dot(g,g));if(!isfinite(norm))return -1;
        double clip=fmin(1,10/fmax(norm,1e-12));p1*=.9;p2*=.999;
        for(int k=0;k<PQ_D;k++){double v=g[k]*clip;m1[k]=.9*m1[k]+.1*v;m2[k]=.999*m2[k]+.001*v*v;theta[k]-=.01*(m1[k]/(1-p1))/(sqrt(m2[k]/(1-p2))+1e-8);}
    }
    double b=best[PQ_K];for(int k=0;k<PQ_K;k++){outw[k]=(float)(best[k]/f->std[k]);b-=f->mean[k]*(best[k]/f->std[k]);}*outb=(float)b;*best_loss=best_value;
    return 0;
}

int pq_accepts(struct pq_clip *c,const struct pq_head *h,double *peak)
{
    int rows=c->cycles*25;double *p=calloc(rows,sizeof(double));if(!p)return -1;
    for(int r=30*25;r<rows;r++)p[r]=sigmoid(pq_logit(c,r,h->q[1],h->scale[1],h->zero[1],h->bias[1])-c->base_quant[r]);
    *peak=0;int accepts=0,lock=-1;
    for(int t=30;t<c->cycles;t++){
        double score=0;for(int age=9;age<=23;age++){double s=0;for(int j=0;j<3;j++)s+=p[lane(t,age,j)]/3.;score=fmax(score,s);}
        *peak=fmax(*peak,score);if(6*(t+1)>lock&&score>=.923){accepts++;lock=6*(t+1)+100;}
    }
    free(p);return accepts;
}
