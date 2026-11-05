/* SPDX-License-Identifier: GPL-2.0 */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(phys_startup_64)
PHDRS { text PT_LOAD FLAGS(7); }
SECTIONS {
 . = 0xffffffff81000000;
 _text = .; _stext = .;
 .text : AT(ADDR(.text)-0xffffffff80000000) {
  KEEP(*(.head.text)) *(.text .text.*) *(.init.text .init.text.*)
 } :text
 _etext = .;
 . = ALIGN(4096);
 __start_rodata = .;
 .rodata : AT(ADDR(.rodata)-0xffffffff80000000) { *(.rodata .rodata.*) } :text
 .data : AT(ADDR(.data)-0xffffffff80000000) { *(.data .data.*) } :text
 . = ALIGN(4096);
 __bss_start = .;
 .bss (NOLOAD) : AT(ADDR(.bss)-0xffffffff80000000) { *(.bss .bss.*) *(COMMON) } :text
 . = ALIGN(4096);
 _end = .;
 _sinittext = _etext; __inittext_end = _etext;
 phys_startup_64 = startup_64 - 0xffffffff80000000;
 /DISCARD/ : { *(.eh_frame*) *(.comment) *(.note*) *(.llvm*) *(.discard*) *(__ksymtab*) *(__kcrctab*) }
}
