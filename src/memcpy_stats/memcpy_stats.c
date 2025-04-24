
#include "../server.h"
#include "../sds.h"
#include "memcpy_stats.h"
#include <time.h>
#include <pthread.h> 

/* 全局统计数据 */
static memcpy_stats_t stats;
static pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;
int stats_initialized = 0 ;

void memcpyStatsCommand(client *c) {
    /* 如果还没有初始化，就先初始化统计数据 */
    if (!stats_initialized) {
        memcpy_stats_init();
    }
    
    /* 强制输出当前统计信息 */
    memcpy_stats_dump();
    
    /* 构建一个回复给客户端 */
    sds reply = sdsempty();
    reply = sdscatprintf(reply, "total calls: %llu\r\n", stats.total_calls);
    reply = sdscatprintf(reply, "total bytes: %llu\r\n", stats.total_bytes);
    
    reply = sdscatprintf(reply, "distribution:\r\n");
    reply = sdscatprintf(reply, "<=16B: %llu (%.2f%%)\r\n", 
            stats.counts[0], 
            (double)stats.counts[0] * 100 / (stats.total_calls ? stats.total_calls : 1));
    
    for (int i = 1; i < MEMCPY_SIZE_RANGES; i++) {
        reply = sdscatprintf(reply, "<=%zuB: %llu (%.2f%%)\r\n", 
                stats.range_limits[i], 
                stats.counts[i], 
                (double)stats.counts[i] * 100 / (stats.total_calls ? stats.total_calls : 1));
    }
    
    reply = sdscatprintf(reply, ">%zuB: %llu (%.2f%%)\r\n", 
            stats.range_limits[MEMCPY_SIZE_RANGES-1], 
            stats.counts[MEMCPY_SIZE_RANGES], 
            (double)stats.counts[MEMCPY_SIZE_RANGES] * 100 / (stats.total_calls ? stats.total_calls : 1));
    
    reply = sdscatprintf(reply, "\r\nSee log file for details: %s\r\n", MEMCPY_SIZE_LOG_FILE);
    
    addReplyBulkSds(c, reply);
}

/* 初始化内存统计的大小区间 */
void memcpy_stats_init(void) {
    pthread_mutex_lock(&stats_mutex);
    
    if (stats_initialized) {
        pthread_mutex_unlock(&stats_mutex);
        return;
    }
    
    /* 设置大小范围: 16B, 32B, 64B, 128B, 256B, 512B, 1KB, 4KB, 16KB, 64KB */
    stats.range_limits[0] = 16;
    stats.range_limits[1] = 32;
    stats.range_limits[2] = 64;
    stats.range_limits[3] = 128;
    stats.range_limits[4] = 256;
    stats.range_limits[5] = 512;
    stats.range_limits[6] = 1024;
    stats.range_limits[7] = 4096;
    stats.range_limits[8] = 16384;
    stats.range_limits[9] = 65536;
    
    /* 初始化计数器 */
    memset(stats.counts, 0, sizeof(stats.counts));
    stats.total_calls = 0;
    stats.total_bytes = 0;
    
    stats_initialized = 1;
    
    pthread_mutex_unlock(&stats_mutex);
    
    /* 创建初始日志文件 */
    FILE *fp = fopen(MEMCPY_SIZE_LOG_FILE, "w");
    if (fp) {
        fprintf(fp, "# Redis memcpy/memmove 统计信息\n");
        fprintf(fp, "# 统计开始于: %s\n", ctime(&(time_t){time(NULL)}));
        fprintf(fp, "# 大小区间: ");
        fprintf(fp, "<=16B, ");
        fprintf(fp, "<=32B, ");
        fprintf(fp, "<=64B, ");
        fprintf(fp, "<=128B, ");
        fprintf(fp, "<=256B, ");
        fprintf(fp, "<=512B, ");
        fprintf(fp, "<=1KB, ");
        fprintf(fp, "<=4KB, ");
        fprintf(fp, "<=16KB, ");
        fprintf(fp, "<=64KB, ");
        fprintf(fp, ">64KB\n");
        fclose(fp);
    }
}

/* 记录一次memcpy调用 */
void memcpy_stats_record(size_t size) {
    int i;
    
    if (!stats_initialized) {
        memcpy_stats_init();
    }
    
    pthread_mutex_lock(&stats_mutex);
    
    /* 更新总计数 */
    stats.total_calls++;
    stats.total_bytes += size;
    
    /* 更新大小分布 */
    for (i = 0; i < MEMCPY_SIZE_RANGES; i++) {
        if (size <= stats.range_limits[i]) {
            stats.counts[i]++;
            break;
        }
    }
    
    /* 如果大小超过了所有范围，记录在最后一个桶中 */
    if (i == MEMCPY_SIZE_RANGES) {
        stats.counts[MEMCPY_SIZE_RANGES]++;
    }
    
    /* 每100万次调用dump一次统计信息 */
    if (stats.total_calls % 1000000 == 0) {
        pthread_mutex_unlock(&stats_mutex);
        memcpy_stats_dump();
    } else {
        pthread_mutex_unlock(&stats_mutex);
    }
}

/* 输出统计结果到日志文件 */
void memcpy_stats_dump(void) {
    FILE *fp;
    int i;
    
    if (!stats_initialized) return;
    
    pthread_mutex_lock(&stats_mutex);
    
    fp = fopen(MEMCPY_SIZE_LOG_FILE, "a");
    if (!fp) {
        pthread_mutex_unlock(&stats_mutex);
        return;
    }
    
    fprintf(fp, "--------------------------------\n");
    fprintf(fp, "时间: %s", ctime(&(time_t){time(NULL)}));
    fprintf(fp, "总调用次数: %llu\n", stats.total_calls);
    fprintf(fp, "总复制字节: %llu\n", stats.total_bytes);
    
    fprintf(fp, "大小分布:\n");
    fprintf(fp, "<=16B: %llu (%.2f%%)\n", 
            stats.counts[0], 
            (double)stats.counts[0] * 100 / stats.total_calls);
    
    for (i = 1; i < MEMCPY_SIZE_RANGES; i++) {
        fprintf(fp, "<=%zuB: %llu (%.2f%%)\n", 
                stats.range_limits[i], 
                stats.counts[i], 
                (double)stats.counts[i] * 100 / stats.total_calls);
    }
    
    fprintf(fp, ">%zuB: %llu (%.2f%%)\n", 
            stats.range_limits[MEMCPY_SIZE_RANGES-1], 
            stats.counts[MEMCPY_SIZE_RANGES], 
            (double)stats.counts[MEMCPY_SIZE_RANGES] * 100 / stats.total_calls);
    
    fclose(fp);
    pthread_mutex_unlock(&stats_mutex);
}

/* 包装的memcpy函数，用于替代标准memcpy */
void *redis_memcpy(void *dest, const void *src, size_t n) {
    /* 记录此次memcpy的大小 */
    memcpy_stats_record(n);
    
    /* 调用实际的memmove */
    return memmove(dest, src, n);
}