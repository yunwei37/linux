/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Tracepoints for Thunderbolt/USB4 networking driver
 *
 * Copyright (C) 2023, Intel Corporation
 * Author: Mika Westerberg <mika.westerberg@linux.intel.com>
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM thunderbolt_net

#ifndef TBNET_RX_ERROR_DEFINED
#define TBNET_RX_ERROR_DEFINED

/*
 * Reason a received IP frame was rejected by tbnet_check_frame(). This is
 * kept outside of the include guard below because the enum is also used by
 * main.c.
 *
 * The values map one-to-one onto the validation steps of tbnet_check_frame()
 * in the order they are evaluated:
 *
 *   TBNET_RX_ERROR_CRC      RING_DESC_CRC_ERROR set on the descriptor
 *   TBNET_RX_ERROR_OVERRUN  RING_DESC_BUFFER_OVERRUN set
 *   TBNET_RX_ERROR_SHORT    frame carries no payload beyond the IP header
 *   TBNET_RX_ERROR_SIZE     header frame_size exceeds the descriptor length
 *   TBNET_RX_ERROR_COUNT    mid-packet frame_count differs from the head
 *   TBNET_RX_ERROR_INDEX    mid-packet frame_index/frame_id not incremented
 *   TBNET_RX_ERROR_MTU      reassembled skb would exceed TBNET_MAX_MTU
 *   TBNET_RX_ERROR_START    start-of-packet frame_count out of range
 *   TBNET_RX_ERROR_GAP      start-of-packet frame_index is not 0
 *
 * The first two are the only ones that increment rx_crc_errors and
 * rx_over_errors before the reason is known; RING_DESC_CRC_ERROR is tested
 * first so a descriptor with both bits set is reported as a CRC error.
 */
enum tbnet_rx_error {
	TBNET_RX_ERROR_NONE,
	TBNET_RX_ERROR_CRC,
	TBNET_RX_ERROR_OVERRUN,
	TBNET_RX_ERROR_SHORT,
	TBNET_RX_ERROR_SIZE,
	TBNET_RX_ERROR_COUNT,
	TBNET_RX_ERROR_INDEX,
	TBNET_RX_ERROR_MTU,
	TBNET_RX_ERROR_START,
	TBNET_RX_ERROR_GAP,
};

#endif /* TBNET_RX_ERROR_DEFINED */

#if !defined(__TRACE_THUNDERBOLT_NET_H) || defined(TRACE_HEADER_MULTI_READ)
#define __TRACE_THUNDERBOLT_NET_H

#include <linux/dma-direction.h>
#include <linux/skbuff.h>
#include <linux/tracepoint.h>

/*
 * The TRACE_DEFINE_ENUM() registration has to be emitted from inside the
 * TRACE_HEADER_MULTI_READ region: stage 1 of <trace/trace_events.h> is the
 * only pass where stages/init.h has a live definition, and that is the pass
 * that creates the _ftrace_eval_map entries. Without them the print_fmt
 * stored in the module keeps the bare enum identifiers and userspace has no
 * way to decode reason=/dir=.
 */
TRACE_DEFINE_ENUM(TBNET_RX_ERROR_NONE);
TRACE_DEFINE_ENUM(TBNET_RX_ERROR_CRC);
TRACE_DEFINE_ENUM(TBNET_RX_ERROR_OVERRUN);
TRACE_DEFINE_ENUM(TBNET_RX_ERROR_SHORT);
TRACE_DEFINE_ENUM(TBNET_RX_ERROR_SIZE);
TRACE_DEFINE_ENUM(TBNET_RX_ERROR_COUNT);
TRACE_DEFINE_ENUM(TBNET_RX_ERROR_INDEX);
TRACE_DEFINE_ENUM(TBNET_RX_ERROR_MTU);
TRACE_DEFINE_ENUM(TBNET_RX_ERROR_START);
TRACE_DEFINE_ENUM(TBNET_RX_ERROR_GAP);

TRACE_DEFINE_ENUM(DMA_BIDIRECTIONAL);
TRACE_DEFINE_ENUM(DMA_TO_DEVICE);
TRACE_DEFINE_ENUM(DMA_FROM_DEVICE);
TRACE_DEFINE_ENUM(DMA_NONE);

#define DMA_DATA_DIRECTION_NAMES			\
	{ DMA_BIDIRECTIONAL, "DMA_BIDIRECTIONAL" },	\
	{ DMA_TO_DEVICE, "DMA_TO_DEVICE" },		\
	{ DMA_FROM_DEVICE, "DMA_FROM_DEVICE" },		\
	{ DMA_NONE, "DMA_NONE" }

DECLARE_EVENT_CLASS(tbnet_frame,
	TP_PROTO(unsigned int index, const void *page, dma_addr_t phys,
		 enum dma_data_direction dir),
	TP_ARGS(index, page, phys, dir),
	TP_STRUCT__entry(
		__field(unsigned int, index)
		__field(const void *, page)
		__field(dma_addr_t, phys)
		__field(enum dma_data_direction, dir)
	),
	TP_fast_assign(
		__entry->index = index;
		__entry->page = page;
		__entry->phys = phys;
		__entry->dir = dir;
	),
	TP_printk("index=%u page=%p phys=%pad dir=%s",
		  __entry->index, __entry->page, &__entry->phys,
		__print_symbolic(__entry->dir, DMA_DATA_DIRECTION_NAMES))
);

DEFINE_EVENT(tbnet_frame, tbnet_alloc_rx_frame,
	TP_PROTO(unsigned int index, const void *page, dma_addr_t phys,
		 enum dma_data_direction dir),
	TP_ARGS(index, page, phys, dir)
);

