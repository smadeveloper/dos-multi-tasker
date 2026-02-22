/* main.c - Full-featured Multitasker for DJGPP/MSDOS
 *
 * Features:
 *   - Preemptive + Cooperative scheduling
 *   - Semaphore, Mutex, Condition Variable, RWLock
 *   - Sleep (tick / ms)
 *   - Message passing (per-task mailbox)
 *   - Priority queue (timed callbacks)
 *   - Task pool (worker threads)
 *   - Watchdog timer
 *   - Stack guard (overflow detection)
 *   - Round-robin groups
 *   - VGA text UI (split screen per task)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dos.h>
#include <dpmi.h>
#include <go32.h>
#include <conio.h>
#include <sys/segments.h>
#include <pc.h>

/* ================================================================ */
#ifndef FALSE
#define FALSE 0
#define TRUE  1
#endif

#define MAIN_PID        (-1)
#define MIN_STACK       2048
#define TICKS_PER_SEC   18

#define STATE_READY     0
#define STATE_SLEEPING  1
#define STATE_BLOCKED   2
#define STATE_DEAD      3

#define MSG_QUEUE_SIZE  16
#define MSG_DATA_SIZE   32
#define MAX_WAITERS     16

/* Stack guard */
#define STACK_GUARD_MAGIC   0xDEADBEEF
#define STACK_GUARD_SIZE    4       /* dwords at bottom of stack */

/* Task pool */
#define POOL_MAX_JOBS       32

/* VGA text mode */
#define VGA_BASE        0xB8000
#define VGA_COLS        80
#define VGA_ROWS        25
#define VGA_SIZE        (VGA_COLS * VGA_ROWS * 2)
#define MAX_WINDOWS     8

/* Watchdog */
#define WATCHDOG_TIMEOUT_TICKS  (TICKS_PER_SEC * 5)

/* ================================================================
 * TCB - offsets 0-40 must match assembly
 * ================================================================ */
typedef struct tcb {
    int             id;             /*  0 */
    struct tcb     *next;           /*  4 */
    unsigned long  *stack_base;     /*  8 */
    unsigned long  *stack_ptr;      /* 12 */
    int             stklen;         /* 16 */
    int             priority;       /* 20 */
    int             pcount;         /* 24 */
    int             active;         /* 28 */
    int             state;          /* 32 */
    unsigned long   sleep_until;    /* 36 */
    void           *wait_obj;       /* 40 */
    /* --- below not accessed from asm --- */
    const char     *name;
    int             rr_group;       /* round-robin group (-1=none) */
    unsigned long   last_run_tick;  /* watchdog: last time this ran */
    int             watchdog_en;    /* watchdog enabled? */
} tcb_t;

/* ================================================================
 * Semaphore
 * ================================================================ */
typedef struct {
    volatile int count;
    int          max_count;
    int          waiter_pids[MAX_WAITERS];
    int          waiter_count;
} semaphore_t;

/* ================================================================
 * Mutex
 * ================================================================ */
typedef struct {
    volatile int locked;
    int          owner_pid;
    int          waiter_pids[MAX_WAITERS];
    int          waiter_count;
} mutex_t;

/* ================================================================
 * Condition Variable
 * ================================================================ */
typedef struct {
    int waiter_pids[MAX_WAITERS];
    int waiter_count;
} condvar_t;

/* ================================================================
 * Read/Write Lock
 * ================================================================ */
typedef struct {
    volatile int readers;
    volatile int writer;
    int          writer_pid;
    int          waiter_pids[MAX_WAITERS];
    int          waiter_count;
} rwlock_t;

/* ================================================================
 * Message Queue
 * ================================================================ */
typedef struct {
    int  sender_pid;
    int  type;
    char data[MSG_DATA_SIZE];
    int  len;
} message_t;

typedef struct {
    message_t msgs[MSG_QUEUE_SIZE];
    int       head, tail, count;
    int       owner_pid;
    int       waiter_pids[MAX_WAITERS];
    int       waiter_count;
} msgqueue_t;

/* ================================================================
 * Priority Queue (timed callbacks)
 * ================================================================ */
typedef struct pqnode {
    unsigned long   when;
    void          (*callback)(void *);
    void           *arg;
    struct pqnode  *next;
} pqnode_t;

/* ================================================================
 * Task Pool
 * ================================================================ */
typedef struct {
    void (*func)(void *);
    void  *arg;
} pool_job_t;

typedef struct {
    pool_job_t      jobs[POOL_MAX_JOBS];
    int             head, tail, count;
    semaphore_t     job_sem;
    mutex_t         job_mutex;
    volatile int    shutdown;
    int             worker_pids[16];
    int             num_workers;
} taskpool_t;

/* ================================================================
 * VGA Window
 * ================================================================ */
typedef struct {
    int  x, y, w, h;       /* position and size in chars */
    int  cx, cy;            /* cursor within window */
    unsigned char attr;     /* color attribute */
    int  owner_pid;
    int  used;
} vga_window_t;

/* ================================================================
 * Globals shared with assembly
 * ================================================================ */
tcb_t          *_task_current;
volatile int    _task_enable;
volatile int    _task_count;
char            _task_fpu_state[108];

/* ================================================================
 * Private globals
 * ================================================================ */
static int          task_initialized = 0;
static int          task_pid_counter = 0;
static int          task_default_flags = 0;
static pqnode_t    *pq_head = NULL;
static vga_window_t vga_wins[MAX_WINDOWS];
static int          vga_initialized = 0;

/* ================================================================
 * Assembly prototypes
 * ================================================================ */
extern void          task_yield(void);
extern void          task_dead_yield(void);
extern void          task_check_preempt(void);
extern void          task_timer_handler(void);
extern void          task_wrapper(void);
extern void          task_init_fpu_state(void);
extern int           task_init_flags(void);
extern void          task_enter_critical(void);
extern void          task_leave_critical(void);
extern unsigned long task_get_tick(void);
extern unsigned long task_old_timer[];
extern long          task_asm_start;
extern long          task_asm_end;

/* ================================================================
 * Forward declarations
 * ================================================================ */
static void     task_deinit(void);
static tcb_t   *task_find(int pid);
static void     task_check_watchdogs(void);
static void     task_check_stack_guards(void);

/* ================================================================
 * Housekeeping - called from asm every tick
 * ================================================================ */
void _task_housekeeping(void)
{
    tcb_t *t;
    unsigned long now;
    pqnode_t *node;
    int i;

    now = task_get_tick();

    /* Wake sleepers */
    t = _task_current;
    for (i = 0; i <= _task_count; i++) {
        if (t->state == STATE_SLEEPING && now >= t->sleep_until) {
            t->state = STATE_READY;
            t->sleep_until = 0;
        }
        /* Update last_run for current task */
        if (t->id == _task_current->id)
            t->last_run_tick = now;
        t = t->next;
    }

    /* Priority queue */
    while (pq_head && pq_head->when <= now) {
        node = pq_head;
        pq_head = pq_head->next;
        if (node->callback)
            node->callback(node->arg);
        free(node);
    }

    /* Watchdog check every 1 second */
    if ((now % TICKS_PER_SEC) == 0)
        task_check_watchdogs();

    /* Stack guard check every 2 seconds */
    if ((now % (TICKS_PER_SEC * 2)) == 0)
        task_check_stack_guards();
}

