

#ifndef _BATCH_MEMORY_H_
#define _BATCH_MEMORY_H_

#include <stddef.h>
#include <time.h>


struct batch_memory_pool;


#ifdef __cplusplus
extern "C" {
#endif


int batch_memory_pool_init(struct batch_memory_pool *pool, size_t max_limit);


void* batch_memory_pool_alloc(struct batch_memory_pool *pool, size_t size);


void batch_memory_pool_cleanup(struct batch_memory_pool *pool);


int batch_memory_pool_get_stats(struct batch_memory_pool *pool, 
                                size_t *total_allocated, 
                                int *block_count);


int batch_memory_pool_validate(struct batch_memory_pool *pool);


size_t batch_memory_pool_gc(struct batch_memory_pool *pool, int force_cleanup);

#define BATCH_MALLOC(pool, size) batch_memory_pool_alloc(pool, size)
#define BATCH_FREE(pool, ptr) 


#define BATCH_ALIGN_SIZE(size, alignment) \
    (((size) + (alignment) - 1) & ~((alignment) - 1))


#define BATCH_CHECK_PTR(ptr) \
    do { \
        if (!(ptr)) { \
            ls_syslog(LOG_ERR, "NULL pointer detected at %s:%d", __FILE__, __LINE__); \
            return -1; \
        } \
    } while(0)

#define BATCH_CHECK_PTR_RETURN_NULL(ptr) \
    do { \
        if (!(ptr)) { \
            ls_syslog(LOG_ERR, "NULL pointer detected at %s:%d", __FILE__, __LINE__); \
            return NULL; \
        } \
    } while(0)

#ifdef __cplusplus
}
#endif

#endif /* _BATCH_MEMORY_H_ */
