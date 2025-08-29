#!/bin/bash
set -e

# first setup DSA, init hugepages and disable numa balancing
echo 20480 > /proc/sys/vm/nr_hugepages
echo 0 | sudo tee /proc/sys/kernel/numa_balancing > /dev/null
echo 3 > /proc/sys/vm/drop_caches
./setup_dsa.sh -d dsa0
./setup_dsa.sh -d dsa0 -w 1 -m s -e 4 -f 1
cd ../../
make distclean > /dev/null
cd AE/figure13/

# Function to start Redis servers
start_redis_servers() {
    echo "Starting Redis servers..."
    cd ../../src/
    nohup numactl -C1 --membind=0 ./redis-server --dir /mnt/pmemdir  --rdbcompression no --rdbchecksum no --dbfilename redis1.rdb --port 9001 > ../AE/figure13/redis1.log 2>&1 & 
    nohup numactl -C2 --membind=0 ./redis-server --dir /mnt/pmemdir  --rdbcompression no --rdbchecksum no --dbfilename redis2.rdb --port 9002 > ../AE/figure13/redis2.log 2>&1 & 
    nohup numactl -C3 --membind=0 ./redis-server --dir /mnt/pmemdir  --rdbcompression no --rdbchecksum no --dbfilename redis3.rdb --port 9003 > ../AE/figure13/redis3.log 2>&1 & 
    nohup numactl -C4 --membind=0 ./redis-server --dir /mnt/pmemdir  --rdbcompression no --rdbchecksum no --dbfilename redis4.rdb --port 9004 > ../AE/figure13/redis4.log 2>&1 &
    sleep 10
    echo "Redis servers started on ports 9001-9004"
    cd ../AE/figure13/
}

# Function to stop Redis servers
stop_redis_servers() {
    echo "Stopping Redis servers, this will trigger RDB persistence..."
    cd ../../src/ 
    ./redis-cli -p 9001 shutdown &
    ./redis-cli -p 9002 shutdown &
    ./redis-cli -p 9003 shutdown &
    ./redis-cli -p 9004 shutdown &
    sleep 10
    cd ../AE/figure13/
}

# Function to copy configuration files and compile
copy_config_files_and_compile() {
    local dsa_conf_file="$1"
    local rdb_file="$2"
    local rio_file="$3"
    
    echo "Copying configuration files..."
    cp "$dsa_conf_file" ../../deps/LightDSA/src/details/dsa_conf.hpp
    cp "$rdb_file" ../../src/rdb.c
    cp "$rio_file" ../../src/rio.c 
    echo "Compiling Redis..."
    cd ../../ 
    make > /dev/null
    make lightDSA > /dev/null
    cd AE/figure13/ 
    echo "Compilation finished."
}

# Function to extract RDB times and calculate average time from log files
output_file="rdb_time_data.txt"
extract_rdb_times() { 
    local total=0
    local count=0   
    for i in {1..4}; do
        local time=$(grep "RDB finished.*cost.*seconds" "redis${i}.log" 2>/dev/null | sed -n 's/.*cost \([0-9.]*\) seconds.*/\1/p')
        if [[ -n "$time" ]]; then 
            total=$(echo "$total + $time" | bc -l)
            count=$((count + 1))
        fi
    done 
    if [[ $count -gt 0 ]]; then
        local average=$(echo "scale=3; $total / $count" | bc -l)
        echo "$average" >> "$output_file" 
    fi
}

# clear output file 
> "$output_file"

# CPU baseline
copy_config_files_and_compile "./confs_codes/dsa_conf_naiveDSA.hpp" "./confs_codes/rdb_cpu.c" "./confs_codes/rio_without_align.c"
start_redis_servers
stop_redis_servers
extract_rdb_times

# DSA baseline
copy_config_files_and_compile "./confs_codes/dsa_conf_naiveDSA.hpp" "./confs_codes/rdb_dsa.c" "./confs_codes/rio_without_align.c"
start_redis_servers
stop_redis_servers
extract_rdb_times

# 1. +Allocator
copy_config_files_and_compile "./confs_codes/dsa_conf_1+Allocator.hpp" "./confs_codes/rdb_dsa.c" "./confs_codes/rio_without_align.c"
start_redis_servers
stop_redis_servers
extract_rdb_times

# 2. +Interleaved page fault
copy_config_files_and_compile "./confs_codes/dsa_conf_2+Intr_PF.hpp" "./confs_codes/rdb_dsa.c" "./confs_codes/rio_without_align.c"
start_redis_servers
stop_redis_servers
extract_rdb_times

# 3. +In batch descriptor mixing
copy_config_files_and_compile "./confs_codes/dsa_conf_3+InBatch_mix.hpp" "./confs_codes/rdb_dsa.c" "./confs_codes/rio_without_align.c"
start_redis_servers
stop_redis_servers
extract_rdb_times

# 4. +Alignment-aware write
copy_config_files_and_compile "./confs_codes/dsa_conf_4+Write_align.hpp" "./confs_codes/rdb_dsa.c" "./confs_codes/rio_with_align.c"
start_redis_servers
stop_redis_servers
extract_rdb_times

# 5. +Out-of-order recycle
copy_config_files_and_compile "./confs_codes/dsa_conf_5+OoO_recycle.hpp" "./confs_codes/rdb_dsa.c" "./confs_codes/rio_with_align.c"
start_redis_servers
stop_redis_servers
extract_rdb_times

# draw figure13
python3 figure13.py
echo "Done!"