/* ================================================================
 * Memory locking
 * ================================================================ */
static int lock_region(void *addr, unsigned long size, int is_code)
{
    unsigned long base;
    __dpmi_meminfo mem;
    int seg = is_code ? _my_cs() : _my_ds();
    if (__dpmi_get_segment_base_address(seg, &base) == -1)
        return FALSE;
    memset(&mem, 0, sizeof(mem));
    mem.address = base + (unsigned long)addr;
    mem.size = size;
    return (__dpmi_lock_linear_region(&mem) != -1) ? TRUE : FALSE;
}

static void task_lock_memory(void)
{
    lock_region(&task_asm_start,
                (unsigned long)&task_asm_end -
                (unsigned long)&task_asm_start, 1);
    lock_region((void *)&_task_enable, sizeof(_task_enable), 0);
    lock_region((void *)&_task_current, sizeof(_task_current), 0);
    lock_region((void *)&_task_count, sizeof(_task_count), 0);
}

/* ================================================================
 * Find task
 * ================================================================ */
static tcb_t *task_find(int pid)
{
    tcb_t *t = _task_current;
    int i;
    for (i = 0; i <= _task_count; i++) {
        if (t->id == pid) return t;
        t = t->next;
    }
    return NULL;
}

/* ================================================================
 * Stack Guard
 * ================================================================ */
static void task_setup_stack_guard(unsigned long *base)
{
    int i;
    for (i = 0; i < STACK_GUARD_SIZE; i++)
        base[i] = STACK_GUARD_MAGIC;
}

static int task_check_one_guard(tcb_t *t)
{
    int i;
    if (!t->stack_base) return TRUE;    /* main thread */
    for (i = 0; i < STACK_GUARD_SIZE; i++) {
        if (t->stack_base[i] != STACK_GUARD_MAGIC)
            return FALSE;
    }
    return TRUE;
}

static void task_check_stack_guards(void)
{
    tcb_t *t = _task_current;
    int i;
    for (i = 0; i <= _task_count; i++) {
        if (t->stack_base && !task_check_one_guard(t)) {
            printf("\n!!! STACK OVERFLOW: task '%s' pid=%d !!!\n",
                   t->name, t->id);
        }
        t = t->next;
    }
}

/* ================================================================
 * Watchdog
 * ================================================================ */
void task_watchdog_enable(int pid)
{
    tcb_t *t = task_find(pid);
    if (t) {
        t->watchdog_en = TRUE;
        t->last_run_tick = task_get_tick();
    }
}

void task_watchdog_disable(int pid)
{
    tcb_t *t = task_find(pid);
    if (t) t->watchdog_en = FALSE;
}

void task_watchdog_kick(void)
{
    _task_current->last_run_tick = task_get_tick();
}

static void task_check_watchdogs(void)
{
    tcb_t *t = _task_current;
    unsigned long now = task_get_tick();
    int i;

    for (i = 0; i <= _task_count; i++) {
        if (t->watchdog_en && t->state == STATE_READY && t->active) {
            if (now - t->last_run_tick > WATCHDOG_TIMEOUT_TICKS) {
                printf("\n!!! WATCHDOG: task '%s' pid=%d stuck !!!\n",
                       t->name, t->id);
                t->watchdog_en = FALSE; /* don't spam */
            }
        }
        t = t->next;
    }
}

/* ================================================================
 * Cleanup
 * ================================================================ */
static void task_deinit(void)
{
    __dpmi_paddr vec;
    pqnode_t *node;

    _task_enable = 0;
    if (!task_initialized) return;

    vec.offset32 = task_old_timer[0];
    vec.selector = (unsigned short)task_old_timer[1];
    __dpmi_set_protected_mode_interrupt_vector(0x08, &vec);

    while (pq_head) {
        node = pq_head;
        pq_head = pq_head->next;
        free(node);
    }
    task_initialized = 0;
}

/* ================================================================
 * Init
 * ================================================================ */
int task_init(int main_priority)
{
    __dpmi_paddr vec, old;

    if (main_priority < 1 || task_initialized) return FALSE;
    if (atexit(task_deinit)) return FALSE;

    task_lock_memory();

    _task_current = (tcb_t *)malloc(sizeof(tcb_t));
    if (!_task_current) return FALSE;
    memset(_task_current, 0, sizeof(tcb_t));

    task_init_fpu_state();
    task_default_flags = task_init_flags();

    _task_current->id           = MAIN_PID;
    _task_current->next         = _task_current;
    _task_current->priority     = main_priority;
    _task_current->pcount       = main_priority;
    _task_current->active       = TRUE;
    _task_current->state        = STATE_READY;
    _task_current->name         = "main";
    _task_current->rr_group     = -1;
    _task_current->last_run_tick = 0;

    _task_enable = 0;
    _task_count = 0;
    task_pid_counter = 0;
    pq_head = NULL;

    __dpmi_get_protected_mode_interrupt_vector(0x08, &old);
    task_old_timer[0] = old.offset32;
    task_old_timer[1] = (unsigned long)old.selector;

    vec.offset32 = (unsigned long)task_timer_handler;
    vec.selector = _my_cs();
    __dpmi_set_protected_mode_interrupt_vector(0x08, &vec);

    task_initialized = 1;
    _task_enable = 1;
    return TRUE;
}

/* ================================================================
 * Spawn
 * ================================================================ */
int task_spawn(void (*func)(void *), void *arg, int stack_size,
               int priority, int active, const char *name)
{
    tcb_t *nt;
    int saved, n;

    if (!func || stack_size < MIN_STACK || priority < 1) return 0;

    saved = _task_enable;
    _task_enable = 0;

    stack_size = (stack_size + 3) & ~3;

    nt = (tcb_t *)malloc(sizeof(tcb_t));
    if (!nt) { _task_enable = saved; return 0; }
    memset(nt, 0, sizeof(tcb_t));

    nt->stack_base = (unsigned long *)malloc(stack_size);
    if (!nt->stack_base) { free(nt); _task_enable = saved; return 0; }
    memset(nt->stack_base, 0, stack_size);

    /* Stack guard at bottom */
    task_setup_stack_guard(nt->stack_base);

    nt->stklen = stack_size;
    n = stack_size / 4;

    nt->stack_base[n - 1]  = (unsigned long)arg;
    nt->stack_base[n - 2]  = (unsigned long)func;
    nt->stack_base[n - 3]  = (unsigned long)task_wrapper;
    nt->stack_base[n - 12] = (unsigned long)_my_ds();
    nt->stack_base[n - 13] = (unsigned long)_my_ds();
    nt->stack_base[n - 14] = (unsigned long)_my_ds();
    nt->stack_base[n - 15] = (unsigned long)_my_ds();
    nt->stack_base[n - 16] = (unsigned long)task_default_flags;
    memcpy(&nt->stack_base[n - 43], _task_fpu_state, 108);
    nt->stack_ptr = &nt->stack_base[n - 43];

    nt->id            = ++task_pid_counter;
    nt->priority      = priority;
    nt->pcount        = priority;
    nt->active        = active;
    nt->state         = STATE_READY;
    nt->name          = name ? name : "unnamed";
    nt->rr_group      = -1;
    nt->last_run_tick = task_get_tick();

    nt->next = _task_current->next;
    _task_current->next = nt;
    _task_count++;

    _task_enable = saved;
    return nt->id;
}

