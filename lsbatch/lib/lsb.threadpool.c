#include "lsb.threadpool.h" 
#include <stdlib.h> 
#include <string.h> 
#include <errno.h> 

/* 工作线程函数 */ 
static void *workerThread(void *arg) { 
    threadPool_t *pool = (threadPool_t *)arg; 
    threadPoolTask_t task; 

    while (1) { 
        pthread_mutex_lock(&pool->lock); 

        while (pool->count == 0 && !pool->shutdown) { 
            pthread_cond_wait(&pool->notEmpty, &pool->lock); 
        } 
        if (pool->shutdown && pool->count == 0) { 
            pthread_mutex_unlock(&pool->lock); 
            pthread_exit(NULL); 
        } 
        task.function = pool->queue[pool->head].function; 
        task.arg = pool->queue[pool->head].arg; 
        pool->head = (pool->head + 1) % pool->queueSize; 
        pool->count--; 

        pthread_cond_signal(&pool->notFull); 
        pthread_mutex_unlock(&pool->lock); 
        (*task.function)(task.arg); 
    } 

    return NULL; 
} 

/* 创建线程池 */ 
threadPool_t *createThreadPool(int threadCount, int queueSize) { 
    threadPool_t *pool; 
    int i; 

    if (threadCount <= 0 || queueSize <= 0) { 
        return NULL; 
    } 

    pool = (threadPool_t *)malloc(sizeof(threadPool_t)); 
    if (pool == NULL) { 
        return NULL; 
    } 
    pool->threadCount = threadCount; 
    pool->queueSize = queueSize; 
    pool->head = 0; 
    pool->tail = 0; 
    pool->count = 0; 
    pool->shutdown = 0; 
    pool->threads = (pthread_t *)malloc(sizeof(pthread_t) * threadCount); 
    pool->queue = (threadPoolTask_t *)malloc(sizeof(threadPoolTask_t) * queueSize); 

    if (pthread_mutex_init(&pool->lock, NULL) != 0 || 
        pthread_cond_init(&pool->notEmpty, NULL) != 0 || 
        pthread_cond_init(&pool->notFull, NULL) != 0 || 
        pool->threads == NULL || pool->queue == NULL) { 
        if (pool->threads) free(pool->threads); 
        if (pool->queue) free(pool->queue); 
        free(pool); 
        return NULL; 
    } 

    for (i = 0; i < threadCount; i++) { 
        if (pthread_create(&pool->threads[i], NULL, workerThread, pool) != 0) { 
            destroyThreadPool(pool); 
            return NULL; 
        } 
    } 

    return pool; 
} 

/* 添加任务到线程池 */ 
int addTaskToThreadPool(threadPool_t *pool, void (*function)(void *), void *arg) { 
    if (pool == NULL || function == NULL) { 
        return -1; 
    } 

    pthread_mutex_lock(&pool->lock); 

    while (pool->count == pool->queueSize && !pool->shutdown) { 
        pthread_cond_wait(&pool->notFull, &pool->lock); 
    } 
    if (pool->shutdown) { 
        pthread_mutex_unlock(&pool->lock); 
        return -1; 
    } 
    pool->queue[pool->tail].function = function; 
    pool->queue[pool->tail].arg = arg; 
    pool->tail = (pool->tail + 1) % pool->queueSize; 
    pool->count++; 

    pthread_cond_signal(&pool->notEmpty); 
    pthread_mutex_unlock(&pool->lock); 

    return 0; 
} 

/* 销毁线程池 */ 
void destroyThreadPool(threadPool_t *pool) { 
    int i; 

    if (pool == NULL) { 
        return; 
    } 

    pthread_mutex_lock(&pool->lock); 
    pool->shutdown = 1; 
    pthread_cond_broadcast(&pool->notEmpty); 
    pthread_mutex_unlock(&pool->lock); 

    for (i = 0; i < pool->threadCount; i++) { 
        pthread_join(pool->threads[i], NULL); 
    } 

    free(pool->threads); 
    free(pool->queue); 
    pthread_mutex_destroy(&pool->lock); 
    pthread_cond_destroy(&pool->notEmpty); 
    pthread_cond_destroy(&pool->notFull); 
    free(pool); 
} 