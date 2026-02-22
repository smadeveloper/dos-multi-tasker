/* main.S - Multitasker assembly routines for DJGPP */

.text

.globl _task_yield
.globl _task_enter_critical
.globl _task_leave_critical
.globl _task_init_fpu_state
.globl _task_init_flags
.globl _task_dead_yield
.globl _task_timer_handler
.globl _task_old_timer
.globl _task_asm_start
.globl _task_asm_end
.globl _task_wrapper
.globl _task_check_preempt
.globl _task_get_tick

.extern __task_current
.extern __task_enable
.extern __task_count
.extern __task_fpu_state
.extern ___djgpp_ds_alias
.extern _free
.extern __task_housekeeping

#define TCB_ID          0
#define TCB_NEXT        4
#define TCB_STACK_BASE  8
#define TCB_STACK_PTR   12
#define TCB_STKLEN      16
#define TCB_PRIORITY    20
#define TCB_PCOUNT      24
#define TCB_ACTIVE      28
#define TCB_STATE       32
#define TCB_SLEEP_UNTIL 36
#define TCB_WAIT_OBJ    40

#define STATE_READY     0

/* ====================== LOCKED REGION ====================== */
.align 4
_task_asm_start:

_in_yield:          .long 0
_timer_tick:        .long 0
_tick_total:        .long 0
_task_old_timer:    .long 0, 0

.align 4
_task_timer_handler:
    pushl   %ds
    pushl   %eax
    .byte   0x2E
    movw    ___djgpp_ds_alias, %ds
    incl    _timer_tick
    incl    _tick_total
    popl    %eax
    popl    %ds
    ljmp    *%cs:_task_old_timer

.align 4
_task_asm_end:

.align 4
_task_get_tick:
    movl    _tick_total, %eax
    ret

.align 4
_task_wrapper:
    popl    %eax
    popl    %ecx
    movl    $0, _in_yield
    pushl   %ecx
    call    *%eax
    addl    $4, %esp
    jmp     _task_dead_yield

.align 4
_task_check_preempt:
    cmpl    $0, _timer_tick
    je      .Lcp_done
    movl    $0, _timer_tick

    pushl   %eax
    pushl   %ecx
    pushl   %edx
    call    __task_housekeeping
    popl    %edx
    popl    %ecx
    popl    %eax

    cmpl    $1, __task_enable
    jne     .Lcp_done
    cmpl    $0, _in_yield
    jne     .Lcp_done

    movl    __task_current, %eax
    decl    TCB_PCOUNT(%eax)
    jnz     .Lcp_done
    movl    TCB_PRIORITY(%eax), %ecx
    movl    %ecx, TCB_PCOUNT(%eax)
    jmp     _task_yield

.Lcp_done:
    ret

.align 4
_task_yield:
    cmpl    $0, _in_yield
    jne     .Lyield_bail
    movl    $1, _in_yield

    pushal
    pushl   %ds
    pushl   %es
    pushl   %fs
    pushl   %gs
    pushfl
    subl    $108, %esp
    fwait
    fnsave  (%esp)
    fwait

    movl    __task_current, %esi
    movl    %esp, TCB_STACK_PTR(%esi)

    movl    %esi, %edi
.Lyield_find:
    movl    TCB_NEXT(%esi), %esi
    cmpl    %edi, %esi
    je      .Lyield_self
    cmpl    $0, TCB_ACTIVE(%esi)
    je      .Lyield_find
    cmpl    $STATE_READY, TCB_STATE(%esi)
    jne     .Lyield_find
    movl    %esi, __task_current
    jmp     .Lyield_restore

.Lyield_self:
.Lyield_restore:
    movl    TCB_STACK_PTR(%esi), %esp
    fwait
    frstor  (%esp)
    fwait
    addl    $108, %esp
    popfl
    popl    %gs
    popl    %fs
    popl    %es
    popl    %ds
    popal
    movl    $0, _in_yield
    ret

.Lyield_bail:
    ret

.align 4
_task_dead_yield:
    cli
    movl    $1, _in_yield
    movl    $0, __task_enable
    decl    __task_count

    movl    __task_current, %esi
    movl    %esi, %edx
    movl    TCB_STACK_BASE(%esi), %eax
    pushl   %edx
    pushl   %eax

    movl    %esi, %edi
    movl    %esi, %ebx
.Ldead_prev:
    movl    %ebx, %ecx
    movl    TCB_NEXT(%ebx), %ebx
    cmpl    %edi, %ebx
    jne     .Ldead_prev

    movl    TCB_NEXT(%esi), %ebx
    movl    %ebx, TCB_NEXT(%ecx)

    movl    %ebx, %esi
.Ldead_active:
    cmpl    $0, TCB_ACTIVE(%esi)
    je      .Ldead_skip
    cmpl    $STATE_READY, TCB_STATE(%esi)
    je      .Ldead_found
.Ldead_skip:
    movl    TCB_NEXT(%esi), %esi
    jmp     .Ldead_active

.Ldead_found:
    movl    %esi, __task_current
    popl    %eax
    popl    %edx
    movl    TCB_STACK_PTR(%esi), %esp
    sti

    pushl   %edx
    pushl   %eax
    pushl   %eax
    call    _free
    addl    $4, %esp
    popl    %eax
    popl    %edx
    pushl   %edx
    call    _free
    addl    $4, %esp

    movl    $1, __task_enable

    fwait
    frstor  (%esp)
    fwait
    addl    $108, %esp
    popfl
    popl    %gs
    popl    %fs
    popl    %es
    popl    %ds
    popal
    movl    $0, _in_yield
    ret

.align 4
_task_init_fpu_state:
    pushl   %eax
    movl    $__task_fpu_state, %eax
    finit
    fwait
    fnsave  (%eax)
    fwait
    frstor  (%eax)
    popl    %eax
    ret

.align 4
_task_init_flags:
    pushfl
    popl    %eax
    ret

.align 4
_task_enter_critical:
    movl    $0, __task_enable
    ret

.align 4
_task_leave_critical:
    movl    $1, __task_enable
    ret