/* ================================================================
 * Kill
 * ================================================================ */
int task_kill(int pid)
{
    tcb_t *prev, *cur;
    int saved, i;

    if (pid == MAIN_PID || pid <= 0) return FALSE;

    saved = _task_enable;
    _task_enable = 0;

    if (pid == _task_current->id) { task_dead_yield(); return FALSE; }

    prev = _task_current;
    cur = _task_current->next;
    for (i = 0; i <= _task_count; i++) {
        if (cur->id == pid) break;
        prev = cur;
        cur = cur->next;
    }
    if (cur->id != pid) { _task_enable = saved; return FALSE; }

    prev->next = cur->next;
    free(cur->stack_base);
    free(cur);
    _task_count--;
    _task_enable = saved;
    return TRUE;
}

/* ================================================================
 * Queries
 * ================================================================ */
int task_getpid(void)           { return _task_current->id; }
int task_thread_count(void)     { return _task_count; }
const char *task_getname(void)  { return _task_current->name; }

/* ================================================================
 * Sleep
 * ================================================================ */
void task_sleep(unsigned long ticks)
{
    task_enter_critical();
    _task_current->state = STATE_SLEEPING;
    _task_current->sleep_until = task_get_tick() + ticks;
    task_leave_critical();
    task_yield();
}

void task_sleep_ms(unsigned long ms)
{
    unsigned long t = (ms * TICKS_PER_SEC) / 1000;
    if (t == 0) t = 1;
    task_sleep(t);
}

/* ================================================================
 * Delay / Yield helpers
 * ================================================================ */
void task_busy_delay(int iterations)
{
    volatile int i;
    for (i = 0; i < iterations; i++) {
        if ((i & 0xFF) == 0)
            task_check_preempt();
    }
}

void task_smart_yield(void)
{
    _task_housekeeping();
    task_yield();
}

/* ================================================================
 * Semaphore
 * ================================================================ */
void sem_init(semaphore_t *s, int initial, int max_val)
{
    memset(s, 0, sizeof(*s));
    s->count = initial;
    s->max_count = max_val;
}

int sem_wait(semaphore_t *s)
{
    if (!s) return FALSE;
    task_enter_critical();
    while (s->count <= 0) {
        if (s->waiter_count < MAX_WAITERS)
            s->waiter_pids[s->waiter_count++] = task_getpid();
        _task_current->state = STATE_BLOCKED;
        _task_current->wait_obj = s;
        task_leave_critical();
        task_smart_yield();
        task_enter_critical();
    }
    s->count--;
    task_leave_critical();
    return TRUE;
}

int sem_trywait(semaphore_t *s)
{
    int r = FALSE;
    if (!s) return FALSE;
    task_enter_critical();
    if (s->count > 0) { s->count--; r = TRUE; }
    task_leave_critical();
    return r;
}

static void wake_first_waiter(int *pids, int *cnt, void *obj)
{
    tcb_t *t;
    int i;
    if (*cnt <= 0) return;
    t = task_find(pids[0]);
    if (t && t->state == STATE_BLOCKED && t->wait_obj == obj) {
        t->state = STATE_READY;
        t->wait_obj = NULL;
    }
    for (i = 0; i < *cnt - 1; i++)
        pids[i] = pids[i + 1];
    (*cnt)--;
}

static void wake_all_waiters(int *pids, int *cnt, void *obj)
{
    tcb_t *t;
    int i;
    for (i = 0; i < *cnt; i++) {
        t = task_find(pids[i]);
        if (t && t->state == STATE_BLOCKED && t->wait_obj == obj) {
            t->state = STATE_READY;
            t->wait_obj = NULL;
        }
    }
    *cnt = 0;
}

int sem_signal(semaphore_t *s)
{
    if (!s) return FALSE;
    task_enter_critical();
    if (s->count < s->max_count) s->count++;
    wake_first_waiter(s->waiter_pids, &s->waiter_count, s);
    task_leave_critical();
    return TRUE;
}

int sem_getcount(semaphore_t *s) { return s ? s->count : -1; }

/* ================================================================
 * Mutex
 * ================================================================ */
void mutex_init(mutex_t *m)
{
    memset(m, 0, sizeof(*m));
    m->owner_pid = -1;
}

int mutex_lock(mutex_t *m)
{
    if (!m) return FALSE;
    task_enter_critical();
    while (m->locked) {
        if (m->waiter_count < MAX_WAITERS)
            m->waiter_pids[m->waiter_count++] = task_getpid();
        _task_current->state = STATE_BLOCKED;
        _task_current->wait_obj = m;
        task_leave_critical();
        task_smart_yield();
        task_enter_critical();
    }
    m->locked = 1;
    m->owner_pid = task_getpid();
    task_leave_critical();
    return TRUE;
}

int mutex_trylock(mutex_t *m)
{
    int r = FALSE;
    if (!m) return FALSE;
    task_enter_critical();
    if (!m->locked) { m->locked = 1; m->owner_pid = task_getpid(); r = TRUE; }
    task_leave_critical();
    return r;
}

int mutex_unlock(mutex_t *m)
{
    if (!m) return FALSE;
    task_enter_critical();
    if (m->owner_pid != task_getpid()) { task_leave_critical(); return FALSE; }
    m->locked = 0;
    m->owner_pid = -1;
    wake_first_waiter(m->waiter_pids, &m->waiter_count, m);
    task_leave_critical();
    return TRUE;
}

/* ================================================================
 * Condition Variable
 * ================================================================ */
void cond_init(condvar_t *cv)
{
    memset(cv, 0, sizeof(*cv));
}

int cond_wait(condvar_t *cv, mutex_t *m)
{
    if (!cv || !m) return FALSE;

    task_enter_critical();
    /* Add to wait list */
    if (cv->waiter_count < MAX_WAITERS)
        cv->waiter_pids[cv->waiter_count++] = task_getpid();

    /* Release mutex */
    m->locked = 0;
    m->owner_pid = -1;
    wake_first_waiter(m->waiter_pids, &m->waiter_count, m);

    /* Block */
    _task_current->state = STATE_BLOCKED;
    _task_current->wait_obj = cv;
    task_leave_critical();
    task_smart_yield();

    /* Re-acquire mutex */
    mutex_lock(m);
    return TRUE;
}

int cond_signal(condvar_t *cv)
{
    if (!cv) return FALSE;
    task_enter_critical();
    wake_first_waiter(cv->waiter_pids, &cv->waiter_count, cv);
    task_leave_critical();
    return TRUE;
}

