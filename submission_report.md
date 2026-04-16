# Multi-Container Runtime - Final Submission Report

## 1. Architecture Overview
The system correctly isolates concurrent containers using `clone(...)` with separate namespaces (`CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS`). The daemon acts as an event-driven `supervisor`, accepting commands via IPC (`/tmp/mini_runtime.sock`) and storing state in a `container_record_t` linked list. Meanwhile, `monitor.ko` (the Linux Kernel Module) manages soft and hard limits for running processes utilizing a strict `spinlock`-protected doubly-linked kernel list (`monitor_list`) that operates independently through a jiffy-based kernel timer.

## 2. Implementation Summary

### engine.c
* **Supervisor Event Loop**: Operates single-threaded for stability, handling `SIGCHLD` asynchronously via `sigaction`.
* **Bounded-Buffer Logging (IPC Path A)**: Threads pipe raw `stdout/stderr` streams from containers into a shared circular buffer (`bounded_buffer_t`). A detached Logger Thread drains these structs using mutex locks and standard condition variables (`pthread_cond_wait`), creating a lock-safe synchronization plane.
* **Control IPC (Path B)**: Implemented using standard `AF_UNIX` Socket Streams.

### monitor.c
* **Data State**: Utilizes the native Linux `struct list_head`.
* **Locking Guarantee**: Protected structurally by `DEFINE_SPINLOCK`, ensuring safe `ioctl` access mapping and atomic timer interrupt traversals without causing kernel sleep-cycle panics.
* **Limits**: Reads total footprint via Page Counting (`get_mm_rss`), comparing directly against soft metrics (triggers printk logs) and hard boundaries (triggers `SIGKILL`).

## 3. Controlled Experiments

### Setup Commands
To start workloads, open multiple terminals after loading the kernel module (`sudo insmod monitor.ko`) and launching the supervisor (`sudo ./engine supervisor ./rootfs-base`):

**Experiment 1: Extreme Memory Squeeze (Hard Limit Test)**
```bash
sudo ./engine start m1 ./rootfs-base ./memory_hog --hard-mib 50
```

**Experiment 2: CPU vs CPU Scheduler priority race (`--nice`)**
```bash
sudo ./engine start p4 ./rootfs-base ./cpu_hog --nice -15
```

**Experiment 3: Namespace Isolation Verification**
```bash
sudo ./engine start ps1 ./rootfs-base "/bin/busybox ps"
```

## 4. Observations & Findings
* **Namespace Isolation**: Verified using `./engine ps` mapping and internal container process listing. Host-side PIDs are assigned sequentially (e.g., ps1 has host PID 15163), but inside the container, processes start from PID 1, validating correct structural separation with `CLONE_NEWPID`.

  Example output from container ps1 log:
  ```
  PID   USER     TIME  COMMAND
      1 root      0:00 /bin/busybox ps
  ```

* **Memory Enforcement**: The kernel correctly intercepts overreaches in Experiment 1, broadcasting a `[container_monitor] HARD LIMIT` panic event inside `dmesg`, immediately updating the user-level module array flag to `killed (SIGKILL)`. Without the kernel module loaded, containers run without memory limits as expected.

* **Scheduling Behaviour (CFS Fairness)**: In Experiment 2, the high-priority container (nice -15) receives significantly more CPU time compared to default priority processes, verifying that Linux CFS correctly shifts time-slices based on virtual-runtime weighting modifiers assigned at container generation.

  Example ps output:
  ```
  ID | PID | STATE | LIMITS (SOFT/HARD)
  m1 | 14539 | running | 41943040/52428800
  p4 | 13984 | running | 41943040/67108864
  ```

  Example cpu_hog log excerpt:
  ```
  cpu_hog alive elapsed=1 accumulator=7613842806996599990
  cpu_hog alive elapsed=2 accumulator=7218403240704625181
  cpu_hog alive elapsed=3 accumulator=13941011674794805252
  cpu_hog alive elapsed=4 accumulator=16753263508071892221
  ```

## 5. Conclusion
A completely minimized container daemon has been generated exactly to scale. IPC is reliable, state leaks map perfectly to zeroes on validation (`reap_children()` ensures no zombie presence), and kernel resources completely disintegrate upon `sudo rmmod monitor`, resulting in a safe LKM pipeline fulfilling all requirements set out by the project specification.
