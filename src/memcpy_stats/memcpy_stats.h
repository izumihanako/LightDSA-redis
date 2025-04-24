#ifndef __MEMCPY_STATS_H
#define __MEMCPY_STATS_H

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// #include "../server.h"

/* 预定义的memcpy大小区间，用于统计 */
#define MEMCPY_SIZE_RANGES 10
#define MEMCPY_SIZE_LOG_FILE "memcpy_stats.log"

/* 统计memcpy调用的数据结构 */
typedef struct {
    size_t range_limits[MEMCPY_SIZE_RANGES];  /* 大小区间的上限值 */
    unsigned long long counts[MEMCPY_SIZE_RANGES + 1];  /* 每个区间的计数 (+1 是为了包含超出最大区间的情况) */
    unsigned long long total_calls;  /* 总调用次数 */
    unsigned long long total_bytes;  /* 总字节数 */
} memcpy_stats_t;

/* 初始化memcpy统计 */
void memcpy_stats_init(void);

/* 记录一次memcpy调用 */
void memcpy_stats_record(size_t size);

/* 输出统计结果到日志文件 */
void memcpy_stats_dump(void);

/* 包装的memcpy函数 */
void *redis_memcpy(void *dest, const void *src, size_t n);

struct client;
void memcpyStatsCommand(struct client *c); 

extern int stats_initialized ; /* 是否已经初始化统计数据 */

#endif /* __MEMCPY_STATS_H */