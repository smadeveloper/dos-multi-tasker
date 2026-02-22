# DJTASK - Preemptive Multitasker for DJGPP/MS-DOS

A fully-featured cooperative and preemptive multitasking kernel for 32-bit DOS programs built with DJGPP. Written in C and i386 assembly, DJTASK provides a rich set of concurrency primitives comparable to modern RTOS kernels — all running on bare DOS.

## Features

### Core Scheduler
- **Preemptive multitasking** via IRQ0 (PIT timer) tick counting with polling-based context switching
- **Cooperative multitasking** via explicit `task_yield()` calls
- **Priority-based scheduling** with configurable per-task priority levels
- **Round-robin groups** for fair time-sharing among equal-priority tasks
- **Named tasks** for easy identification and debugging

### Synchronization Primitives
- **Semaphores** — counting semaphores with blocking wait, try-wait, and signal
- **Mutexes** — binary locks with ownership tracking and try-lock support
- **Condition Variables** — wait/signal/broadcast with automatic mutex release and re-acquisition
- **Read/Write Locks** — multiple concurrent readers, exclusive writer access

### Communication
- **Message Queues** — per-task mailboxes with blocking receive, non-blocking try-receive, and queue depth query
- **Configurable message size** (default 32 bytes payload per message, 16 messages per queue)

### Timing & Deferred Execution
- **Sleep** — put tasks to sleep for a specified number of ticks or milliseconds
- **Priority Queue** — schedule one-shot callbacks to fire after a delay (tick or millisecond granularity)
- Automatic sleeper wake-up and callback dispatch via per-tick housekeeping

### Task Pool
- **Worker thread pool** with job submission queue
- Configurable number of workers and stack size
- Clean shutdown with graceful worker termination

### Safety & Debugging
- **Stack guard** — magic sentinel values at the bottom of every task stack; periodic overflow detection
- **Watchdog timer** — detects stuck tasks that haven't run within a configurable timeout
- **Task list dump** — prints PID, name, priority, state, active flag, round-robin group, and stack guard status for every task

### VGA Text UI
- **Windowed text output** — create bordered, colored windows on the 80×25 VGA text screen
- Per-window cursor tracking, automatic scrolling, and printf-style formatted output
- Each task can own its own window for isolated visual output




## Building

