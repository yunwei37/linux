/* SPDX-License-Identifier: MIT */
#ifndef MODELOS_L1_LAUNCH_H
#define MODELOS_L1_LAUNCH_H
#include "nvtypes.h"
struct l1_launch_memory {void *cpu;NvU64 gpu,bytes;};
struct l1_launch_resources {
 struct l1_launch_memory local,host,weights,state;
 void *doorbell;NvU32 token;NvU64 weights_phys,model_bytes;
};
NV_STATUS modelos_l1_launch(struct l1_launch_resources *resources);
#endif
