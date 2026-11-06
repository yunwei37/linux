/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Reuse the existing polynomial fold, without task/FPU context machinery. */
#include "../../lib/crc/x86/crc-pclmul-template.S"

SYM_FUNC_START(role_crc32_pclmul)
	_crc_pclmul n=32, lsb_crc=1, vl=16, avx_level=0
SYM_FUNC_END(role_crc32_pclmul)
