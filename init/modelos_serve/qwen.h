/* SPDX-License-Identifier: MIT */
#ifndef MODELOS_QWEN_H
#define MODELOS_QWEN_H
#ifdef __KERNEL__
#include <linux/types.h>
typedef u8 qu8;
typedef u16 qu16;
typedef u32 qu32;
typedef u64 qu64;
#else
#include <stdint.h>
typedef uint8_t qu8;
typedef uint16_t qu16;
typedef uint32_t qu32;
typedef uint64_t qu64;
#endif
#define Q_DIM 1024
#define Q_FF 3072
#define Q_LAYERS 28
#define Q_HEADS 16
#define Q_KV_HEADS 8
#define Q_HD 128
#define Q_QDIM (Q_HEADS * Q_HD)
#define Q_KVDIM (Q_KV_HEADS * Q_HD)
#define Q_VOCAB 151936
#define Q_MAX_SEQ 256
#define Q_HEADER_BYTES 4096
struct q_layer {
	qu64 ln1, ln2, qn, kn, wq, wk, wv, wo, wg, wu, wd;
};
struct q_header {
	char magic[8];
	qu32 version, dim, hidden, layers, heads, kv_heads, head_dim, vocab, max_seq;
	qu32 reserved;
	qu64 bytes, embedding, norm, rope_cos, rope_sin;
	struct q_layer layer[Q_LAYERS];
};
struct q_model {
	const qu8 *base;
	const struct q_header *h;
};
/* All mutable FP state belongs to one admitted request, never to a CPU. */
struct q_state {
	qu32 capacity;
	float x[Q_DIM], xn[Q_DIM], q[Q_QDIM], k[Q_KVDIM], v[Q_KVDIM];
	float att[Q_QDIM], proj[Q_DIM], gate[Q_FF], up[Q_FF], scores[Q_MAX_SEQ];
	float last_logit;
	float kv[];
};
static inline qu64 q_arena_bytes(qu32 capacity)
{
	return sizeof(struct q_state) + (qu64)2 * Q_LAYERS * capacity * Q_KVDIM * sizeof(float);
}
/* Pure FP/SIMD entrypoints: caller must bracket every kernel call with FPU API. */
void q_begin(const struct q_model *m, struct q_state *s, qu32 token);
void q_layer(const struct q_model *m, struct q_state *s, qu32 layer, qu32 position);
qu32 q_logits(const struct q_model *m, struct q_state *s);
#ifdef __KERNEL__
int modelos_serve_prepare(void);
int modelos_serve_worker(unsigned int cpu);
int modelos_serve_network(void *unused);
#endif
#endif
