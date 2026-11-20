.section .crt0, "ax", %progbits
.global _start
_start:
    ret
    .word __module_mod0 - _start
    .ascii "HOMEBREW"
.org _start + 0x80
__module_mod0:
    .ascii "MOD0"
    .word _DYNAMIC - __module_mod0
    .word __bss_start__ - __module_mod0
    .word __bss_end__ - __module_mod0
    .word 0
    .word 0
#ifdef LIBOUNCE_MODULE_TLS
    .word __libounce_tls_info - __module_mod0

.global __libounce_tls_info
.hidden __libounce_tls_info
__libounce_tls_info:
    .ascii "TLS0"
    .word __tdata_lma - __libounce_tls_info
    .word __libounce_tls_filesz
    .word __libounce_tls_memsz
    .word __libounce_tls_align
    .word libounce_module_tls_init - __libounce_tls_info
    .word libounce_module_tls_close - __libounce_tls_info
#else
    .word 0
#endif
