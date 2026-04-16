#!/bin/bash
set -e

echo "=== System Scheduler Experiment ==="
echo "Building the necessary components..."
make clean >/dev/null 2>&1
make >/dev/null 2>&1

echo "Killing any left-over engine processes..."
sudo killall engine 2>/dev/null || true
sleep 1 # Wait for them to release the kernel module

echo "Loading kernel module..."
sudo rmmod monitor 2>/dev/null || true
sudo insmod monitor.ko

echo "Starting the background supervisor..."
sudo ./engine supervisor ./rootfs-base > /dev/null 2>&1 &
SUP_PID=$!
sleep 1

echo "Preparing container rootfilespaces..."
sudo rm -rf rootfs-c1 rootfs-c2
cp -a rootfs-base rootfs-c1
cp -a rootfs-base rootfs-c2
cp cpu_hog rootfs-c1/
cp cpu_hog rootfs-c2/

echo "Launching Container 1: High Priority (nice = -10)..."
sudo ./engine start c1 ./rootfs-c1 "./cpu_hog 10" --nice -10

echo "Launching Container 2: Low Priority (nice = 19)..."
sudo ./engine start c2 ./rootfs-c2 "./cpu_hog 10" --nice 19

echo "Waiting for 10 seconds while CPU profiles accumulate..."
sleep 12

echo ""
echo "=== Experiment Results ==="
echo "[Container 1 Logs: High Priority / Nice -10]"
cat logs/c1.log

echo ""
echo "[Container 2 Logs: Low Priority / Nice 19]"
cat logs/c2.log

echo ""
echo "Cleaning up..."
sudo killall -TERM engine || true
wait $SUP_PID 2>/dev/null || true
sudo rmmod monitor
echo "Experiment complete."