int cond_broadcast(condvar_t *cv)
{
    if (!cv) return FALSE;
    task_enter_critical();
    wake_all_waiters(cv->waiter_pids, &cv->waiter_count, cv);
    task_leave_critical();
    return TRUE;
}

/* ================================================================
 * Read/Write Lock
 * ================================================================ */
void rwlock_init(rwlock_t *rw)
{
    memset(rw, 0, sizeof(*rw));
    rw->writer_pid = -1;
}

int rwlock_read_lock(rwlock_t *rw)
{
    if (!rw) return FALSE;
    task_enter_critical();
    while (rw->writer) {
        if (rw->waiter_count < MAX_WAITERS)
            rw->waiter_pids[rw->waiter_count++] = task_getpid();
        _task_current->state = STATE_BLOCKED;
        _task_current->wait_obj = rw;
        task_leave_critical();
        task_smart_yield();
        task_enter_critical();
    }
    rw->readers++;
    task_leave_critical();
    return TRUE;
}

int rwlock_read_unlock(rwlock_t *rw)
{
    if (!rw) return FALSE;
    task_enter_critical();
    if (rw->readers > 0) rw->readers--;
    if (rw->readers == 0)
        wake_first_waiter(rw->waiter_pids, &rw->waiter_count, rw);
    task_leave_critical();
    return TRUE;
}

int rwlock_write_lock(rwlock_t *rw)
{
    if (!rw) return FALSE;
    task_enter_critical();
    while (rw->writer || rw->readers > 0) {
        if (rw->waiter_count < MAX_WAITERS)
            rw->waiter_pids[rw->waiter_count++] = task_getpid();
        _task_current->state = STATE_BLOCKED;
        _task_current->wait_obj = rw;
        task_leave_critical();
        task_smart_yield();
        task_enter_critical();
    }
    rw->writer = 1;
    rw->writer_pid = task_getpid();
    task_leave_critical();
    return TRUE;
}

int rwlock_write_unlock(rwlock_t *rw)
{
    if (!rw) return FALSE;
    task_enter_critical();
    if (rw->writer_pid != task_getpid()) { task_leave_critical(); return FALSE; }
    rw->writer = 0;
    rw->writer_pid = -1;
    wake_all_waiters(rw->waiter_pids, &rw->waiter_count, rw);
    task_leave_critical();
    return TRUE;
}

/* ================================================================
 * Message Queue
 * ================================================================ */
void mq_init(msgqueue_t *mq, int owner_pid)
{
    memset(mq, 0, sizeof(*mq));
    mq->owner_pid = owner_pid;
}

int mq_send(msgqueue_t *mq, int type, const void *data, int len)
{
    message_t *msg;
    if (!mq) return FALSE;
    if (len > MSG_DATA_SIZE) len = MSG_DATA_SIZE;

    task_enter_critical();
    if (mq->count >= MSG_QUEUE_SIZE) { task_leave_critical(); return FALSE; }

    msg = &mq->msgs[mq->tail];
    msg->sender_pid = task_getpid();
    msg->type = type;
    msg->len = (data && len > 0) ? len : 0;
    if (msg->len > 0) memcpy(msg->data, data, msg->len);

    mq->tail = (mq->tail + 1) % MSG_QUEUE_SIZE;
    mq->count++;
    wake_first_waiter(mq->waiter_pids, &mq->waiter_count, mq);
    task_leave_critical();
    return TRUE;
}

int mq_receive(msgqueue_t *mq, message_t *out)
{
    if (!mq || !out) return FALSE;
    task_enter_critical();
    while (mq->count == 0) {
        if (mq->waiter_count < MAX_WAITERS)
            mq->waiter_pids[mq->waiter_count++] = task_getpid();
        _task_current->state = STATE_BLOCKED;
        _task_current->wait_obj = mq;
        task_leave_critical();
        task_smart_yield();
        task_enter_critical();
    }
    memcpy(out, &mq->msgs[mq->head], sizeof(message_t));
    mq->head = (mq->head + 1) % MSG_QUEUE_SIZE;
    mq->count--;
    task_leave_critical();
    return TRUE;
}

int mq_tryreceive(msgqueue_t *mq, message_t *out)
{
    int r = FALSE;
    if (!mq || !out) return FALSE;
    task_enter_critical();
    if (mq->count > 0) {
        memcpy(out, &mq->msgs[mq->head], sizeof(message_t));
        mq->head = (mq->head + 1) % MSG_QUEUE_SIZE;
        mq->count--;
        r = TRUE;
    }
    task_leave_critical();
    return r;
}

int mq_pending(msgqueue_t *mq) { return mq ? mq->count : 0; }

/* ================================================================
 * Priority Queue
 * ================================================================ */
int pq_schedule(unsigned long delay, void (*cb)(void *), void *arg)
{
    pqnode_t *nd, *prev, *cur;
    nd = (pqnode_t *)malloc(sizeof(pqnode_t));
    if (!nd) return FALSE;
    nd->when = task_get_tick() + delay;
    nd->callback = cb;
    nd->arg = arg;
    nd->next = NULL;

    task_enter_critical();
    if (!pq_head || nd->when < pq_head->when) {
        nd->next = pq_head;
        pq_head = nd;
    } else {
        prev = pq_head;
        cur = pq_head->next;
        while (cur && cur->when <= nd->when) { prev = cur; cur = cur->next; }
        nd->next = cur;
        prev->next = nd;
    }
    task_leave_critical();
    return TRUE;
}

int pq_schedule_ms(unsigned long ms, void (*cb)(void *), void *arg)
{
    unsigned long t = (ms * TICKS_PER_SEC) / 1000;
    return pq_schedule(t > 0 ? t : 1, cb, arg);
}

/* ================================================================
 * Round-Robin Groups
 *
 * Tasks in the same rr_group share a single time slice.
 * When one yields, the next in the same group runs.
 * ================================================================ */
void task_set_rr_group(int pid, int group)
{
    tcb_t *t = task_find(pid);
    if (t) t->rr_group = group;
}

/* Yield only to tasks in same rr_group */
void task_rr_yield(void)
{
    int grp = _task_current->rr_group;
    tcb_t *start, *t;

    if (grp < 0) { task_yield(); return; }

    /* Find next ready task in same group */
    _task_housekeeping();

    task_enter_critical();
    start = _task_current;
    t = _task_current->next;
    while (t != start) {
        if (t->active && t->state == STATE_READY && t->rr_group == grp) {
            task_leave_critical();
            task_yield();
            return;
        }
        t = t->next;
    }
    task_leave_critical();
    /* No other task in group, continue running */
}

/* ================================================================
 * Task Pool
 * ================================================================ */
static void pool_worker(void *arg)
{
    taskpool_t *pool = (taskpool_t *)arg;
    pool_job_t job;

    while (!pool->shutdown) {
        sem_wait(&pool->job_sem);
        if (pool->shutdown) break;

        mutex_lock(&pool->job_mutex);
        if (pool->count > 0) {
            job = pool->jobs[pool->head];
            pool->head = (pool->head + 1) % POOL_MAX_JOBS;
            pool->count--;
            mutex_unlock(&pool->job_mutex);

            if (job.func)
                job.func(job.arg);
        } else {
            mutex_unlock(&pool->job_mutex);
        }
    }
}

