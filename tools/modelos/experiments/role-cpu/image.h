/* SPDX-License-Identifier: GPL-2.0 */
#ifndef MODELOS_ROLE_IMAGE_H
#define MODELOS_ROLE_IMAGE_H
#include "role.h"
struct role_image_segment {u64 physical,bytes,sector;u32 crc32,kind;} __attribute__((packed));
struct role_image_arena {u64 physical,bytes;u32 kind,reserved;} __attribute__((packed));
struct role_image_header {
 char magic[8];u32 version,segments,cpus,arenas,crc32,reserved;
 struct role_image_segment segment[4];
 struct role_image_arena arena[4];
 u8 padding[256];
} __attribute__((packed));
extern u64 role_linear_phys,role_table_phys,role_tokenizer_phys,role_tokenizer_bytes;
extern const u32 *role_fixture_input,*role_fixture_output;
extern unsigned role_fixture_inputs,role_fixture_outputs;
int role_image_load(void);
#endif
