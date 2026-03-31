#ifndef LSB_THREAD_POOL_H 
#define LSB_THREAD_POOL_H 

#include <pthread.h> 
#include <stdbool.h> 

typedef struct { 
    void *(*function)(void *);  /* Task function pointer */ 
    void *arg;                 /* Task argument (passed to the function) */ 
} threadPoolTask_t; 

typedef struct { 
    pthread_t *threads;        /* Array of worker threads in the pool */ 
    threadPoolTask_t *queue;   /* Queue storing pending tasks */ 
    int threadCount;           /* Total number of worker threads */ 
    int queueSize;             /* Maximum capacity of the task queue */ 
    int head;                  /* Index of the queue's head (next task to process) */ 
    int tail;                  /* Index of the queue's tail (next position to add task) */ 
    int count;                 /* Current number of pending tasks in the queue */ 
    int shutdown;              /* Shutdown flag (non-zero = pool is shutting down) */ 
    pthread_mutex_t lock;      /* Mutex lock for synchronizing queue access */ 
    pthread_cond_t notEmpty;   /* Condition variable: signaled when queue is not empty (wakes workers) */ 
} threadPool_t;

threadPool_t *createThreadPool(int threadCount, int queueSize); 
int addTaskToThreadPool(threadPool_t *pool, void* (*function)(void *), void *arg); 
void destroyThreadPool(threadPool_t *pool); 
int createAndRunThread(void* (*function)(void*), void* arg);
#endif /* LSB_THREAD_POOL_H */