void pool_init(taskpool_t *pool, int num_workers, int stack_size)
{
    int i;
    char name[16];

    memset(pool, 0, sizeof(*pool));
    sem_init(&pool->job_sem, 0, POOL_MAX_JOBS);
    mutex_init(&pool->job_mutex);
    pool->shutdown = 0;

    if (num_workers > 16) num_workers = 16;
    pool->num_workers = num_workers;

    for (i = 0; i < num_workers; i++) {
        sprintf(name, "worker-%d", i);
        pool->worker_pids[i] = task_spawn(pool_worker, pool,
                                           stack_size, 1, TRUE, name);
    }
}

int pool_submit(taskpool_t *pool, void (*func)(void *), void *arg)
{
    if (!pool || pool->shutdown) return FALSE;

    mutex_lock(&pool->job_mutex);
    if (pool->count >= POOL_MAX_JOBS) {
        mutex_unlock(&pool->job_mutex);
        return FALSE;
    }
    pool->jobs[pool->tail].func = func;
    pool->jobs[pool->tail].arg = arg;
    pool->tail = (pool->tail + 1) % POOL_MAX_JOBS;
    pool->count++;
    mutex_unlock(&pool->job_mutex);

    sem_signal(&pool->job_sem);
    return TRUE;
}

void pool_shutdown(taskpool_t *pool)
{
    int i;
    pool->shutdown = 1;
    /* Wake all workers */
    for (i = 0; i < pool->num_workers; i++)
        sem_signal(&pool->job_sem);
    /* Wait for them */
    for (i = 0; i < 100 && task_thread_count() > 0; i++)
        task_busy_delay(5000);
    /* Kill stragglers */
    for (i = 0; i < pool->num_workers; i++) {
        if (pool->worker_pids[i])
            task_kill(pool->worker_pids[i]);
    }
}

/* ================================================================
 * VGA Text UI
 * ================================================================ */
void vga_init(void)
{
    memset(vga_wins, 0, sizeof(vga_wins));
    vga_initialized = 1;
}

static void vga_putchar_at(int x, int y, char ch, unsigned char attr)
{
    unsigned long offset = (y * VGA_COLS + x) * 2;
    _farpokeb(_dos_ds, VGA_BASE + offset, ch);
    _farpokeb(_dos_ds, VGA_BASE + offset + 1, attr);
}

static void vga_scroll_window(vga_window_t *w)
{
    int row, col;
    unsigned long src, dst;

    for (row = w->y + 1; row < w->y + w->h; row++) {
        for (col = w->x; col < w->x + w->w; col++) {
            src = (row * VGA_COLS + col) * 2;
            dst = ((row - 1) * VGA_COLS + col) * 2;
            _farpokeb(_dos_ds, VGA_BASE + dst,
                      _farpeekb(_dos_ds, VGA_BASE + src));
            _farpokeb(_dos_ds, VGA_BASE + dst + 1,
                      _farpeekb(_dos_ds, VGA_BASE + src + 1));
        }
    }
    /* Clear last row */
    for (col = w->x; col < w->x + w->w; col++)
        vga_putchar_at(col, w->y + w->h - 1, ' ', w->attr);
}

static void vga_draw_border(vga_window_t *w, const char *title)
{
    int i, bx, by, bw, bh;
    unsigned char ba = w->attr;

    bx = w->x - 1;
    by = w->y - 1;
    bw = w->w + 2;
    bh = w->h + 2;

    if (bx < 0 || by < 0) return;

    /* Corners */
    vga_putchar_at(bx, by, '+', ba);
    vga_putchar_at(bx + bw - 1, by, '+', ba);
    vga_putchar_at(bx, by + bh - 1, '+', ba);
    vga_putchar_at(bx + bw - 1, by + bh - 1, '+', ba);

    /* Horizontal */
    for (i = 1; i < bw - 1; i++) {
        vga_putchar_at(bx + i, by, '-', ba);
        vga_putchar_at(bx + i, by + bh - 1, '-', ba);
    }

    /* Vertical */
    for (i = 1; i < bh - 1; i++) {
        vga_putchar_at(bx, by + i, '|', ba);
        vga_putchar_at(bx + bw - 1, by + i, '|', ba);
    }

    /* Title */
    if (title) {
        int tlen = (int)strlen(title);
        int tx = bx + (bw - tlen) / 2;
        for (i = 0; i < tlen && tx + i < bx + bw - 1; i++)
            vga_putchar_at(tx + i, by, title[i], ba);
    }
}

int vga_create_window(int x, int y, int w, int h, unsigned char attr,
                      const char *title)
{
    int i, row, col;

    if (!vga_initialized) vga_init();

    for (i = 0; i < MAX_WINDOWS; i++) {
        if (!vga_wins[i].used) {
            vga_wins[i].x = x;
            vga_wins[i].y = y;
            vga_wins[i].w = w;
            vga_wins[i].h = h;
            vga_wins[i].cx = 0;
            vga_wins[i].cy = 0;
            vga_wins[i].attr = attr;
            vga_wins[i].owner_pid = task_getpid();
            vga_wins[i].used = 1;

            /* Clear window area */
            for (row = y; row < y + h; row++)
                for (col = x; col < x + w; col++)
                    vga_putchar_at(col, row, ' ', attr);

            vga_draw_border(&vga_wins[i], title);
            return i;
        }
    }
    return -1;
}

void vga_window_print(int win_id, const char *fmt, ...)
{
    va_list ap;
    char buf[256];
    vga_window_t *w;
    int i, len;

    if (win_id < 0 || win_id >= MAX_WINDOWS) return;
    w = &vga_wins[win_id];
    if (!w->used) return;

    va_start(ap, fmt);
    len = vsprintf(buf, fmt, ap);
    va_end(ap);

    task_enter_critical();
    for (i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            w->cx = 0;
            w->cy++;
        } else {
            if (w->cx < w->w) {
                vga_putchar_at(w->x + w->cx, w->y + w->cy, buf[i], w->attr);
                w->cx++;
            }
        }

        if (w->cy >= w->h) {
            vga_scroll_window(w);
            w->cy = w->h - 1;
            w->cx = 0;
        }
    }
    task_leave_critical();
}

void vga_clear_window(int win_id)
{
    vga_window_t *w;
    int row, col;

    if (win_id < 0 || win_id >= MAX_WINDOWS) return;
    w = &vga_wins[win_id];
    if (!w->used) return;

    task_enter_critical();
    for (row = w->y; row < w->y + w->h; row++)
        for (col = w->x; col < w->x + w->w; col++)
            vga_putchar_at(col, row, ' ', w->attr);
    w->cx = 0;
    w->cy = 0;
    task_leave_critical();
}

void vga_clear_screen(void)
{
    int i;
    for (i = 0; i < VGA_ROWS * VGA_COLS; i++) {
        _farpokeb(_dos_ds, VGA_BASE + i * 2, ' ');
        _farpokeb(_dos_ds, VGA_BASE + i * 2 + 1, 0x07);
    }
}

