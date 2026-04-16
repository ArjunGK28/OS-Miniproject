#!/bin/bash
set -e

# Clear screen
clear
echo "=========================================="
echo "📸 OS Mini-Project Screenshot Assistant!"
echo "This script will pause at the exact moments you need to take your screenshots."
echo "Press [ENTER] to advance to the next step when ready."
echo "=========================================="
read -p "Press [Enter] to start..."

# 1. Setup
echo "Cleaning up any old sessions and loading the kernel module..."
sudo killall -TERM engine 2>/dev/null || true
sudo rmmod monitor 2>/dev/null || true
make clean >/dev/null 2>&1 && make >/dev/null 2>&1
sudo insmod monitor.ko

# Start supervisor
sudo ./engine supervisor ./rootfs-base > /dev/null 2>&1 &
SUP_PID=$!
sleep 1
echo "[Kernel Module & Supervisor initialized]"

# Prepare rootfs
rm -rf rootfs-c1 rootfs-c2 rootfs-mem rootfs-io
cp -a rootfs-base rootfs-c1
cp -a rootfs-base rootfs-c2
cp -a rootfs-base rootfs-mem
cp -a rootfs-base rootfs-io
cp cpu_hog io_pulse rootfs-c1/
cp cpu_hog io_pulse rootfs-c2/
cp memory_hog rootfs-mem/
cp io_pulse rootfs-io/

# 1. Multi-Container Supervision
echo "Starting multiple independent workloads..."
sudo ./engine start c1 ./rootfs-c1 "./cpu_hog 100"
sudo ./engine start c2 ./rootfs-c2 "./io_pulse 100"
sleep 1
echo "--------------------------------------------------------"
echo "📸 [SCREENSHOT 1 / 8]: Multi-Container Supervision"
echo "Please take your screenshot of the 'Started successfully' messages above!"
echo "--------------------------------------------------------"
read -p "Press [Enter] when done to move to Metadata Tracking..."

# 2. Metadata Tracking
sudo ./engine ps
echo "--------------------------------------------------------"
echo "📸 [SCREENSHOT 2 / 8]: Metadata Tracking (ps)"
echo "Please take your screenshot showing the container IDs, PIDs, and Statuses above!"
echo "--------------------------------------------------------"
read -p "Press [Enter] when done to move to the Logging System..."

# 3. Logging System
sudo ./engine logs c1
echo "--------------------------------------------------------"
echo "📸 [SCREENSHOT 3 / 8]: Logging System"
echo "Please take your screenshot showing the live pipe output dumped from engine logs!"
echo "--------------------------------------------------------"
read -p "Press [Enter] when done to move to CLI + IPC..."

# 4. CLI + IPC
sudo ./engine stop c2
echo "--------------------------------------------------------"
echo "📸 [SCREENSHOT 4 / 8]: CLI + IPC"
echo "Please take your screenshot showing the stop command successfully resolving above!"
echo "--------------------------------------------------------"
read -p "Press [Enter] when done to move to Memory Allocations..."

# 5. Soft Memory Limit
echo "Starting the Memory Hog (Allocating 8MB per second)..."
sudo ./engine start mem1 ./rootfs-mem "./memory_hog"
echo "Waiting 6 seconds for the Soft Limit threshold to be breached..."
sleep 6

# Dump kernel buffer for Soft Limit
echo "KERNEL RING BUFFER (dmesg):"
sudo dmesg | tail -n 3
echo "--------------------------------------------------------"
echo "📸 [SCREENSHOT 5 / 8]: Soft Memory Limit"
echo "Please take your screenshot containing the Kernel dmesg Warning indicating the Soft Limit threshold breach!"
echo "--------------------------------------------------------"
read -p "Press [Enter] when done to wait for Hard Limit kill..."

# 6. Hard Memory Limit
echo "Waiting an additional 5 seconds for Memory Hog to violate the Hard Limit and be killed..."
sleep 5
sudo ./engine ps
echo "--------------------------------------------------------"
echo "📸 [SCREENSHOT 6 / 8]: Hard Memory Limit"
echo "Please take your screenshot showing the Memory Hog container status updated to KILLED!"
echo "--------------------------------------------------------"
read -p "Press [Enter] when done to execute Scheduling Experiments..."

# 7. Scheduling Experiment
echo "Running Scheduling Experiments (10 seconds)..."
sudo ./engine start high_cpu ./rootfs-c1 "./cpu_hog 10" --nice -10
sudo ./engine start low_cpu ./rootfs-c2 "./cpu_hog 10" --nice 19
sleep 12
echo "High Priority (--nice -10) Log:"
sudo ./engine logs high_cpu
echo "Low Priority (--nice 19) Log:"
sudo ./engine logs low_cpu
echo "--------------------------------------------------------"
echo "📸 [SCREENSHOT 7 / 8]: Scheduling Experiment"
echo "Please take your screenshot comparing the final LCG Accumulators computed by the High vs Low priority containers."
echo "--------------------------------------------------------"
read -p "Press [Enter] when done to process the final System Cleanup..."


# 8. Clean Teardown
echo "Terminating Supervisor and gracefully unlinking the Kernel hook..."
sudo killall -TERM engine 2>/dev/null || true
wait $SUP_PID 2>/dev/null || true
sudo rmmod monitor
echo "Checking for remaining zombie supervisor instances..."
ps aux | grep engine | grep -v grep || true
echo "--------------------------------------------------------"
echo "📸 [SCREENSHOT 8 / 8]: Clean Teardown"
echo "Please take your absolute final screenshot proving the 'ps aux' list holds no orphaned containers!"
echo "--------------------------------------------------------"

echo "🎉 ALL DONE! Your project is 100% complete and ready!"
