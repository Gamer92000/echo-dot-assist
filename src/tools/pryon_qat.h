#ifndef PRYON_QAT_H
#define PRYON_QAT_H
#include <stdint.h>
#define PQ_K 100
#define PQ_D 101
struct pq_head { int8_t q[3][PQ_K]; float scale[3]; int32_t zero[3]; float bias[3]; };
struct pq_clip {
    int cycles, label;
    float *x, *scale;
    int32_t *q;
    unsigned char *constant;
    double *base_float, *base_quant;
};
struct pq_fit { double mean[PQ_K], std[PQ_K], prior[PQ_D]; const struct pq_head *head; };
int pq_prepare(struct pq_clip *, const struct pq_head *);
void pq_release(struct pq_clip *);
int pq_normalize(struct pq_fit *, struct pq_clip *, int);
double pq_exp(double);
double pq_log(double);
double pq_log1p(double);
void pq_quantize(const float *, int8_t *, float *, int32_t *, int32_t *);
float pq_logit(const struct pq_clip *, int, const int8_t *, float, int32_t, float);
double pq_evaluate(const struct pq_fit *, struct pq_clip *, int, const int *, int,
                   const double *, int, double *);
int pq_train(struct pq_fit *, struct pq_clip *, int, int, float *, float *, double *);
int pq_accepts(struct pq_clip *, const struct pq_head *, double *);
#endif
