// SPDX-License-Identifier: MIT
/* Qwen3 CPU inference. This file never allocates, sleeps, logs or does I/O. */
#include "qwen.h"
typedef float f8 __attribute__((vector_size(32)));
typedef qu32 u8x32 __attribute__((vector_size(32)));
typedef qu16 u8x16 __attribute__((vector_size(16)));

static float bf(qu16 value)
{
	union { qu32 i; float f; } bits = { .i = (qu32)value << 16 };
	return bits.f;
}

static f8 loadf(const float *p)
{
	f8 v;
	__builtin_memcpy(&v, p, sizeof(v));
	return v;
}

static f8 loadbf(const qu16 *p)
{
	u8x16 narrow;
	union { u8x32 i; f8 f; } wide;
	__builtin_memcpy(&narrow, p, sizeof(narrow));
	wide.i = __builtin_convertvector(narrow, u8x32) << 16;
	return wide.f;
}

static f8 fmadd(f8 a, f8 b, f8 c)
{
	__asm__("vfmadd231ps %2,%1,%0" : "+x" (c) : "x" (a), "x" (b));
	return c;
}

static float sum8(f8 v)
{
	float a[8];
	__builtin_memcpy(a, &v, sizeof(v));
	return ((a[0] + a[1]) + (a[2] + a[3])) + ((a[4] + a[5]) + (a[6] + a[7]));
}

static float dotbf(const qu16 *w, const float *x, int n)
{
	f8 a = {0}, b = {0}, c = {0}, d = {0};
	int i;
	for (i = 0; i < n; i += 32) {
		a = fmadd(loadbf(w + i), loadf(x + i), a);
		b = fmadd(loadbf(w + i + 8), loadf(x + i + 8), b);
		c = fmadd(loadbf(w + i + 16), loadf(x + i + 16), c);
		d = fmadd(loadbf(w + i + 24), loadf(x + i + 24), d);
	}
	return sum8((a + b) + (c + d));
}

static float dotf(const float *a, const float *b, int n)
{
	f8 v = {0};
	int i;
	for (i = 0; i < n; i += 8)
		v += loadf(a + i) * loadf(b + i);
	return sum8(v);
}

static const qu16 *weight(const struct q_model *m, qu64 off)
{
	return (const qu16 *)(m->base + off);
}

static void matvec(float *out, const qu16 *w, const float *x, int rows, int cols)
{
	int r;
	for (r = 0; r < rows; r++)
		out[r] = dotbf(w + (qu64)r * cols, x, cols);
}

static void rms(float *out, const float *x, const qu16 *gamma, int n)
{
	float scale = 1.0f / __builtin_sqrtf(dotf(x, x, n) / n + 1.0e-6f);
	int i;
	for (i = 0; i < n; i++)
		out[i] = (x[i] * scale) * bf(gamma[i]);
}

/* Range reduction and degree-10 exp polynomial in double; one FP32 rounding. */
static float exp32(float x)
{
	double z, r, p, scale;
	int n;
	union { qu32 i; float f; } bits;
	if (x < -104.0f)
		return 0.0f;
	if (x > 88.72f) {
		bits.i = 0x7f800000;
		return bits.f;
	}
	z = (double)x * 1.4426950408889634074;
	n = (int)(z + (z >= 0 ? 0.5 : -0.5));
	r = (double)x - n * 0.6931471805599453094;
	p = 1.0 + r * (1.0 + r * (0.5 + r * (1.0/6 + r * (1.0/24 + r *
	    (1.0/120 + r * (1.0/720 + r * (1.0/5040 + r * (1.0/40320 + r *
	    (1.0/362880 + r * (1.0/3628800))))))))));
	if (n < -126) {
		bits.i = (qu32)(n + 64 + 127) << 23;
		scale = (double)bits.f * 0x1p-64;
	} else {
		bits.i = (qu32)(n + 127) << 23;
		scale = bits.f;
	}
	return (float)(p * scale);
}

void q_begin(const struct q_model *m, struct q_state *s, qu32 token)
{
	const qu16 *embedding = weight(m, m->h->embedding) + (qu64)token * Q_DIM;
	int i;
	for (i = 0; i < Q_DIM; i++)
		s->x[i] = bf(embedding[i]);
}

