import redis 
import pandas as pd
import sys
from tqdm import tqdm  # progress bar

class SimpleRedisClient:
    def __init__(self, host='localhost', port=6379, db=0, password=None):
        """ init Redis connection """
        self.redis = redis.Redis(
            host=host,
            port=port,
            db=db,
            password=password,
            decode_responses=True  # store strings instead of bytes
        ) 
    def set(self, key, value, expire=None):
        """set key-value with optional expiration"""
        return self.redis.set(key, value, ex=expire)
    
    def get(self, key):
        """get key value"""
        return self.redis.get(key)
    
    def exists(self, key):
        """ check if key exists """
        return self.redis.exists(key)
    
    def execute_command(self, command, *args):
        """ execute raw Redis command """
        return self.redis.execute_command(command, *args)
    

# 1. Read CSV file
# The CSV file path is provided as a command line argument
if len(sys.argv) < 2:
    print(" please provide the path to the CSV file as a command line argument.")
    sys.exit(1)
csv_path = sys.argv[1]
print(f"Reading CSV file: {csv_path}")
df = pd.read_csv(csv_path)
# 2. Initialize Redis client
redis_client = SimpleRedisClient()

# 3. Data insertion function
def insert_to_redis(df, batch_size=1000):
    """Batch insert data into Redis"""
    with redis_client.redis.pipeline() as pipe:
        for idx, row in tqdm(df.iterrows(), total=len(df)):
            paper_key = f"arxiv:paper:{idx}" 
            # Use hash storage
            pipe.hset(paper_key, mapping={
                "abstract": str(row['abstract']),
                "article": str(row['article'])
            }) 
            # Use pipeline to submit to Redis
            if idx % batch_size == 0:
                pipe.execute()
        pipe.execute() 

# 4. Execute insertion
print(f"Starting insertion of {len(df)} records into Redis...")
insert_to_redis(df)
print("Data insertion complete!")

# 5. Verify insertion results
sample_key = "arxiv:paper:0"
print("\nSample data verification:")
print(f"Abstract length: {len(redis_client.redis.hget(sample_key, 'abstract'))} characters")
print(f"Article length: {len(redis_client.redis.hget(sample_key, 'article'))} characters")
redis_client.redis.shutdown()  # close connection