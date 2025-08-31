#!/bin/bash
set -e

cd figure13
# Setup DSA
sudo ./setup_dsa.sh -d dsa0
sudo ./setup_dsa.sh -d dsa0 -w 1 -m s -e 4 -f 1

# The pmem disk is configured following https://pmem.io/blog/2016/02/how-to-emulate-persistent-memory/
# The GRUB_CMDLINE_LINUX is appended with "memmap=64G!48G"

# The pmem disk is auto-mounted through crontab @reboot
# if ! mountpoint -q /mnt/pmemdir; then
#     mount -o dax /dev/pmem0 /mnt/pmemdir
# else
#     echo "/mnt/pmemdir is mounted already, skip mount."
# fi

# Now prepare the data
cd ../dataset
sudo ./prepare_dataset.sh
sudo ./load_dataset.sh

echo "Copying arxiv.rdb to pmem disk..."
sudo cp arxiv.rdb /mnt/pmemdir/redis1.rdb
sudo cp /mnt/pmemdir/redis1.rdb /mnt/pmemdir/redis2.rdb
sudo cp /mnt/pmemdir/redis1.rdb /mnt/pmemdir/redis3.rdb
sudo cp /mnt/pmemdir/redis1.rdb /mnt/pmemdir/redis4.rdb
echo "Experiment environment initialized!"