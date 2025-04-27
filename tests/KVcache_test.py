import redis
import numpy as np
from typing import Iterable, Tuple, List
import hashlib
import time

class SimpleRedisClient:
    def __init__(self, host='localhost', port=6379, db=0, password=None):
        """初始化Redis连接"""
        self.redis = redis.Redis(
            host=host,
            port=port,
            db=db,
            password=password,
            decode_responses=True  # 自动将响应解码为字符串
        ) 
    def set(self, key, value, expire=None):
        """设置键值对，可选过期时间（秒）"""
        return self.redis.set(key, value, ex=expire)
    
    def get(self, key):
        """获取键值"""
        return self.redis.get(key)
    
    def exists(self, key):
        """检查键是否存在"""
        return self.redis.exists(key)
    
    def execute_command(self, command, *args):
        """执行自定义命令"""
        return self.redis.execute_command(command, *args)

def tuple_kv_to_blob( kv_tensors: tuple ) -> np.ndarray: 
    """将kv_tensors转换为blob格式"""
    """5个维度分别为: 模型层数(num_layer), Key/Value类型(2个元素, 0表示Key, 1表示Value), token数量(num_tok), 注意力头数量(num_kv_head), 每个头的隐藏层大小(head_size)"""
    k_temp = []
    v_temp = []
    for kv_layer in kv_tensors:
        k_temp.append(kv_layer[0])
        v_temp.append(kv_layer[1])
    k_tensor_blob = np.stack(k_temp)
    v_tensor_blob = np.stack(v_temp) 
    # kv_tensors: [num_layer, 2, num_tok, num_kv_head, head_size]
    kv_tensors_flatten = np.stack((k_tensor_blob, v_tensor_blob)) 
    kv_tensors_flatten = kv_tensors_flatten.transpose([1, 0, 2, 3, 4]) 
    return kv_tensors_flatten

def generate_kv_cache(num_tokens:int , fmt:str):
    """生成模拟的KV缓存数据,以LLama3-70B为例"""
    num_layers = 80
    num_kv_heads = 8
    head_size = 128
    shape = ([num_tokens, num_kv_heads, head_size]
             if fmt == "vllm" else [num_kv_heads, num_tokens, head_size])
    dtype = np.float16 if fmt == "vllm" else np.float32

    kv_cache = []
    for i in range(num_layers):
        k = np.random.rand(*shape).astype(dtype)
        v = np.random.rand(*shape).astype(dtype)
        kv_cache.append((k, v))

    return tuple(kv_cache)

def generate_tokens(num_tokens):
    """生成模拟的token数据""" 
    return np.random.randint(0, 10000, size=[num_tokens]).astype(np.int32)

def slice_kv_at( start_idx: int, kv_tensors: np.ndarray, fmt: str ) -> list:
    """
    vllm format: [num_layer, 2, num_tokens, num_kv_head, head_size]
    huggingface format: [num_layer, 2, num_kv_head, num_tokens, head_size]
    """
    if fmt == "vllm":
        return [
            x[start_idx:, ...] for x in np.split(kv_tensors, kv_tensors.shape[2], axis=2)
        ]
    else:
        return [
            x[:, :, :, start_idx:, ...] for x in np.split(kv_tensors, kv_tensors.shape[3], axis=3)
        ]

def LMcache_hash( tokens , prefix_hash: str ) -> str:
    """按LMcache的方式计算哈希值"""
    hasher = hashlib.sha256()
    hasher.update(prefix_hash.encode("ascii"))
    hasher.update(tokens.tobytes())
    return hasher.hexdigest()
 
def prefix_hash( token_chunks: list) -> List[str]:
    """计算前缀哈希值"""
    prefix_hash = ""
    prefix_hashes = []
    for token_chunk in token_chunks:
        prefix_hash = LMcache_hash(token_chunk, prefix_hash)
        prefix_hashes.append(prefix_hash) 
    return prefix_hashes


def make_chunks( tokens: np.ndarray, kv_tensors: np.ndarray, chunk_size:int , fmt: str ) -> Iterable[Tuple[str, np.ndarray]]:
    """将tokens和kv_tensors分块""" 
    # 将零散的部分截断丢弃 
    if len(tokens) % chunk_size != 0:
        tokens = tokens[:len(tokens) - len(tokens) % chunk_size]
        if fmt == "vllm":
            kv_tensors = kv_tensors[:, :, :len(tokens), ...]
        else: # fmt == "huggingface"
            kv_tensors = kv_tensors[:, :, :, :len(tokens), ...]

    num_chunks = len(tokens) // chunk_size
    token_chunks = np.split(tokens, num_chunks)
    # 计算前缀哈希值
    prefix_hashes = prefix_hash(token_chunks) 
    kv_chunks = slice_kv_at(0, kv_tensors, fmt) 
    # 将token_chunks和kv_chunks打包成元组
    chunk_hashes_and_kvs = zip(prefix_hashes, kv_chunks)
    print(f"tokens: {len(tokens)}, chunk_size: {chunk_size}, num_chunks: {num_chunks}", flush=True)
    return chunk_hashes_and_kvs
 
def make_key(chunk_hash: str, fmt: str) -> str:
    """生成缓存键"""
    modelname = "llama3-70B"
    world_size = 1
    worker_id = 0
    return f"{fmt}:{modelname}:{world_size}:{worker_id}:{chunk_hash}"

def insert_KVcache_to( client:SimpleRedisClient, num_tokens:int, chunk_size:int ) -> None:
    """插入KV缓存"""
    fmt="vllm" 
    tokens = generate_tokens(num_tokens) 
    kv_cache = generate_kv_cache(num_tokens,fmt)
    kv_cache_blob = tuple_kv_to_blob(kv_cache) 
    chunk_hashes_and_kvs = make_chunks( tokens, kv_cache_blob, chunk_size , fmt )

    id = 0 
    for prefix_hash, kv in chunk_hashes_and_kvs: 
        cache_key = make_key(prefix_hash, fmt) 
        kv_blob = kv.tobytes() 
        client.set(cache_key, kv_blob)
        id += 1
        print(f"{id:}-th insert\n -> key={cache_key}\n -> value={len(kv_blob)} Bytes into Redis") 
    pass 

if __name__ == "__main__":
    np.random.seed(int(time.time()))
    client = SimpleRedisClient()
    for i in range(1000):
        insert_KVcache_to( client , 5120 , 512 )