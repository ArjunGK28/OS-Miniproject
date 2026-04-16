#!/bin/bash
set -e

clear
echo "========================================================"
echo "    LINUX CFS SCHEDULING EXPERIMENT (10 SECONDS)        "
echo "========================================================"
echo "Running setup and launching isolated supervisor..."
sudo killall -TERM engine 2>/dev/null || true
sudo rmmod monitor 2>/dev/null || true
make clean >/dev/null 2>&1 && make >/dev/null 2>&1
sudo insmod monitor.ko
sudo ./engine supervisor ./rootfs-base > /dev/null 2>&1 &
SUP_PID=$!
sleep 1
sudo rm -rf rootfs-c1 rootfs-c2
cp -a rootfs-base rootfs-c1 && cp -a rootfs-base rootfs-c2
cp cpu_hog rootfs-c1/ && cp cpu_hog rootfs-c2/

echo -e "\n[+] Launching 'c1' (High Priority: nice = -10)..."
sudo ./engine start c1 ./rootfs-c1 "./cpu_hog 10" --nice -10 >/dev/null 2>&1
echo "[+] Launching 'c2' (Low Priority : nice =  19)..."
sudo ./engine start c2 ./rootfs-c2 "./cpu_hog 10" --nice 19 >/dev/null 2>&1

echo -n "[*] Math accumulators running. Please wait 10 seconds"
for i in {1..12}; do
    echo -n "."
    sleep 1
done
echo " Done!"

C1_RES=$(grep "done" logs/c1.log | awk -F'=' '{print $3}' | tr -d '\r')
C2_RES=$(grep "done" logs/c2.log | awk -F'=' '{print $3}' | tr -d '\r')

echo ""
echo "--------------------------------------------------------"
echo "                    FINAL RESULTS                       "
echo "--------------------------------------------------------"
echo -e "Process\t\tNice Value\tOperations Completed (10s)"
echo -e "c1\t\t-10\t\t$C1_RES"
echo -e "c2\t\t 19\t\t$C2_RES"
echo "--------------------------------------------------------"
echo ""
echo "📸 You can take your screenshot of the table above now!"
echo ""

sudo killall -TERM engine 2>/dev/null || true
wait $SUP_PID 2>/dev/null || true
sudo rmmod monitor
