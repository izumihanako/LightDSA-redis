numactl -C1 --membind=0 ./redis-server --dir /mnt/pmemdir  --rdbcompression no --rdbchecksum no --dbfilename redis1.rdb --port 9001 &
numactl -C2 --membind=0 ./redis-server --dir /mnt/pmemdir  --rdbcompression no --rdbchecksum no --dbfilename redis2.rdb --port 9002 &
numactl -C3 --membind=0 ./redis-server --dir /mnt/pmemdir  --rdbcompression no --rdbchecksum no --dbfilename redis3.rdb --port 9003 &
numactl -C4 --membind=0 ./redis-server --dir /mnt/pmemdir  --rdbcompression no --rdbchecksum no --dbfilename redis4.rdb --port 9004 &