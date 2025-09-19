#ifndef LSB_THREAD_POOL_H 
#define LSB_THREAD_POOL_H 

#include <pthread.h> 
#include <stdbool.h> 
#include "lsb.h" 

typedef struct { 
    void (*function)(void *);  /* 任务函数指针 */ 
    void *arg;                 /* 任务参数 */ 
} threadPoolTask_t; 

typedef struct { 
    pthread_t *threads;        /* 线程数组 */ 
    threadPoolTask_t *queue;     /* 任务队列 */ 
    int threadCount;           /* 线程数量 */ 
    int queueSize;             /* 队列大小 */ 
    int head;                  /* 队列头索引 */ 
    int tail;                  /* 队列尾索引 */ 
    int count;                 /* 当前任务数 */ 
    int shutdown;              /* 关闭标志 */ 
    pthread_mutex_t lock;      /* 互斥锁 */ 
    pthread_cond_t notEmpty;   /* 非空条件变量 */ 
    pthread_cond_t notFull;    /* 非满条件变量 */ 
} threadPool_t;

threadPool_t *createThreadPool(int threadCount, int queueSize); 
int addTaskToThreadPool(threadPool_t *pool, void (*function)(void *), void *arg); 
void destroyThreadPool(threadPool_t *pool); 

#endif /* LSB_THREAD_POOL_H */