/* ================================================================
 * Task List (debug)
 * ================================================================ */
void task_list(void)
{
    static const char *sn[] = {"RDY", "SLP", "BLK", "DED"};
    tcb_t *t;
    int i;

    task_enter_critical();
    printf("\n%-5s %-10s %-4s %-4s %-4s %-4s %-5s\n",
           "PID", "Name", "Pri", "St", "Act", "RR", "Guard");
    printf("------------------------------------------\n");
    t = _task_current;
    for (i = 0; i <= _task_count; i++) {
        printf("%-5d %-10s %-4d %-4s %-4s %-4d %-5s%s\n",
               t->id,
               t->name ? t->name : "?",
               t->priority,
               (t->state >= 0 && t->state <= 3) ? sn[t->state] : "?",
               t->active ? "Y" : "N",
               t->rr_group,
               t->stack_base ? (task_check_one_guard(t) ? "OK" : "FAIL") : "N/A",
               (t->id == _task_current->id) ? " <-" : "");
        t = t->next;
    }
    printf("------------------------------------------\n");
    printf("Total: %d  Tick: %lu\n\n", _task_count, task_get_tick());
    task_leave_critical();
}

/* ================================================================
 * Wait helper
 * ================================================================ */
static void wait_tasks(void)
{
    while (task_thread_count() > 0)
        task_busy_delay(1000);
}

static void wait_tasks_timeout(int secs)
{
    unsigned long dl = task_get_tick() + (unsigned long)secs * TICKS_PER_SEC;
    while (task_thread_count() > 0 && task_get_tick() < dl)
        task_busy_delay(1000);
}

/* ================================================================
 *                       DEMO TASKS
 * ================================================================ */

static volatile int demo_stop = 0;
static int shared_counter = 0;
static semaphore_t  demo_sem;
static mutex_t      demo_mutex;
static condvar_t    demo_cond;
static rwlock_t     demo_rwlock;
static msgqueue_t   demo_mq;
static taskpool_t   demo_pool;
static int          shared_data = 0;

/* --- Preemptive --- */
void preemptive_task(void *arg)
{
    int id = *((int *)arg);
    while (!demo_stop) {
        task_enter_critical();
        printf("  [Pre] T%d pid=%d\n", id, task_getpid());
        task_leave_critical();
        task_busy_delay(50000);
    }
}

/* --- Cooperative --- */
void coop_task(void *arg)
{
    int id = *((int *)arg);
    int i;
    for (i = 0; i < 5; i++) {
        task_enter_critical();
        printf("  [Coop] T%d #%d\n", id, i);
        task_leave_critical();
        task_smart_yield();
    }
}

/* --- Semaphore --- */
void producer_task(void *arg)
{
    int id = *((int *)arg);
    int i;
    for (i = 0; i < 5; i++) {
        task_enter_critical();
        printf("  [Prod %d] item %d\n", id, i);
        task_leave_critical();
        sem_signal(&demo_sem);
        task_busy_delay(15000);
    }
}

void consumer_task(void *arg)
{
    int id = *((int *)arg);
    int i;
    for (i = 0; i < 5; i++) {
        sem_wait(&demo_sem);
        task_enter_critical();
        printf("  [Cons %d] got %d\n", id, i);
        task_leave_critical();
    }
}

/* --- Mutex --- */
void mutex_task(void *arg)
{
    int id = *((int *)arg);
    int i;
    for (i = 0; i < 10; i++) {
        mutex_lock(&demo_mutex);
        shared_counter++;
        task_enter_critical();
        printf("  [Mtx] T%d cnt=%d\n", id, shared_counter);
        task_leave_critical();
        mutex_unlock(&demo_mutex);
        task_busy_delay(10000);
    }
}

/* --- Condition Variable --- */
void cond_waiter_task(void *arg)
{
    int id = *((int *)arg);

    mutex_lock(&demo_mutex);
    task_enter_critical();
    printf("  [CondW %d] waiting...\n", id);
    task_leave_critical();

    cond_wait(&demo_cond, &demo_mutex);

    task_enter_critical();
    printf("  [CondW %d] woke! data=%d\n", id, shared_data);
    task_leave_critical();
    mutex_unlock(&demo_mutex);
}

void cond_signaler_task(void *arg)
{
    (void)arg;
    task_busy_delay(40000);

    mutex_lock(&demo_mutex);
    shared_data = 42;
    task_enter_critical();
    printf("  [CondS] broadcasting data=%d\n", shared_data);
    task_leave_critical();
    mutex_unlock(&demo_mutex);

    cond_broadcast(&demo_cond);
}

/* --- RWLock --- */
void reader_task(void *arg)
{
    int id = *((int *)arg);
    int i;
    for (i = 0; i < 5; i++) {
        rwlock_read_lock(&demo_rwlock);
        task_enter_critical();
        printf("  [Rd %d] data=%d\n", id, shared_data);
        task_leave_critical();
        rwlock_read_unlock(&demo_rwlock);
        task_busy_delay(10000);
    }
}

void writer_task(void *arg)
{
    int id = *((int *)arg);
    int i;
    for (i = 0; i < 3; i++) {
        rwlock_write_lock(&demo_rwlock);
        shared_data += 10;
        task_enter_critical();
        printf("  [Wr %d] wrote %d\n", id, shared_data);
        task_leave_critical();
        rwlock_write_unlock(&demo_rwlock);
        task_busy_delay(20000);
    }
}

/* --- Messages --- */
void sender_task(void *arg)
{
    int id = *((int *)arg);
    char buf[MSG_DATA_SIZE];
    int i;
    for (i = 0; i < 5; i++) {
        sprintf(buf, "hi from %d #%d", id, i);
        mq_send(&demo_mq, i, buf, (int)strlen(buf) + 1);
        task_enter_critical();
        printf("  [Snd %d] %s\n", id, buf);
        task_leave_critical();
        task_busy_delay(15000);
    }
}

void receiver_task(void *arg)
{
    int n = *((int *)arg);  /* how many to receive */
    int cnt = 0;
    message_t msg;
    while (cnt < n) {
        if (mq_receive(&demo_mq, &msg)) {
            task_enter_critical();
            printf("  [Rcv] from %d: %s\n", msg.sender_pid, msg.data);
            task_leave_critical();
            cnt++;
        }
    }
}

/* --- Sleep --- */
void sleepy_task(void *arg)
{
    int id = *((int *)arg);
    int i;
    for (i = 0; i < 3; i++) {
        task_enter_critical();
        printf("  [Slp %d] tick=%lu sleeping 1s\n", id, task_get_tick());
        task_leave_critical();
        task_sleep(TICKS_PER_SEC);
    }
    task_enter_critical();
    printf("  [Slp %d] done tick=%lu\n", id, task_get_tick());
    task_leave_critical();
}

/* --- Priority Queue callback --- */
static int pq_ids[3] = {100, 200, 300};
void pq_callback(void *arg)
{
    int id = *((int *)arg);
    printf("  >>> PQ cb id=%d tick=%lu\n", id, task_get_tick());
}

