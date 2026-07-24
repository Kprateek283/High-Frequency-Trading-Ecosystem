#!/bin/bash
set -e

echo "Backing up /etc/default/grub to /etc/default/grub.bak..."
cp /etc/default/grub /etc/default/grub.bak

echo "Applying isolcpus, nohz_full, and rcu_nocbs to cores 1-8 (gateway 1,3,5,7 + engine 2,4,6,8; aux threads stay on 0,9)..."
# This safely appends the parameters inside the GRUB_CMDLINE_LINUX_DEFAULT quotes
sed -i 's/GRUB_CMDLINE_LINUX_DEFAULT="\(.*\)"/GRUB_CMDLINE_LINUX_DEFAULT="\1 isolcpus=1-8 nohz_full=1-8 rcu_nocbs=1-8"/' /etc/default/grub

echo "Updating GRUB..."
update-grub

echo ""
echo "============================================================"
echo "SUCCESS: CPU Isolation parameters applied to GRUB."
echo "You MUST REBOOT your machine for the kernel to isolate the cores."
echo "After reboot, verify with: cat /proc/cmdline"
echo "============================================================"