DEFINE_EVENT(tbnet_frame, tbnet_alloc_tx_frame,
	TP_PROTO(unsigned int index, const void *page, dma_addr_t phys,
		 enum dma_data_direction dir),
	TP_ARGS(index, page, phys, dir)
);

DEFINE_EVENT(tbnet_frame, tbnet_free_frame,
	TP_PROTO(unsigned int index, const void *page, dma_addr_t phys,
		 enum dma_data_direction dir),
	TP_ARGS(index, page, phys, dir)
);

DECLARE_EVENT_CLASS(tbnet_ip_frame,
	TP_PROTO(__le32 size, __le16 id, __le16 index, __le32 count),
	TP_ARGS(size, id, index, count),
	TP_STRUCT__entry(
		__field(u32, size)
		__field(u16, id)
		__field(u16, index)
		__field(u32, count)
	),
	TP_fast_assign(
		__entry->size = le32_to_cpu(size);
		__entry->id = le16_to_cpu(id);
		__entry->index = le16_to_cpu(index);
		__entry->count = le32_to_cpu(count);
	),
	TP_printk("id=%u size=%u index=%u count=%u",
		  __entry->id, __entry->size, __entry->index, __entry->count)
);

DEFINE_EVENT(tbnet_ip_frame, tbnet_rx_ip_frame,
	TP_PROTO(__le32 size, __le16 id, __le16 index, __le32 count),
	TP_ARGS(size, id, index, count)
);

TRACE_EVENT(tbnet_invalid_rx_ip_frame,
	TP_PROTO(enum tbnet_rx_error reason, u32 desc, dma_addr_t phys,
		 unsigned int cons, unsigned int prod, __le32 size, __le16 id,
		 __le16 fidx, __le32 count),
	TP_ARGS(reason, desc, phys, cons, prod, size, id, fidx, count),
	TP_STRUCT__entry(
		__field(enum tbnet_rx_error, reason)
		__field(u32, desc)
		__field(dma_addr_t, phys)
		__field(unsigned int, cons)
		__field(unsigned int, prod)
		__field(u32, size)
		__field(u16, id)
		__field(u16, fidx)
		__field(u32, count)
	),
	TP_fast_assign(
		__entry->reason = reason;
		__entry->desc = desc;
		__entry->phys = phys;
		__entry->cons = cons;
		__entry->prod = prod;
		__entry->size = le32_to_cpu(size);
		__entry->id = le16_to_cpu(id);
		__entry->fidx = le16_to_cpu(fidx);
		__entry->count = le32_to_cpu(count);
	),
	TP_printk("reason=%s desc=%#010x dlen=%u deof=%u dsof=%u flags=%#x phys=%pad cons=%u prod=%u id=%u size=%u index=%u count=%u",
		  __print_symbolic(__entry->reason,
			{ TBNET_RX_ERROR_NONE, "none" },
			{ TBNET_RX_ERROR_CRC, "crc" },
			{ TBNET_RX_ERROR_OVERRUN, "overrun" },
			{ TBNET_RX_ERROR_SHORT, "short" },
			{ TBNET_RX_ERROR_SIZE, "size" },
			{ TBNET_RX_ERROR_COUNT, "count" },
			{ TBNET_RX_ERROR_INDEX, "index" },
			{ TBNET_RX_ERROR_MTU, "mtu" },
			{ TBNET_RX_ERROR_START, "start" },
			{ TBNET_RX_ERROR_GAP, "gap" }),
		  __entry->desc, __entry->desc & 0xfff,
		  (__entry->desc >> 12) & 0xf, (__entry->desc >> 16) & 0xf,
		  (__entry->desc >> 20) & 0xfff, &__entry->phys,
		  __entry->cons, __entry->prod, __entry->id, __entry->size,
		  __entry->fidx, __entry->count)
);

DEFINE_EVENT(tbnet_ip_frame, tbnet_tx_ip_frame,
	TP_PROTO(__le32 size, __le16 id, __le16 index, __le32 count),
	TP_ARGS(size, id, index, count)
);

DECLARE_EVENT_CLASS(tbnet_skb,
	TP_PROTO(const struct sk_buff *skb),
	TP_ARGS(skb),
	TP_STRUCT__entry(
		__field(const void *, addr)
		__field(unsigned int, len)
		__field(unsigned int, data_len)
		__field(unsigned int, nr_frags)
		__field(unsigned int, gso_size)
		__field(unsigned int, gso_segs)
		__field(unsigned int, gso_type)
	),
	TP_fast_assign(
		__entry->addr = skb;
		__entry->len = skb->len;
		__entry->data_len = skb->data_len;
		__entry->nr_frags = skb_shinfo(skb)->nr_frags;
		__entry->gso_size = skb_shinfo(skb)->gso_size;
		__entry->gso_segs = skb_shinfo(skb)->gso_segs;
		__entry->gso_type = skb_shinfo(skb)->gso_type;
	),
	TP_printk("skb=%p len=%u data_len=%u nr_frags=%u gso_size=%u gso_segs=%u gso_type=%#x",
		  __entry->addr, __entry->len, __entry->data_len,
		  __entry->nr_frags, __entry->gso_size, __entry->gso_segs,
		  __entry->gso_type)
);

DEFINE_EVENT(tbnet_skb, tbnet_rx_skb,
	TP_PROTO(const struct sk_buff *skb),
	TP_ARGS(skb)
);

DEFINE_EVENT(tbnet_skb, tbnet_tx_skb,
	TP_PROTO(const struct sk_buff *skb),
	TP_ARGS(skb)
);

DEFINE_EVENT(tbnet_skb, tbnet_consume_skb,
	TP_PROTO(const struct sk_buff *skb),
	TP_ARGS(skb)
);

#endif /* _TRACE_THUNDERBOLT_NET_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .

#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE trace

#include <trace/define_trace.h>