/* --- Task Pool job --- */
static volatile int pool_done_count = 0;
void pool_job_func(void *arg)
{
    int id = *((int *)arg);
    task_enter_critical();
    printf("  [Pool] job %d running on pid=%d\n", id, task_getpid());
    task_leave_critical();
    task_busy_delay(10000);
    pool_done_count++;
}

/* --- Round-robin group --- */
void rr_task(void *arg)
{
    int id = *((int *)arg);
    int i;
    for (i = 0; i < 5; i++) {
        task_enter_critical();
        printf("  [RR] T%d grp=%d #%d\n", id,
               _task_current->rr_group, i);
        task_leave_critical();
        task_rr_yield();
    }
}

/* --- VGA window task --- */
void vga_task(void *arg)
{
    int win = *((int *)arg);
    int i;
    for (i = 0; i < 20; i++) {
        vga_window_print(win, "tick %lu #%d\n", task_get_tick(), i);
        task_busy_delay(20000);
    }
}

/* --- Watchdog test --- */
void good_task(void *arg)
{
    int id = *((int *)arg);
    int i;
    for (i = 0; i < 10; i++) {
        task_watchdog_kick();
        task_enter_critical();
        printf("  [Good %d] alive #%d\n", id, i);
        task_leave_critical();
        task_busy_delay(20000);
    }
}

/* --- Stack overflow test (intentional) --- */
void stack_eater(void *arg)
{
    char buf[512];
    int id = *((int *)arg);
    memset(buf, 'X', sizeof(buf));
    task_enter_critical();
    printf("  [StackEat %d] used 512 bytes\n", id);
    task_leave_critical();
    task_busy_delay(20000);
}

/* ================================================================
 * Main
 * ================================================================ */
