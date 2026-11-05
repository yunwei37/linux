/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MODELOS_NET_H
#define _LINUX_MODELOS_NET_H
#include <linux/types.h>
/* Single owner, frame copies, no IRQ or runtime allocation. See serve/net/net.h. */
int mos_nic_init(u8 mac[6]);
void mos_nic_poll(void);
int mos_nic_receive(void *, size_t);
int mos_nic_tx_ready(void);
int mos_nic_send(const void *, size_t);
#endif
