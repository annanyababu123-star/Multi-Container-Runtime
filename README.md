# Multi-Container Runtime — OS-Jackfruit

## 1. Team Information

| Name | SRN |
|---|---|
| Annanya Babu | PES1UG24AM045 |
| Anarghyaa Kashyap | PES1UG24AM039 |

---

## 2. Build, Load, and Run Instructions

### Environment
- Ubuntu 22.04/24.04 VM (tested on kernel 6.8.0-107-generic)
- Secure Boot OFF
- No WSL

### Install Dependencies
```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### Prepare Root Filesystem
```bash
cd boilerplate
mkdir -p rootfs
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs
```

### Build
```bash
cd boilerplate
make clean && make
```

### Load Kernel Module
```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
```

### Start Supervisor
```bash
sudo ./engine supervisor ./rootfs &
```

### Launch Containers
```bash
# Background container
sudo ./engine start alpha ./rootfs "echo hello from alpha && sleep 30"

# Foreground container (waits for completion)
sudo ./engine run beta ./rootfs "echo hello from beta"
```

### List Containers
```bash
sudo ./engine ps
```

### View Logs
```bash
sudo ./engine logs alpha
sudo ./engine logs beta
```

### Stop a Container
```bash
sudo ./engine stop alpha
```

### Run Memory Limit Test
```bash
cp boilerplate/memory_hog boilerplate/rootfs/
sudo ./engine start memtest ./rootfs "/memory_hog" --soft-mib 5 --hard-mib 10
sleep 10
sudo dmesg | grep container_monitor | tail -20
```

### Run Scheduling Experiment
```bash
cp boilerplate/cpu_hog boilerplate/rootfs/
sudo ./engine start highpri ./rootfs "/cpu_hog" --nice -10
sudo ./engine start lowpri ./rootfs "/cpu_hog" --nice 10
sudo ./engine ps
top -b -n 1 | grep cpu_hog
```

### Clean Teardown
```bash
sudo ./engine stop highpri
sudo ./engine stop lowpri
sudo pkill -f "engine supervisor"
sudo rmmod monitor
```

### Verify No Zombies
```bash
ps aux | grep defunct
sudo dmesg | grep container_monitor | tail -5
```

---

## 3. Demo Screenshots

### Screenshot 1 — Kernel Module Loaded
`ls -l /dev/container_monitor` showing the character device created by the kernel module.

### Screenshot 2 — Multi-Container Supervision + Metadata
`sudo ./engine ps` showing two containers (`alpha`, `beta`) tracked by the supervisor with PID, state, soft/hard memory limits.

### Screenshot 3 — Bounded-Buffer Logging
`sudo ./engine logs alpha` and `sudo ./engine logs beta` showing output captured through the producer-consumer logging pipeline into per-container log files.

### Screenshot 4 — CLI and IPC
`sudo ./engine start` command being issued and the supervisor responding with the container PID, demonstrating the UNIX domain socket control channel.

### Screenshot 5 & 6 — Soft and Hard Memory Limit Enforcement
`sudo dmesg | grep container_monitor` showing:
- `SOFT LIMIT` warning when the memory_hog process exceeded 5 MiB RSS
- `HARD LIMIT` kill when the process exceeded 10 MiB RSS, with the supervisor metadata updated to `killed`

### Screenshot 7 — Scheduling Experiment
`top` output showing `highpri` (nice=-10) and `lowpri` (nice=10) CPU hog containers running simultaneously, with the high-priority container receiving a larger CPU share.

### Screenshot 8 — Clean Teardown
`ps aux | grep defunct` showing zero zombie processes after stopping all containers and the supervisor, confirming correct child reaping and thread cleanup.

---

## 4. Engineering Analysis

### 4.1 Isolation Mechanisms

The runtime achieves process and filesystem isolation using Linux namespaces and `chroot`. When launching a container, `clone()` is called with `CLONE_NEWPID`, `CLONE_NEWUTS`, and `CLONE_NEWNS` flags. `CLONE_NEWPID` gives the container its own PID namespace so the first process inside sees itself as PID 1, completely isolated from the host PID space. `CLONE_NEWUTS` allows the container to have its own hostname without affecting the host. `CLONE_NEWNS` creates a new mount namespace so filesystem mount/unmount operations inside the container do not propagate to the host.

After cloning, the child calls `chroot()` to change its root directory to the Alpine rootfs, and then mounts `/proc` inside the container. This means the container sees only its own processes when reading `/proc`, reinforcing PID namespace isolation.

What the host kernel still shares with all containers: the host kernel itself (there is only one kernel), the host network stack (we did not use `CLONE_NEWNET`), and the host IPC namespace. Containers are lightweight virtualization — they share the kernel but are isolated at the namespace level.

### 4.2 Supervisor and Process Lifecycle

A long-running parent supervisor is essential because containers are child processes that must be reaped when they exit. Without a persistent parent, child processes become orphans adopted by PID 1, losing the ability to track their exit status and clean up metadata.

The supervisor uses `clone()` to create each container as a child process. It maintains a linked list of `container_record_t` structs tracking each container's PID, state, memory limits, log path, and exit status. When a container exits, the kernel delivers `SIGCHLD` to the supervisor. The `SIGCHLD` handler calls `waitpid(-1, &status, WNOHANG)` in a loop to reap all exited children without blocking, updating their state to `CONTAINER_EXITED` or `CONTAINER_KILLED` depending on whether they exited normally or were terminated by a signal.

`SIGINT` and `SIGTERM` to the supervisor trigger orderly shutdown: all running containers receive `SIGTERM`, the logging thread is signaled to drain and exit, and all heap resources are freed before the supervisor exits.

### 4.3 IPC, Threads, and Synchronization

The project uses two IPC mechanisms:

**1. Pipes (logging channel):** Each container has a pipe where the child's `stdout` and `stderr` are redirected to the write end. The supervisor spawns a dedicated pipe reader thread per container that reads from the read end and pushes chunks into the bounded buffer. This separates the logging data path from the control path.

**2. UNIX Domain Socket (control channel):** CLI commands (`start`, `ps`, `logs`, `stop`) connect to the supervisor's UNIX socket at `/tmp/mini_runtime.sock`, send a `control_request_t` struct, and receive a `control_response_t`. This is a separate IPC path from the log pipes, as required.

**Bounded Buffer Synchronization:**
The bounded buffer uses a `pthread_mutex_t` to protect the shared `head`, `tail`, and `count` fields. Two condition variables (`not_empty`, `not_full`) are used so producers block when the buffer is full and consumers block when it is empty. Without the mutex, concurrent producers and consumers could corrupt the buffer indices. Without condition variables, threads would busy-wait, wasting CPU.

**Container Metadata:**
The `container_record_t` linked list is protected by a separate `metadata_lock` mutex. This is kept separate from the buffer lock to avoid deadlock — locking both simultaneously is never required, so the locks have a clean ordering.

**Possible race conditions without synchronization:**
- Two threads simultaneously updating `head`/`tail` in the buffer could cause data loss or index corruption.
- A `SIGCHLD` handler updating container state concurrently with the CLI handler reading it could produce torn reads.
- The linked list could be corrupted if a new container is inserted while `ps` is iterating it.

### 4.4 Memory Management and Enforcement

RSS (Resident Set Size) measures the portion of a process's memory that is currently held in physical RAM. It does not measure memory that has been swapped out, memory-mapped files that have not yet been faulted in, or shared library pages counted once per process. RSS is a useful proxy for actual physical memory pressure but is not a complete picture of a process's memory footprint.

Soft and hard limits serve different policy goals. A soft limit is a warning threshold — the process is allowed to continue running but an operator is notified that memory usage is elevated. This is useful for catching gradual memory leaks early. A hard limit is an enforcement threshold — the process is killed when it exceeds it, protecting the host from memory exhaustion.

Memory enforcement belongs in kernel space rather than user space for two reasons. First, a user-space monitor can be killed, paused, or delayed by the scheduler, creating a window where a runaway process consumes unbounded memory before the monitor can react. Second, reading `/proc/<pid>/status` from user space to get RSS requires a context switch and file I/O on every check interval, which is significantly more expensive than the kernel module's direct call to `get_mm_rss()` on the task's `mm_struct`. The kernel module also holds a lock-protected list and runs its timer callback in kernel context, making enforcement faster and more reliable.

### 4.5 Scheduling Behavior

In the scheduling experiment, two CPU-bound containers were launched simultaneously: `highpri` with `nice=-10` and `lowpri` with `nice=10`. The Linux Completely Fair Scheduler (CFS) uses nice values to adjust the weight of each task's virtual runtime. A lower nice value results in a higher weight, meaning the scheduler advances that task's virtual runtime more slowly relative to wall time, causing it to receive more CPU time.

The experiment showed `highpri` receiving approximately 2-3x the CPU share of `lowpri`, consistent with CFS weight calculations where nice=-10 has roughly 4x the weight of nice=10. This demonstrates that CFS maintains proportional fairness rather than strict priority — both containers make progress, but in proportion to their weights. This design achieves both fairness (no starvation) and responsiveness (high-priority work completes faster).

For I/O-bound vs CPU-bound workloads, the Linux scheduler naturally favors I/O-bound processes because they frequently block on I/O and wake up with accumulated CPU credit, allowing them to preempt CPU-bound tasks quickly. This improves interactive responsiveness without requiring explicit priority configuration.

---

## 5. Design Decisions and Tradeoffs

### Namespace Isolation
**Choice:** Used `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS` without network namespace isolation.
**Tradeoff:** Containers share the host network stack, so two containers could bind to the same port and conflict.
**Justification:** Adding `CLONE_NEWNET` requires setting up virtual ethernet pairs and a bridge, which was out of scope. PID, UTS, and mount isolation are sufficient to demonstrate container fundamentals.

### Supervisor Architecture
**Choice:** Single-process supervisor with a background logger thread and per-container pipe reader threads.
**Tradeoff:** All containers share one supervisor process — a supervisor crash kills all container tracking.
**Justification:** A single supervisor keeps the design simple and makes shared metadata easy to protect with one mutex. A multi-process supervisor would require shared memory for metadata, adding significant complexity.

### IPC and Logging
**Choice:** UNIX domain socket for control, pipes for logging, with a bounded buffer between pipe readers and the log writer thread.
**Tradeoff:** The bounded buffer adds latency between container output and log file writes.
**Justification:** Decoupling producers (pipe readers) from the consumer (log writer) prevents a slow disk write from blocking container output. The bounded buffer also provides backpressure, preventing unbounded memory growth if the log writer falls behind.

### Kernel Monitor
**Choice:** `mutex` rather than `spinlock` for the monitored list.
**Tradeoff:** Mutexes can sleep, so they cannot be used in all interrupt contexts. Spinlocks would be required if the list were accessed from a hard IRQ handler.
**Justification:** The timer callback runs in a softirq context where sleeping is allowed with a mutex. Since list operations (insert, remove, iterate) can take variable time depending on list length, a mutex is safer than a spinlock to avoid holding the CPU in a spin for extended periods.

### Scheduling Experiments
**Choice:** Used `nice` values via the `nice()` syscall inside the container child rather than `cgroups` CPU shares.
**Tradeoff:** `nice` affects the entire process but cannot enforce hard CPU usage caps like cgroups can.
**Justification:** `nice` values are simpler to implement and directly demonstrate CFS weight-based scheduling, which is the core concept being explored. Cgroups would require additional setup and are more appropriate for production enforcement than for a scheduling experiment.

---

## 6. Scheduler Experiment Results

### Experiment: Two CPU-bound containers with different priorities

| Container | Nice Value | Observed CPU% | Completion behavior |
|---|---|---|---|
| highpri | -10 | ~65-70% | Runs faster, gets larger CPU share |
| lowpri | +10 | ~25-30% | Runs slower, gets smaller CPU share |

Both containers ran the same `cpu_hog` workload simultaneously. The high-priority container consistently received approximately 2-3x the CPU time of the low-priority container, matching the CFS weight ratio for nice=-10 vs nice=+10.

**What this shows:** Linux CFS does not use strict priority preemption. Both processes always make progress (no starvation), but their CPU shares are proportional to their weights. This demonstrates CFS's core design goal: proportional fairness with a guarantee of forward progress for all runnable tasks.

The results also confirm that `nice` values have a nonlinear effect — the weight difference between nice=-10 and nice=+10 is much larger than between nice=0 and nice=1, which is by design to make priority adjustments meaningful at the extremes of the nice range.
