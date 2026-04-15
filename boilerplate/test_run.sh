#!/bin/bash
set -e
echo "Building project..."
make clean > /dev/null 2>&1
make > /dev/null 2>&1
echo "Loading kernel module..."
sudo rmmod monitor >/dev/null 2>&1 || true
sudo insmod monitor.ko || exit 1
echo "Kernel module loaded."

./engine supervisor ./rootfs-base &
SUP_PID=$!
sleep 1

echo "Copying rootfs versions..."
rm -rf rootfs-test1 rootfs-test2
cp -a rootfs-base rootfs-test1
cp -a rootfs-base rootfs-test2
cp cpu_hog rootfs-test1/
cp cpu_hog rootfs-test2/

echo "Starting container in background..."
sudo ./engine start test1 ./rootfs-test1 ./cpu_hog
sleep 1

echo "PS Output:"
sudo ./engine ps

echo "Stopping background container..."
sudo ./engine stop test1
sleep 1

echo "PS Output after stop:"
sudo ./engine ps

echo "Starting foreground container, testing run blocking..."
timeout 3 sudo ./engine run test2 ./rootfs-test2 ./cpu_hog > run_output.txt 2>&1 || true
echo "Run test complete."

echo "Logs for test1:"
sudo ./engine logs test1

echo "Logs for test2:"
sudo ./engine logs test2

sudo kill -TERM $SUP_PID
wait $SUP_PID || true
sudo rmmod monitor

echo "All tests completed!"
