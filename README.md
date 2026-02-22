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

## Credits

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
