# OS Mini-Project: Container Runtime Engine
**Student/Group Information:** 
- Name: Arjun G Kanagal
- SRN: `PES1UG24CS922`

- Name: Anirudh Ramesh
- SRN: `PES1UG24CS929`

## Setup and Run Instructions
Follow these steps to seamlessly build the kernel dependencies and test the architecture locally:

```bash
cd boilerplate

# Build the application suite and workload testing suites
make clean && make

# Prepare the test rootfs bounds. You must explicitly copy workloads into the environments!
rm -rf rootfs-test && cp -a rootfs-base rootfs-test
cp cpu_hog rootfs-test/

# Load the kernel monitoring module 
sudo insmod monitor.ko

# Launch the Daemon securely in the background
sudo ./engine supervisor ./rootfs-base &
```

## Example Lifecycle Usage
```bash
# Launch a background workload container
sudo ./engine start c1 ./rootfs-test ./cpu_hog

# Verify it is currently registered and RUNNING
./engine ps

# Wait several seconds, then inspect the container's generated logs
./engine logs c1

# Block the terminal while attaching to a new container instance. 
# Feel free to trigger Ctrl+C directly against this prompt!
sudo ./engine run c2 ./rootfs-test ./cpu_hog

# Terminate execution gracefully
sudo ./engine stop c1
sudo killall -TERM engine
sudo rmmod monitor
```

## Demonstration Screenshots
Please insert the required screenshots below documenting runtime verification correctness (please replace each block with markdown images capturing your local OS validation):

1. `kernel_log.png` - `dmesg` output demonstrating module loading and container registration.
2. `engine_ps.png` - Output demonstrating multiple running/stopped containers.
3. `engine_run.png` - Foreground container execution correctly blocking.
4. `engine_stop.png` - Successful invocation of the stop command.
5. `engine_logs.png` - Live piped output representing `/bin/sh` or a C workload.
6. `memory_hog.png` - Demonstrating memory restrictions kicking in.
7. `cpu_hog_high.png` - Priority CPU testing (-10 nice value).
8. `cpu_hog_low.png` - Priority CPU testing (19 nice value).

--- 

## Engineering Analysis

**1. Namespace Isolation (`MS_PRIVATE`)**
The intrinsic `clone()` execution establishes core container boundaries (e.g. `CLONE_NEWPID` and `CLONE_NEWNS`) to logically fragment process IDs and virtualization trees. However, modern host Operating Systems typically configure the true hardware root (`/`) with `MS_SHARED` propagation. Consequently, any native modification of containerized mounts (such as forcefully mounting a fresh `/proc` tree) silently leaks across the virtualization threshold and mounts pseudo-files directly back onto the Ubuntu Host hardware stack. By securely enforcing `mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL)` *before* issuing our `chroot()` layer lock, we cleanly sever this bidirectional propagation link and isolate all mounts precisely inward.

**2. Kernel Tracing & Irq-Safe Concurrency (`spin_lock_bh`)**
By attaching `monitor.ko` as a Loadable Kernel Module (LKM), we maintain exceptionally accurate memory tracking (such as RSS metric aggregation bounding) disconnected from User-space thrashing. The kernel intercepts memory scaling bounds mapped to a synchronized `monitor_list`. This list structure is heavily shielded explicitly using `spin_lock_bh` algorithms rather than traditional semaphores. Bottom-half IRQ spinlocks deliberately stall internal software interrupt processing while locked, mathematically preventing local timers from preempting the processor during `ioctl(MONITOR_REGISTER)` link adjustments, effectively dodging race-conditions when containers register initialization sequences simultaneously.

**3. State Tracking and Blocking IPC Socket Pipelines (`engine run`)**
To accurately broadcast state transitions (`SIGCHLD` process cleanup vs active running constraints), our User-space daemon tightly unifies UNIX Domain Socket endpoints with internal object graphs. Our baseline iteration managed blocking (`CMD_WAIT`) features blindly utilizing an infinite 500ms `poll()` loop loop, needlessly squandering frontend client-side resources. By restructuring our system into an asynchronous hook graph, the daemon directly steals the active `AF_UNIX` client socket descriptor (`wait_fd`) and suspends it quietly in memory. When the host OS ultimately routes the `SIGCHLD` process death signal natively, our internal daemon rapidly shifts through `reap_children()`, locates the frozen socket map, dumps the strict process conclusion code directly across the byte stream instantly, and finalizes the socket! 

**4. CPU Scheduling Experiments (Task 5 Analysis)**
During isolated benchmark analysis mapping the local OS Completely Fair Scheduler (CFS) logic paths, we stressed extreme process bounds utilizing concurrent `cpu_hog` containers attached to aggressive priority nodes.
*   **Container 1 (High Priority: `nice = -10`)**: The hardware CFS vastly amplified core cycle distributions in favor of the first container. By studying the `accumulator` print rates, Container 1 continuously rotated processing logic layers measurably faster than the baseline counterpart without incurring standard task latency delays.
*   **Container 2 (Low Priority: `nice = 19`)**: Surrendering all primary calculation timeshares, Container 2 was aggressively preempted by the CFS architecture. Overall `accumulator` loop execution staggered dramatically backwards in physical time intervals matching the exact intended consequences of `--nice` flags successfully cascading down across our container barriers!

---

## Design Decisions and Key Tradeoffs

**1. Background Log File Descriptors Array Caching vs High-Frequency File Operation IO**
*Decision:* Rather than manually resolving `fopen()`, array appending, and `fclose()` executions repetitively on a per-`LOG_CHUNK` chunk level (which predictably induces severe VFS (Virtual File System) kernel execution bottlenecks for containers dynamically generating tens of thousands of lines of terminal output instantly), our background `logging_thread` constructs a dedicated `log_cache_item_t` file descriptor registry maintaining 128 asynchronous open handles simultaneously. 
*Tradeoff:* Constructing this registry forces the supervisor into slightly elevated continuous memory footprints, and mandates an "EOF marker pattern" (a simulated packet bearing `length=0` dispatched organically when `pipe_reader_thread` receives an explicit internal End of File condition upon program termination) to cleanly resolve dangling descriptor references dynamically. Ultimately, bypassing heavy Kernel IO mapping sequences cleanly overrides the minor array-context memory costs.

**2. Persistent Object Caching for Daemon Diagnostics vs Implicit Resource Freezing**
*Decision:* Upon container lifecycle termination (`reap_children` completing and flagging `CONTAINER_EXITED`), the majority of runtime memory spaces typically purge dynamic allocated structures instantly. We intentionally rejected unlinking inactive target structures from the primary daemon `supervisor_ctx_t` linked list mapping blocks. 
*Tradeoff:* Retaining dormant object pointers scales our daemon memory footprint marginally proportionally matching total lifetime execution commands across a session. However, this perfectly serves user diagnostics logically. By forcing historical node retention, clients querying `./engine ps` hours after critical workloads inherently crash mathematically reconstruct exact failure flags, exit signals, and initialization metadata.
