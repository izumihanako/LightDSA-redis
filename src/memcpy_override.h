#ifndef __MEMCPY_OVERRIDE_H
#define __MEMCPY_OVERRIDE_H

#include "memcpy_stats/memcpy_stats.h"

/* 
 * 这个宏会劫持所有对memcpy的调用，重定向到我们的redis_memcpy函数
 * 这样我们就可以统计所有memcpy调用的大小分布
 */
#define memcpy(dest, src, n)  redis_memcpy(dest, src, n)
#define memmove(dest, src, n) redis_memcpy(dest, src, n)

#endif /* __MEMCPY_OVERRIDE_H */