/* SPDX-License-Identifier: GPL-2.0 */
#include "image-layout.h"
.section .rodata.role_tokenizer,"a"
.global role_tokenizer,role_tokenizer_end
role_tokenizer:
.incbin ROLE_TOKENIZER_ASSET_PATH
role_tokenizer_end:
.section .note.GNU-stack,"",@progbits
