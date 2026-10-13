// SPDX-License-Identifier: BSD-2-Clause

#ifndef FAS_BPF_OBJ_PATH
    #error "FAS_BPF_OBJ_PATH is not defined. Build the library with the library Makefile."
#endif

__asm__(
    ".section .rodata\n"
    ".balign 8\n"
    ".hidden fas_bpf_obj_start\n"
    ".global fas_bpf_obj_start\n"
    "fas_bpf_obj_start:\n"
    ".incbin \"" FAS_BPF_OBJ_PATH
    "\"\n"
    ".hidden fas_bpf_obj_end\n"
    ".global fas_bpf_obj_end\n"
    "fas_bpf_obj_end:\n"
    ".byte 0\n"
    ".previous\n");
