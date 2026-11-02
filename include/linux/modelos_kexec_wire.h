/* SPDX-License-Identifier: MIT */
/* MOS1 frame retained from the existing ModelOS token transport. */
#ifndef MODELOS_KEXEC_WIRE_H
#define MODELOS_KEXEC_WIRE_H
#define KTCP_MAGIC 0x31534f4dU
#define KTCP_SUBMIT 1
#define KTCP_HELLO 5
#define KTCP_ACK 6
#define KTCP_RESUME 8
#define KTCP_DONE 17
#define KTCP_ERROR 18
#define KTCP_TOKEN_SEQ 20
#define KTCP_HANDOFF 21
#define KTCP_PORT 7373
struct ktcp_frame {unsigned magic,op,id,value;};
#endif