void q_layer(const struct q_model *m, struct q_state *s, qu32 index, qu32 pos)
{
	const struct q_layer *l = &m->h->layer[index];
	const float *cos = (const float *)(m->base + m->h->rope_cos) + pos * (Q_HD / 2);
	const float *sin = (const float *)(m->base + m->h->rope_sin) + pos * (Q_HD / 2);
	float *keys = s->kv + (qu64)index * s->capacity * Q_KVDIM;
	float *values = s->kv + (qu64)(Q_LAYERS + index) * s->capacity * Q_KVDIM;
	int h, j, t;
	rms(s->xn, s->x, weight(m, l->ln1), Q_DIM);
	matvec(s->q, weight(m, l->wq), s->xn, Q_QDIM, Q_DIM);
	matvec(s->k, weight(m, l->wk), s->xn, Q_KVDIM, Q_DIM);
	matvec(s->v, weight(m, l->wv), s->xn, Q_KVDIM, Q_DIM);
	for (h = 0; h < Q_HEADS; h++)
		rms(s->q + h * Q_HD, s->q + h * Q_HD, weight(m, l->qn), Q_HD);
	for (h = 0; h < Q_KV_HEADS; h++)
		rms(s->k + h * Q_HD, s->k + h * Q_HD, weight(m, l->kn), Q_HD);
	for (h = 0; h < Q_HEADS + Q_KV_HEADS; h++) {
		float *head = h < Q_HEADS ? s->q + h * Q_HD : s->k + (h - Q_HEADS) * Q_HD;
		for (j = 0; j < Q_HD / 2; j++) {
			float a = head[j], b = head[j + Q_HD / 2];
			head[j] = a * cos[j] - b * sin[j];
			head[j + Q_HD / 2] = b * cos[j] + a * sin[j];
		}
	}
	__builtin_memcpy(keys + pos * Q_KVDIM, s->k, sizeof(s->k));
	__builtin_memcpy(values + pos * Q_KVDIM, s->v, sizeof(s->v));
	for (h = 0; h < Q_HEADS; h++) {
		int kh = h / (Q_HEADS / Q_KV_HEADS);
		float maximum = -3.402823466e38f, denom = 0.0f;
		float *out = s->att + h * Q_HD;
		for (t = 0; t <= pos; t++) {
			float score = dotf(s->q + h * Q_HD, keys + t * Q_KVDIM + kh * Q_HD, Q_HD) * 0.08838834764831844f;
			s->scores[t] = score;
			if (score > maximum)
				maximum = score;
		}
		for (t = 0; t <= pos; t++) {
			s->scores[t] = exp32(s->scores[t] - maximum);
			denom += s->scores[t];
		}
		for (j = 0; j < Q_HD; j++)
			out[j] = 0.0f;
		for (t = 0; t <= pos; t++) {
			float a = s->scores[t] / denom;
			for (j = 0; j < Q_HD; j++)
				out[j] += a * values[t * Q_KVDIM + kh * Q_HD + j];
		}
	}
	matvec(s->proj, weight(m, l->wo), s->att, Q_DIM, Q_QDIM);
	for (j = 0; j < Q_DIM; j++)
		s->x[j] += s->proj[j];
	rms(s->xn, s->x, weight(m, l->ln2), Q_DIM);
	matvec(s->gate, weight(m, l->wg), s->xn, Q_FF, Q_DIM);
	matvec(s->up, weight(m, l->wu), s->xn, Q_FF, Q_DIM);
	for (j = 0; j < Q_FF; j++)
		s->gate[j] = (s->gate[j] / (1.0f + exp32(-s->gate[j]))) * s->up[j];
	matvec(s->proj, weight(m, l->wd), s->gate, Q_DIM, Q_FF);
	for (j = 0; j < Q_DIM; j++)
		s->x[j] += s->proj[j];
}

qu32 q_logits(const struct q_model *m, struct q_state *s)
{
	const qu16 *w = weight(m, m->h->embedding);
	float best = -3.402823466e38f;
	qu32 token = 0, i;
	rms(s->xn, s->x, weight(m, m->h->norm), Q_DIM);
	for (i = 0; i < Q_VOCAB; i++) {
		float score = dotbf(w + (qu64)i * Q_DIM, s->xn, Q_DIM);
		if (score > best) {
			best = score;
			token = i;
		}
	}
	s->last_logit = best;
	return token;
}
