.text
.align 2
.global __libnx_exception_entry
.type __libnx_exception_entry, %function
__libnx_exception_entry:
    cbz x1, 9f
    ldr x2, [x1, #88]
    adr x3, probe_fault_instruction
    cmp x2, x3
    b.ne 9f
    ldr w2, [x1, #108]
    lsr w2, w2, #26
    cmp w2, #0x24
    b.ne 9f
    ldr x2, [x1, #112]
    ldr x3, [x1]
    cmp x2, x3
    b.ne 9f
    adr x2, probe_fault_resume
    str x2, [x1, #88]
    mov x2, #0x1234
    str x2, [x1]
    adrp x2, probe_fault_count
    add x2, x2, :lo12:probe_fault_count
    ldr w3, [x2]
    add w3, w3, #1
    str w3, [x2]
    mov w0, wzr
    svc #0x28
    brk #0
9:
    mov w0, #0xf801
    svc #0x28
    brk #0
.size __libnx_exception_entry, .-__libnx_exception_entry

.global probe_fault_read
.type probe_fault_read, %function
probe_fault_read:
    mov x9, #0x4321
    cmp x9, x9
probe_fault_instruction:
    ldr x0, [x0]
probe_fault_resume:
    b.ne 8f
    /* Compare against a register: the sentinel exceeds immediate range. */
    mov x10, #0x4321
    cmp x9, x10
    b.ne 8f
    ret
8:
    mov x0, #0
    ret
.size probe_fault_read, .-probe_fault_read
