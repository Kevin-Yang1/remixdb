// test_pthread_simple.c - 简单的多线程测试，不涉及slab
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>

#define N_THREADS 3
#define N_ITERATIONS 5

// 全局共享变量
static int shared_counter = 0;
static pthread_mutex_t counter_mutex = PTHREAD_MUTEX_INITIALIZER;

// 线程工作函数
static void *worker_thread(void *arg)
{
    long thread_id = (long)arg;

    printf("线程 %ld 开始执行\n", thread_id);

    for (int i = 0; i < N_ITERATIONS; i++) {
        // 在这里设置断点测试
        printf("线程 %ld: 迭代 %d 开始\n", thread_id, i);

        // 加锁操作共享资源
        pthread_mutex_lock(&counter_mutex);
        int old_value = shared_counter;
        shared_counter++;
        printf("线程 %ld: 计数器从 %d 增加到 %d\n", thread_id, old_value, shared_counter);
        pthread_mutex_unlock(&counter_mutex);

        // 模拟一些工作
        usleep(100000); // 100ms

        printf("线程 %ld: 迭代 %d 完成\n", thread_id, i);
    }

    printf("线程 %ld 完成所有工作\n", thread_id);
    return (void*)thread_id;
}

int main(void)
{
    printf("开始简单多线程测试\n");
    printf("将创建 %d 个线程，每个线程执行 %d 次迭代\n", N_THREADS, N_ITERATIONS);

    pthread_t threads[N_THREADS];

    // 创建线程
    for (long i = 0; i < N_THREADS; i++) {
        printf("创建线程 %ld\n", i);
        int result = pthread_create(&threads[i], NULL, worker_thread, (void*)i);
        if (result != 0) {
            printf("创建线程 %ld 失败: %d\n", i, result);
            exit(1);
        }
    }

    // 等待所有线程完成
    for (int i = 0; i < N_THREADS; i++) {
        void *thread_result;
        int result = pthread_join(threads[i], &thread_result);
        if (result == 0) {
            printf("线程 %d 已结束，返回值: %ld\n", i, (long)thread_result);
        } else {
            printf("等待线程 %d 失败: %d\n", i, result);
        }
    }

    printf("最终计数器值: %d (期望值: %d)\n", shared_counter, N_THREADS * N_ITERATIONS);
    printf("多线程测试完成\n");

    return 0;
}
