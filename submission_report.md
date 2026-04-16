# Multi-Container Runtime - Final Submission Report

## 1. Team Information
- **Anirudh Ramesh** - SRN: PES1UG24CS929
- **Arjun Gopinath Kanagal** - SRN: PES1UG24CS922

---

## 2. Build, Load, and Run Instructions
*Execute the following step-by-step from a fresh Ubuntu VM:*

```bash
# 1. Build User-Space binaries and Kernel Module
make

# 2. Load the Memory Monitor LKM
sudo insmod monitor.ko

# 3. Start the Supervisor Daemon in the background
sudo ./engine supervisor ./rootfs-base &

# 4. Create an isolated writable root filesystem for the target container
sudo cp -a ./rootfs-base ./rootfs-alpha

# 5. Launch a container with soft and hard memory bounds
sudo ./engine start p1 ./rootfs-alpha /cpu_hog --nice -15 --soft-mib 40 --hard-mib 80

# 6. Verify its state via CLI
sudo ./engine ps

# 7. Observe the logs captured by the bounded-buffer
sudo ./engine logs p1

# 8. Unload runtime cleanly
sudo ./engine stop p1
sudo killall engine
sudo rmmod monitor
```

---

## 3. Demo with Screenshots 
*(Captions outlining the terminal outputs demonstrating rubric completeness)*

**1. Multi-container supervision & 2. Metadata tracking**
Running `sudo ./engine ps` accurately lists states mapped natively from the supervisor array in real-time. 
```text
ID | PID | STATE | LIMITS (SOFT/HARD)
p1 | 99835 | running | 41943040/67108864
p2 | 99857 | running | 41943040/52428800
ps1 | 99830 | exited | 41943040/67108864
```

**3. Bounded-buffer logging & 4. CLI and IPC verification**
Using `/tmp/mini_runtime.sock`, IPC commands seamlessly flow. Running `sudo ./engine run ps1 ./rootfs-alpha "/bin/busybox ps"` proves that the bounded buffer logger intercepts standard pipes flawlessly:
```text
PID   USER     TIME  COMMAND
    1 root      0:00 /bin/busybox ps
```

**5. Soft-limit warning & 6. Hard-limit enforcement**
Under `--hard-mib 42`, the kernel strictly interrupts and destroys bloated containers cleanly. `dmesg` reflects soft-warning thresholds first:
```text
[21085.679901] [container_monitor] SOFT LIMIT container=p3 pid=101096 rss=42336256 limit=41943040
[21086.704903] [container_monitor] HARD LIMIT container=p3 pid=101096 rss=50724864 limit=44040192
```

**7. Scheduling experiment & 8. Clean teardown**
CFS re-prioritization successfully occurs. Upon daemon shutdown, zombie counts map internally to zero due to `reap_children()` intercepts via `SIGCHLD`.

---

## 4. Engineering Analysis

1. **Isolation Mechanisms:** 
   Our runtime utilizes `clone()` with `CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS`. This constructs structurally disparate namespaces. While the kernel scheduler and physical memory plane remain shared to ensure Host operation, internal mounts to `/proc` strictly blind the container from root processes.

2. **Supervisor and Process Lifecycle:** 
   A long-running daemon dictates lifecycle via an event-driven `container_record_t` list. When `SIGCHLD` arrives, the kernel has already destroyed the metadata structure. The daemon leverages asynchronous interceptors to `waitpid` reliably, averting zombie allocations securely.

3. **IPC, Threads, and Synchronization:** 
   We employ concurrent pipes feeding a `bounded_buffer_t` struct locked via `mutex` alongside `pthread_cond_wait`. This ensures the consumer Logger thread never burns core cycles spinning, and the Producer pipes are safely halted if bounds are fully consumed—solving classic deadlocking race-conditions completely.

4. **Memory Management and Enforcement:** 
   RSS (Resident Set Size) natively reads actual mapped pages actively utilized, unlike Virtual Memory (VSZ) which falsely maps allocated-yet-untouched swap. Soft limits permit elastic metrics before prompting a logged warning. Hard limits map immediately to a non-maskable `SIGKILL` initiated organically by the kernel. This cannot be circumvented from user-space logic contexts.

5. **Scheduling Behavior:** 
   Adjusting prioritizing weights utilizing `--nice` directly biases the Linux Completely Fair Scheduler (CFS) tree. Giving a heavier CPU-bound load a highly aggressive modifier (-15) causes its virtual-runtime to age extremely slowly, stealing cycles dynamically without blocking standard system daemon I/O tasks.

---

## 5. Design Decisions and Tradeoffs

* **Namespace Isolation**: Relied on `chroot()` directly combined with clone isolation over `pivot_root()`. *Tradeoff*: While `pivot_root` safely segregates pathing, it creates complexities with Alpine `sh` symlinks against Windows Host-Mappings.
* **Supervisor Architecture**: Kept logic purely single-threaded aside from the logger. *Tradeoff*: Blockages are more risky, but race-conditions over linked lists are largely mitigated natively.
* **IPC/Logging Design**: Chose `AF_UNIX` streams over shared memory queues. *Tradeoff*: Unix sockets cause slight overhead, but permit structured packet parsing safely out-of-the-box.
* **Kernel Monitor**: Ran a pure 2000ms `timer_list` jitter check. *Tradeoff*: Between polls, memory runs unbounded natively. However, this protects the OS from interrupt starvation.

---

## 6. Scheduler Experiment Results

**Experiment Output Validation:**
When running two identical CPU Hog processes under differing `nice` parameters, CFS radically segregated throughput limits.
* **Default Priority Hog:** `accumulator=7218403240704625181`
* **Nice -15 High Priority Hog:** `accumulator=16753263508071892221`

**Linux Scheduling Analysis:**
The Linux `SCHED_OTHER` scheduler maps these bounds through virtual elapsed execution logic mapping (`vruntime`). The high-priority hog's time increments exceptionally slower, prompting the scheduler map it as 'historically starved', continuously granting it preferential physical CPU cycles over the default process.
