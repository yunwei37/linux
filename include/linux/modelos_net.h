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
void *mos_nic_tx_acquire(size_t);
int mos_nic_tx_commit(size_t);
void mos_nic_flush(void);
struct mos_nic_stats {
 u64 tx_frames, tx_bytes, tx_kicks, rx_frames, rx_kicks, copy_bytes;
 u64 tx_kick_ns, rx_kick_ns;
};
void mos_nic_stats(struct mos_nic_stats *, int reset);
#endif