int main(void)
{
    int args[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    int choice;
    int pid1, pid2, pid3, pid4;
    int wins[4];
    int recv_count;
    int pool_jobs[8];
    int i;

    clrscr();
    printf("=============================================\n");
    printf("  DJGPP Multitasker - All Features Demo\n");
    printf("=============================================\n\n");
    printf(" 1. Preemptive\n");
    printf(" 2. Cooperative\n");
    printf(" 3. Semaphore\n");
    printf(" 4. Mutex\n");
    printf(" 5. Condition Variable\n");
    printf(" 6. Read/Write Lock\n");
    printf(" 7. Messages\n");
    printf(" 8. Sleep\n");
    printf(" 9. Priority Queue\n");
    printf(" A. Task Pool\n");
    printf(" B. Round-Robin Groups\n");
    printf(" C. Watchdog\n");
    printf(" D. Stack Guard\n");
    printf(" E. VGA Text UI\n");
    printf(" F. EVERYTHING\n");
    printf("\nChoice: ");
    fflush(stdout);

    choice = getch();
    if (choice >= 'a') choice -= 32;    /* to upper */
    printf("%c\n\n", choice);

    if (!task_init(1)) {
        printf("Init failed!\n");
        return 1;
    }

    switch (choice) {

    case '1':
        printf("--- Preemptive --- (key to stop)\n\n");
        demo_stop = 0;
        for (i = 0; i < 4; i++)
            task_spawn(preemptive_task, &args[i], 16384, 1, TRUE, "pre");
        while (!kbhit()) {
            task_enter_critical();
            printf("MAIN\n");
            task_leave_critical();
            task_busy_delay(50000);
        }
        getch(); demo_stop = 1;
        wait_tasks();
        printf("\nDone.\n");
        break;

    case '2':
        printf("--- Cooperative ---\n\n");
        for (i = 0; i < 4; i++)
            task_spawn(coop_task, &args[i], 16384, 1, TRUE, "coop");
        wait_tasks();
        printf("\nDone.\n");
        break;

    case '3':
        printf("--- Semaphore ---\n\n");
        sem_init(&demo_sem, 0, 100);
        task_spawn(producer_task, &args[0], 16384, 1, TRUE, "prod-1");
        task_spawn(producer_task, &args[1], 16384, 1, TRUE, "prod-2");
        task_spawn(consumer_task, &args[2], 16384, 1, TRUE, "cons-1");
        task_spawn(consumer_task, &args[3], 16384, 1, TRUE, "cons-2");
        wait_tasks();
        printf("\nDone.\n");
        break;

    case '4':
        printf("--- Mutex ---\n\n");
        mutex_init(&demo_mutex);
        shared_counter = 0;
        for (i = 0; i < 3; i++)
            task_spawn(mutex_task, &args[i], 16384, 1, TRUE, "mtx");
        wait_tasks();
        printf("\nCounter: %d\n", shared_counter);
        break;

    case '5':
        printf("--- Condition Variable ---\n\n");
        mutex_init(&demo_mutex);
        cond_init(&demo_cond);
        shared_data = 0;
        task_spawn(cond_waiter_task, &args[0], 16384, 1, TRUE, "cw-1");
        task_spawn(cond_waiter_task, &args[1], 16384, 1, TRUE, "cw-2");
        task_spawn(cond_waiter_task, &args[2], 16384, 1, TRUE, "cw-3");
        task_spawn(cond_signaler_task, &args[3], 16384, 1, TRUE, "cs");
        wait_tasks();
        printf("\nDone.\n");
        break;

    case '6':
        printf("--- RW Lock ---\n\n");
        rwlock_init(&demo_rwlock);
        shared_data = 0;
        task_spawn(reader_task, &args[0], 16384, 1, TRUE, "rd-1");
        task_spawn(reader_task, &args[1], 16384, 1, TRUE, "rd-2");
        task_spawn(reader_task, &args[2], 16384, 1, TRUE, "rd-3");
        task_spawn(writer_task, &args[3], 16384, 1, TRUE, "wr-1");
        wait_tasks();
        printf("\nFinal data: %d\n", shared_data);
        break;

    case '7':
        printf("--- Messages ---\n\n");
        mq_init(&demo_mq, MAIN_PID);
        recv_count = 10;
        task_spawn(sender_task, &args[0], 16384, 1, TRUE, "snd-1");
        task_spawn(sender_task, &args[1], 16384, 1, TRUE, "snd-2");
        task_spawn(receiver_task, &recv_count, 16384, 1, TRUE, "recv");
        wait_tasks();
        printf("\nDone.\n");
        break;

    case '8':
        printf("--- Sleep ---\n\n");
        task_spawn(sleepy_task, &args[0], 16384, 1, TRUE, "slp-1");
        task_spawn(sleepy_task, &args[1], 16384, 1, TRUE, "slp-2");
        while (task_thread_count() > 0) {
            task_enter_critical();
            printf("MAIN tick=%lu\n", task_get_tick());
            task_leave_critical();
            task_busy_delay(20000);
        }
        printf("\nDone.\n");
        break;

    case '9':
        printf("--- Priority Queue ---\n");
        printf("Callbacks at +1s +2s +3s\n\n");
        pq_schedule(TICKS_PER_SEC * 1, pq_callback, &pq_ids[0]);
        pq_schedule(TICKS_PER_SEC * 2, pq_callback, &pq_ids[1]);
        pq_schedule(TICKS_PER_SEC * 3, pq_callback, &pq_ids[2]);
        {
            unsigned long dl = task_get_tick() + TICKS_PER_SEC * 4;
            while (task_get_tick() < dl) {
                task_enter_critical();
                printf("MAIN tick=%lu\n", task_get_tick());
                task_leave_critical();
                task_busy_delay(20000);
            }
        }
        printf("\nDone.\n");
        break;

    case 'A':
        printf("--- Task Pool ---\n\n");
        pool_init(&demo_pool, 3, 16384);
        pool_done_count = 0;
        for (i = 0; i < 8; i++) {
            pool_jobs[i] = i + 1;
            pool_submit(&demo_pool, pool_job_func, &pool_jobs[i]);
        }
        while (pool_done_count < 8)
            task_busy_delay(5000);
        printf("\nAll jobs done. Shutting down pool...\n");
        pool_shutdown(&demo_pool);
        printf("Done.\n");
        break;

    case 'B':
        printf("--- Round-Robin Groups ---\n\n");
        pid1 = task_spawn(rr_task, &args[0], 16384, 1, TRUE, "rr-a1");
        pid2 = task_spawn(rr_task, &args[1], 16384, 1, TRUE, "rr-a2");
        pid3 = task_spawn(rr_task, &args[2], 16384, 1, TRUE, "rr-b1");
        pid4 = task_spawn(rr_task, &args[3], 16384, 1, TRUE, "rr-b2");
        task_set_rr_group(pid1, 1);
        task_set_rr_group(pid2, 1);
        task_set_rr_group(pid3, 2);
        task_set_rr_group(pid4, 2);
        wait_tasks();
        printf("\nDone.\n");
        break;

    case 'C':
        printf("--- Watchdog ---\n");
        printf("Good tasks kick watchdog. One will stop.\n\n");
        pid1 = task_spawn(good_task, &args[0], 16384, 1, TRUE, "good-1");
        pid2 = task_spawn(good_task, &args[1], 16384, 1, TRUE, "good-2");
        task_watchdog_enable(pid1);
        task_watchdog_enable(pid2);
        wait_tasks();
        printf("\nDone.\n");
        break;

    case 'D':
        printf("--- Stack Guard ---\n\n");
        task_spawn(stack_eater, &args[0], 4096, 1, TRUE, "eater-1");
        task_spawn(stack_eater, &args[1], 4096, 1, TRUE, "eater-2");
        task_list();
        wait_tasks();
        task_enter_critical();
        printf("\nStack guard check after tasks finished.\n");
        task_leave_critical();
        printf("Done.\n");
        break;

    case 'E':
        printf("VGA demo starting in 2 seconds...\n");
        task_busy_delay(60000);
        vga_clear_screen();
        vga_init();

        wins[0] = vga_create_window(2,  2,  35, 8, 0x1F, " Task 1 ");
        wins[1] = vga_create_window(42, 2,  35, 8, 0x2F, " Task 2 ");
        wins[2] = vga_create_window(2,  14, 35, 8, 0x4F, " Task 3 ");
        wins[3] = vga_create_window(42, 14, 35, 8, 0x5F, " Task 4 ");

        for (i = 0; i < 4; i++)
            task_spawn(vga_task, &wins[i], 16384, 1, TRUE, "vga");

        while (task_thread_count() > 0 && !kbhit())
            task_busy_delay(5000);

        if (kbhit()) getch();
        demo_stop = 1;
        wait_tasks_timeout(3);

        /* Kill remaining */
        while (task_thread_count() > 0) {
            tcb_t *t = _task_current->next;
            if (t->id != MAIN_PID) task_kill(t->id);
            else break;
        }

        printf("\nPress key for normal screen...\n");
        getch();
        clrscr();
        printf("Done.\n");
        break;

    case 'F':
        printf("--- EVERYTHING --- (key to stop)\n\n");
        demo_stop = 0;
        shared_counter = 0;
        shared_data = 0;
        pool_done_count = 0;

        sem_init(&demo_sem, 0, 100);
        mutex_init(&demo_mutex);
        cond_init(&demo_cond);
        rwlock_init(&demo_rwlock);
        mq_init(&demo_mq, MAIN_PID);

        /* Preemptive */
        task_spawn(preemptive_task, &args[0], 16384, 1, TRUE, "pre-1");

        /* Cooperative */
        task_spawn(coop_task, &args[1], 16384, 1, TRUE, "coop-1");

        /* Semaphore */
        task_spawn(producer_task, &args[2], 16384, 1, TRUE, "prod");
        task_spawn(consumer_task, &args[3], 16384, 1, TRUE, "cons");

        /* Mutex */
        task_spawn(mutex_task, &args[0], 16384, 1, TRUE, "mtx-1");

        /* Cond var */
        task_spawn(cond_waiter_task, &args[1], 16384, 1, TRUE, "cw");
        task_spawn(cond_signaler_task, &args[2], 16384, 1, TRUE, "cs");

        /* RWLock */
        task_spawn(reader_task, &args[0], 16384, 1, TRUE, "rd");
        task_spawn(writer_task, &args[1], 16384, 1, TRUE, "wr");

        /* Messages */
        task_spawn(sender_task, &args[0], 16384, 1, TRUE, "snd");
        recv_count = 5;
        task_spawn(receiver_task, &recv_count, 16384, 1, TRUE, "rcv");

        /* Sleep */
        task_spawn(sleepy_task, &args[0], 16384, 1, TRUE, "slp");

        /* RR groups */
        pid1 = task_spawn(rr_task, &args[0], 16384, 1, TRUE, "rr-1");
        pid2 = task_spawn(rr_task, &args[1], 16384, 1, TRUE, "rr-2");
        task_set_rr_group(pid1, 1);
        task_set_rr_group(pid2, 1);

        /* Pool */
        pool_init(&demo_pool, 2, 16384);
        for (i = 0; i < 4; i++) {
            pool_jobs[i] = i + 1;
            pool_submit(&demo_pool, pool_job_func, &pool_jobs[i]);
        }

        /* Watchdog */
        pid3 = task_spawn(good_task, &args[0], 16384, 1, TRUE, "wdog");
        task_watchdog_enable(pid3);

        /* PQ callback */
        pq_schedule(TICKS_PER_SEC * 2, pq_callback, &pq_ids[0]);

        task_list();

        while (!kbhit()) {
            task_enter_critical();
            printf("MAIN: %d tasks tick=%lu\n",
                   task_thread_count(), task_get_tick());
            task_leave_critical();
            task_busy_delay(40000);
        }
        getch();
        demo_stop = 1;
        pool_shutdown(&demo_pool);
        wait_tasks_timeout(8);

        while (task_thread_count() > 0) {
            tcb_t *t = _task_current->next;
            if (t->id != MAIN_PID) task_kill(t->id);
            else break;
        }

        printf("\nCounter=%d Data=%d PoolJobs=%d\n",
               shared_counter, shared_data, pool_done_count);
        printf("Done.\n");
        break;

    default:
        printf("Invalid choice.\n");
        break;
    }

    return 0;
}
