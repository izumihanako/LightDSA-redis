#!/bin/bash
set -e

# first compile and run redis
cd ../.. 
make all > /dev/null
cd AE/dataset
rm arxiv.rdb -f
pkill -9 redis-server || true

echo "Starting redis server..."
nohup ../../src/redis-server --dir ./ --rdbcompression no --rdbchecksum no --dbfilename arxiv.rdb > redis.log 2>&1 & 

# wait for redis to start
sleep 5
# load arxiv dataset
echo "Loading arxiv dataset..."
python3 load_csv_to_redis.py arxiv_full.csv
echo "Load dataset complete."

echo "Checking arxiv.rdb file size..."
expected_size=7759243540
actual_size=$(stat -c %s arxiv.rdb)
if [ "$actual_size" -eq "$expected_size" ]; then
    echo "arxiv.rdb size is correct: $actual_size bytes"
else
    echo "arxiv.rdb size mismatch! Actual: $actual_size bytes, Expected: $expected_size bytes"
fi
rm memcpy_stats.log redis.log
echo "Done."