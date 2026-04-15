#!/bin/bash
set -e
make > /dev/null 2>&1
sudo rmmod monitor >/dev/null 2>&1 || true
sudo insmod monitor.ko || exit 1

sudo ./engine supervisor ./rootfs-base > supervisor.log 2>&1 &
SUP_PID=$!
sleep 1

rm -rf rootfs-test1 rootfs-test2
cp -a rootfs-base rootfs-test1
cp -a rootfs-base rootfs-test2
cp cpu_hog rootfs-test1/
cp cpu_hog rootfs-test2/

echo "Starting container 1 (background)"
sudo ./engine start test1 ./rootfs-test1 ./cpu_hog

echo "Running ps"
sudo ./engine ps

echo "Stopping container 1"
sudo ./engine stop test1
sleep 1

echo "Starting container 2 (foreground with timeout)..."
# We expect this to be cleanly cancelled by timeout!
# timeout sends SIGTERM, which kills engine. Wait, we want to test SIGINT!
# We can run it in background, then send SIGINT
sudo ./engine run test2 ./rootfs-test2 ./cpu_hog > run2.log 2>&1 &
RUN_PID=$!
sleep 1
sudo kill -INT $RUN_PID
wait $RUN_PID || true

echo "----- supervisor logs -----"
cat logs/test1.log || true
cat logs/test2.log || true

sudo ./engine ps

sudo kill -TERM $SUP_PID
wait $SUP_PID || true
sudo rmmod monitor
echo "Done"
