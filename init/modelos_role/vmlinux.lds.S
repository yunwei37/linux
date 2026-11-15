/* SPDX-License-Identifier: GPL-2.0 */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(phys_startup_64)
/* Public single-owner runtime API, callable after Image placement changes. */
EXTERN(mos_role_memory_add_vram mos_role_gpu_init mos_role_vram_alloc mos_role_object_free mos_role_object_location mos_role_gpu_map mos_role_gpu_unmap mos_role_gpu_poll m3_gpu_root m3_gsp_post m3_gsp_poll)
/* A declared `PHDRS` block stops lld from making the `PT_TLS` segment it
   would otherwise create for `.tdata`/`.tbss`; without this one the two
   sections get a non-TLS segment and the link refuses the object. */
PHDRS { text PT_LOAD FLAGS(7); tls PT_TLS FLAGS(4); }
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
 /* Place non-TLS orphans explicitly outside PT_TLS. Otherwise lld includes
  * their span in p_memsz and local-exec offsets do not match the template. */
 .init_array : AT(ADDR(.init_array)-0xffffffff80000000) {
  KEEP(*(.init_array .init_array.*))
 } :text
 .got : AT(ADDR(.got)-0xffffffff80000000) { *(.got .got.*) } :text
 /* The immutable ELF TLS template is copied into charged RAM by the serving
  * owner. FS names a Variant II TCB after that private copy, never this data. */
 . = ALIGN(16);
 .tdata : AT(ADDR(.tdata)-0xffffffff80000000) {
  __tdata_start = .; KEEP(*(.tdata .tdata.*)) __tdata_end = .;
 } :text :tls
 .tbss : AT(ADDR(.tbss)-0xffffffff80000000) {
  __tbss_start = .; KEEP(*(.tbss .tbss.*)) KEEP(*(.tls_common)) . = ALIGN(16); __tbss_end = .;
 } :text :tls
 __tls_alignment = ABSOLUTE(MAX(16, MAX(ALIGNOF(.tdata), ALIGNOF(.tbss))));
 __tls_end = __tbss_end;
 /* TLS NOBITS does not advance the normal location counter. */
 . = __tls_end;
 . = ALIGN(4096);
 __bss_start = .;
 .bss (NOLOAD) : AT(ADDR(.bss)-0xffffffff80000000) { *(.bss .bss.*) *(COMMON) } :text
 . = ALIGN(4096);
 _end = .;
 _sinittext = _etext; __inittext_end = _etext;
 phys_startup_64 = startup_64 - 0xffffffff80000000;
 /DISCARD/ : { *(.eh_frame*) *(.comment) *(.note*) *(.llvm*) *(.discard*) *(__ksymtab*) *(__kcrctab*) }
}
