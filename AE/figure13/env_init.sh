#!/bin/bash
set -e

# first setup DSA, init hugepages and disable numa balancing
echo 20480 > /proc/sys/vm/nr_hugepages
echo 0 | sudo tee /proc/sys/kernel/numa_balancing
./setup_dsa.sh -d dsa0
./setup_dsa.sh -d dsa0 -w 1 -m s -e 4 -f 1

# The pmem disk is configured following https://pmem.io/blog/2016/02/how-to-emulate-persistent-memory/
# The GRUB_CMDLINE_LINUX is appended with "memmap=64G!48G"

# Now mount the pmem disk
if ! mountpoint -q /mnt/pmemdir; then
    mount -o dax /dev/pmem0 /mnt/pmemdir
else
    echo "/mnt/pmemdir is mounted already, skip mount."
fi

# Now prepare the data
cd ../dataset
./prepare_dataset.sh
./load_dataset.sh

echo "Copying arxiv.rdb to pmem disk..."
cp arxiv.rdb /mnt/pmemdir/redis1.rdb
cp /mnt/pmemdir/redis1.rdb /mnt/pmemdir/redis2.rdb
cp /mnt/pmemdir/redis1.rdb /mnt/pmemdir/redis3.rdb
cp /mnt/pmemdir/redis1.rdb /mnt/pmemdir/redis4.rdb
echo "Experiment environment initialized!"