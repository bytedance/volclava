#include "lsb.threadpool.h"
#include <stdlib.h> 
#include <string.h> 
#include <errno.h> 

/*
 * Worker thread function for the thread pool
 * Loops to retrieve tasks from the task queue and execute them; exits when the pool shuts down
 * @param[in] arg: Pointer to the target thread pool instance
 * @return NULL (unreachable in normal execution, added to match function signature)
 */
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

        pthread_mutex_unlock(&pool->lock); 
        (*task.function)(task.arg); 
    } 

    return NULL; 
} 

/*
 * Create and initialize a thread pool
 * Allocates memory for pool structure, worker threads, and task queue; initializes synchronization primitives
 * @param[in] threadCount: Number of worker threads to spawn
 * @param[in] queueSize: Maximum capacity of the task queue
 * @return Pointer to the initialized threadPool_t structure on success; NULL on failure
 */
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
/*
 * Add a task to the thread pool's task queue
 * @param[in] pool: Pointer to the target thread pool
 * @param[in] function: Pointer to the task function to execute
 * @param[in] arg: Argument to pass to the task function
 * @return 0 on successful task addition; -1 on failure
 */
int addTaskToThreadPool(threadPool_t *pool, void* (*function)(void *), void *arg) { 
    if (pool == NULL || function == NULL) { 
        return -1; 
    } 

    pthread_mutex_lock(&pool->lock); 

    if (pool->shutdown || pool->count == pool->queueSize) { 
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

/*
 * Destroy the thread pool and release all associated resources
 * Sets the shutdown flag, wakes all worker threads, waits for threads to exit, and frees memory/sync primitives
 * @param[in] pool: Pointer to the thread pool to destroy
 */
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
    free(pool); 
} 

/*
 * Create a detached thread to run the specified function with given argument
 * Initializes thread attributes, sets detached state, creates thread, handles errors, and cleans up attributes
 * @param[in] function: Function to be executed by the thread
 * @param[in] arg: Argument passed to the function
 * @return 0 on successful; -1 on failure
 */
int createAndRunThread(void* (*function)(void*), void* arg) {
    pthread_t thread;
    pthread_attr_t attr;
    
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    
    if (pthread_create(&thread, &attr, (void*(*)(void*))function, arg) != 0) {
        pthread_attr_destroy(&attr);
        return -1;
    }
    
    pthread_attr_destroy(&attr);
    return 0;
}