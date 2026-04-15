#!/bin/bash
sudo killall engine || true
rm -rf logs
sudo ./engine supervisor ./rootfs-base > sup.out 2>&1 &
sleep 0.5
sudo ./engine start alpha ./rootfs-alpha "echo HELLO WORLD"
sleep 1
cat logs/alpha.log
echo "End of cat"
