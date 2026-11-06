// SPDX-License-Identifier: GPL-2.0-only
// One recorder source, maintained in the ModelOS checkout.
#include MODELOS_CAPTURE_SOURCE
#include <linux/init.h>
#include <linux/delay.h>
extern int modelos_nv_capture_run(unsigned int gpu_index);
static int __init modelos_native_capture_init(void)
{
 int rc = modelos_nv_capture_run(0);
 if (rc) pr_err("MODELOS_NATIVE_CAPTURE_FAIL status=%d\n", rc);
 for (;;) msleep(1000); /* Retain RM ownership until QEMU exits. */
 return 0;
}
late_initcall_sync(modelos_native_capture_init);