Requires [DJGPP](http://www.delorie.com/djgpp/) cross-compiler or native DOS DJGPP installation.

```bash
# Using make
make

# Or manually
gcc -Wall -O2 -g -o multitask.exe main.c main.S

```

## Running

Run multitask.exe in a DOS environment (real DOS, DOSBox, FreeDOS, or Windows 9x DOS prompt).
=============================================
  DJGPP Multitasker - All Features Demo
=============================================

 1. Preemptive
 2. Cooperative
 3. Semaphore
 4. Mutex
 5. Condition Variable
 6. Read/Write Lock
 7. Messages
 8. Sleep
 9. Priority Queue
 A. Task Pool
 B. Round-Robin Groups
 C. Watchdog
 D. Stack Guard
 E. VGA Text UI
 F. EVERYTHING

## Demo Descriptions
1	Preemptive	4 infinite-loop tasks preempted by timer ticks
2	Cooperative	4 tasks that voluntarily yield and then exit
3	Semaphore	Producer/consumer pattern with counting semaphore
4	Mutex	3 tasks incrementing a shared counter under mutex
5	Condition Variable	3 waiters wake when signaler broadcasts
6	Read/Write Lock	3 readers + 1 writer with concurrent read access
7	Messages	2 senders posting to a queue, 1 blocking receiver
8	Sleep	2 tasks sleeping for 1 second intervals
9	Priority Queue	3 timed callbacks firing at +1s, +2s, +3s
A	Task Pool	3 worker threads processing 8 submitted jobs
B	Round-Robin	2 groups of 2 tasks each, yielding within group
C	Watchdog	Tasks that kick the watchdog vs. stuck detection
D	Stack Guard	Stack overflow detection via sentinel checks
E	VGA Text UI	4 colored windows with independent scrolling output
F	Everything	All of the above running simultaneously

## API Reference
Initialization & Task Management

C

int         task_init(int main_priority);
int         task_spawn(void (*func)(void *), void *arg, int stack_size,
                       int priority, int active, const char *name);
int         task_kill(int pid);
void        task_yield(void);
void        task_smart_yield(void);
void        task_busy_delay(int iterations);
int         task_getpid(void);
int         task_thread_count(void);
const char *task_getname(void);
void        task_list(void);

Sleep

C

void task_sleep(unsigned long ticks);
void task_sleep_ms(unsigned long ms);

Semaphore

C

void sem_init(semaphore_t *sem, int initial, int max_val);
int  sem_wait(semaphore_t *sem);
int  sem_trywait(semaphore_t *sem);
int  sem_signal(semaphore_t *sem);
int  sem_getcount(semaphore_t *sem);

Mutex

C

void mutex_init(mutex_t *mtx);
int  mutex_lock(mutex_t *mtx);
int  mutex_trylock(mutex_t *mtx);
int  mutex_unlock(mutex_t *mtx);

Condition Variable

C

void cond_init(condvar_t *cv);
int  cond_wait(condvar_t *cv, mutex_t *mtx);
int  cond_signal(condvar_t *cv);
int  cond_broadcast(condvar_t *cv);

Read/Write Lock

C

void rwlock_init(rwlock_t *rw);
int  rwlock_read_lock(rwlock_t *rw);
int  rwlock_read_unlock(rwlock_t *rw);
int  rwlock_write_lock(rwlock_t *rw);
int  rwlock_write_unlock(rwlock_t *rw);

Message Queue

C

void mq_init(msgqueue_t *mq, int owner_pid);
int  mq_send(msgqueue_t *mq, int type, const void *data, int len);
int  mq_receive(msgqueue_t *mq, message_t *out);
int  mq_tryreceive(msgqueue_t *mq, message_t *out);
int  mq_pending(msgqueue_t *mq);

Priority Queue (Timed Callbacks)

C

int pq_schedule(unsigned long delay_ticks, void (*cb)(void *), void *arg);
int pq_schedule_ms(unsigned long ms, void (*cb)(void *), void *arg);

Task Pool

C

void pool_init(taskpool_t *pool, int num_workers, int stack_size);
int  pool_submit(taskpool_t *pool, void (*func)(void *), void *arg);
void pool_shutdown(taskpool_t *pool);

Round-Robin Groups

C

void task_set_rr_group(int pid, int group);
void task_rr_yield(void);

Watchdog

C

void task_watchdog_enable(int pid);
void task_watchdog_disable(int pid);
void task_watchdog_kick(void);

VGA Text UI

C

void vga_init(void);
void vga_clear_screen(void);
int  vga_create_window(int x, int y, int w, int h,
                       unsigned char attr, const char *title);
void vga_window_print(int win_id, const char *fmt, ...);
void vga_clear_window(int win_id);

Critical Sections

C

void task_enter_critical(void);
void task_leave_critical(void);

File Structure

text

├── main.c          C kernel + all features + demos
├── main.S          i386 assembly (IRQ handler, context switch, yield)
├── Makefile        Build script
├── thanks          Contributors list
└── README.md       This file

How It Works

    task_init() hooks IRQ0 to count timer ticks and sets up the main thread as the first task in a circular linked list.

    task_spawn() allocates a stack, writes a synthetic context frame (FPU state, flags, segment registers, general-purpose registers, and a return address pointing to task_wrapper), and inserts the new task into the circular list.

    task_wrapper is the true entry point for every spawned task. It pops the function pointer and argument from the stack, calls the task function, and jumps to task_dead_yield when the function returns.

    task_check_preempt() is called periodically from task_busy_delay(). When a timer tick has occurred, it runs housekeeping (waking sleepers, firing priority queue callbacks) and then does a priority countdown. When the countdown reaches zero, it falls through to task_yield().

    task_yield() saves the full register state (including FPU) onto the current stack, walks the circular task list to find the next READY + active task, switches _task_current, and restores the new task's saved state.

    task_dead_yield() unlinks the dying task from the circular list, switches to the next ready task's stack, and then frees the dead task's stack and TCB from the safety of the new stack.

Limitations

    Polling-based preemption: Tasks must call task_busy_delay() or task_check_preempt() for preemptive switching to occur. A task stuck in a tight loop without these calls will not be preempted.
    Single address space: All tasks share the same flat 32-bit address space. There is no memory protection between tasks.
    Not SMP-aware: Designed for single-processor DOS systems.
    Stack size is fixed: Each task's stack is allocated at spawn time and cannot grow.
    Timer resolution: The default DOS PIT runs at ~18.2 Hz. Timing granularity for sleep and priority queue is approximately 55ms per tick.

Credits

This project builds on concepts from the LWP (Light Weight Process) library (by Josh Turpen	and others) for DJGPP. See the thanks file for the full list of contributors.
License

text

Disclaimer: THIS SOFTWARE IS PROVIDED "AS IS" WITHOUT WARRANTY, IMPLIED
OR OTHERWISE. I make no promises that this software will even do what it
is intended to do or claims to do. USE AT YOUR OWN RISK.

This software is provided to the public domain. The only restriction is
that anything that is created using this software has to include a list
of names (in the included "thanks" file) crediting the contributors to
this software, as they have also contributed to your software.
