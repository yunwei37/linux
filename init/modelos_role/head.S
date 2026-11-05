/* SPDX-License-Identifier: GPL-2.0 */
/* Entered by the unchanged Linux x86 decompressor, with boot_params in RSI. */
.section .head.text,"ax"
.code64
.globl startup_64
startup_64:
 cli
 cld
 mov %rsi,%r15
 /* EFI may relocate the physical image even without virtual KASLR. */
 lea startup_64(%rip),%r14
 movabs $(startup_64-0xffffffff80000000),%rax
 sub %rax,%r14
 lea role_phys_delta(%rip),%rax
 mov %r14,(%rax)
 lea role_boot_stack_end(%rip),%rsp
 /* One direct map through 64TiB (also covers QEMU's high PCI aperture).
  * 1GiB direct-map pages; separate 2MiB pages for the kernel text alias. */
 mov $0x80000001,%eax
 cpuid
 test $(1<<26),%edx
 jnz 9f
 lea role_no_1gb(%rip),%rsi
7: lodsb
 test %al,%al
 jz 8f
 mov %al,%bl
 mov $0x3fd,%dx
6: in %dx,%al
 test $32,%al
 jz 6b
 mov %bl,%al
 mov $0x3f8,%dx
 out %al,%dx
 jmp 7b
8: hlt
 jmp 8b
9:
 lea role_pml4(%rip),%rdi
 lea role_puds(%rip),%rax
 or $3,%rax
 mov %rax,0(%rdi)
 add $2184,%rdi
 mov $128,%ecx
1: mov %rax,(%rdi)
 add $4096,%rax
 add $8,%rdi
 loop 1b
 lea role_high_pud(%rip),%rax
 or $3,%rax
 lea role_pml4(%rip),%rdi
 mov %rax,4088(%rdi)
 lea role_kernel_pmd(%rip),%rax
 or $3,%rax
 lea role_high_pud(%rip),%rdi
 mov %rax,4080(%rdi)
 lea role_kernel_pmd(%rip),%rdi
 mov $512,%ecx
 mov $0x83,%eax
 add %r14,%rax
2: test %rax,%rax
 js 5f
 mov %rax,(%rdi)
 jmp 6f
5: movq $0,(%rdi)
6:
 add $0x200000,%rax
 add $8,%rdi
 loop 2b
 lea role_puds(%rip),%rdi
 mov $65536,%ecx
 mov $0x83,%eax
4: mov %rax,(%rdi)
 add $0x40000000,%rax
 add $8,%rdi
 loop 4b
 lea role_pml4(%rip),%rax
 mov %rax,%cr3
 lea __bss_start(%rip),%rdi
 lea _end(%rip),%rcx
 sub %rdi,%rcx
 xor %eax,%eax
 rep stosb
 movabs $3f,%rax
 jmp *%rax
3: movabs $role_boot_stack_end,%rsp
 xor %ebp,%ebp
 mov %r15,%rdi
 call start_kernel
 ud2
.section .data.role_tables,"aw"
.balign 4096
.globl role_pml4
role_pml4: .skip 4096
.globl role_phys_delta
role_phys_delta: .quad 0
.balign 4096
.globl role_puds
role_puds: .skip 524288
role_high_pud: .skip 4096
role_kernel_pmd: .skip 4096
.section .bss,"aw",@nobits
.balign 16
role_boot_stack: .skip 1048576
role_boot_stack_end:

/* Linux INIT/SIPI startup sequence, specialized for fixed roles and stacks.
 * Copied below 1MiB. Parameters are written by the BSP before each SIPI. */
.section .rodata.role_ap,"a"
.globl role_ap_begin,role_ap_end
role_ap_begin:
.code16
 cli
 cld
 xor %ax,%ax
 mov %ax,%ds
 mov %ax,%es
 mov %ax,%ss
 lgdtl 0x8000+(role_ap_gdtr-role_ap_begin)
 mov %cr0,%eax
 or $1,%eax
 mov %eax,%cr0
 ljmpl $8,$0x8000+(role_ap_32-role_ap_begin)
.code32
role_ap_32:
 mov $16,%ax
 mov %ax,%ds
 mov %ax,%es
 mov %ax,%ss
 mov $0x20,%eax
 mov %eax,%cr4
 mov 0x8f00,%eax
 mov %eax,%cr3
 mov $0xc0000080,%ecx
 rdmsr
 or $0x100,%eax
 wrmsr
 mov %cr0,%eax
 or $0x80000001,%eax
 mov %eax,%cr0
 ljmpl $24,$0x8000+(role_ap_64-role_ap_begin)
.code64
role_ap_64:
 mov 0x8f08,%rsp
 mov 0x8f18,%edi
 mov 0x8f10,%rax
 jmp *%rax
.balign 8
role_ap_gdt:
 .quad 0,0x00cf9a000000ffff,0x00cf92000000ffff,0x00af9a000000ffff
role_ap_gdtr:
 .word 31
 .long 0x8000+(role_ap_gdt-role_ap_begin)
role_ap_end:

.section .text,"ax"
.code64
.globl role_wake_isr
role_wake_isr:
 push %rax
 movabs $0xffff8880fee000b0,%rax
 movl $0,(%rax)
 pop %rax
 iretq
.balign 16
.globl role_fault_stubs
role_fault_stubs:
.altmacro
.macro role_vector n
.balign 16
 /* No error code for these vectors. Normalize frame to vector,error,RIP. */
 .if (\n!=8)&&(\n!=10)&&(\n!=11)&&(\n!=12)&&(\n!=13)&&(\n!=14)&&(\n!=17)&&(\n!=21)&&(\n!=29)&&(\n!=30)
 pushq $0
 .endif
 pushq $\n
 jmp role_fault_common
.endm
.set vector,0
.rept 32
 role_vector %vector
 .set vector,vector+1
.endr
role_fault_common:
 cli
 mov %rsp,%rdi
 and $-16,%rsp
 call role_fault
 ud2
.section .note.GNU-stack,"",@progbits

.section .rodata.role_boot_error,"a"
role_no_1gb: .asciz "ROLE_BOOT_ERROR CPU requires 1GiB pages\n"
