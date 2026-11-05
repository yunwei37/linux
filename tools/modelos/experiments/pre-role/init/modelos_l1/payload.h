/* SPDX-License-Identifier: GPL-2.0 */
#ifndef MODELOS_KEXEC_PAYLOAD_H
#define MODELOS_KEXEC_PAYLOAD_H
#include <linux/types.h>
#define KHO_MODEL_BYTES 1192235008ULL
#define KHO_MODEL_NAME "modelos-image"
struct modelos_kho_payload {
 char magic[8];
 u64 weights_bytes, cubin_offset, cubin_bytes;
 u32 weights_fnv, cubin_fnv;
 u32 old_client, old_device, old_subdevice, mode;
 u64 producer_tsc;
};
#endif
