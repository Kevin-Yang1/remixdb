// test_slad_debug.c - 专门用于调试的简化版本
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include "../lib.h"

struct myobj {
    int  id;
    char payload[32];
} __attribute__((aligned(8)));

#define N_THREADS 2
#define N_PER_THREAD 10

static struct slab *g_slab;

static void *worker(void *arg)
{
    long tid = (long)arg;
    printf("线程 %ld 开始工作\n", tid);

    for (int i = 0; i < N_PER_THREAD; i++) {
        // 在这里设置断点应该是安全的
        struct myobj *o = slab_alloc_safe(g_slab);
        if (o) {
            o->id = (tid << 20) | i;
            printf("线程 %ld: 分配对象 %d (id=%d)\n", tid, i, o->id);

            // 添加一些延迟让调试更容易
            usleep(1000);

            slab_free_safe(g_slab, o);
        }
    }

    printf("线程 %ld 完成工作\n", tid);
    return NULL;
}

static void demo_debug(void)
{
    printf("开始多线程调试演示\n");

    g_slab = slab_create(sizeof(struct myobj), 64 * 1024);

    pthread_t th[N_THREADS];
    for (long i = 0; i < N_THREADS; i++) {
        printf("创建线程 %ld\n", i);
        pthread_create(&th[i], NULL, worker, (void *)i);
    }

    for (int i = 0; i < N_THREADS; i++) {
        pthread_join(th[i], NULL);
        printf("线程 %d 已结束\n", i);
    }

    printf("剩余对象数: %lu\n", slab_get_nalloc(g_slab));
    slab_destroy(g_slab);
    printf("调试演示完成\n");
}

int main(void)
{
    demo_debug();
    return 0;
}
