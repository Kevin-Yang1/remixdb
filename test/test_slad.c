// demo_slab.c
#include <stdio.h>
#include <pthread.h>
#include "../lib.h"   // 你刚才看到的实现头文件，假设已可直接 include

// /* ---------- 1. 要管理的对象 ---------- */
// struct myobj {
//     int   id;
//     char  payload[36];
// };
/* -------- 1. 对象定义：保证 8 字节对齐 -------- */
struct myobj {
    int  id;
    char payload[32];
} __attribute__((aligned(8)));   // sizeof = 40, 满足 slab 要求

/* ---------- 2. 单线程（unsafe）演示 ---------- */
static void demo_unsafe(void)
{
    const size_t OBJ_SIZE = sizeof(struct myobj);
    const size_t BLK_SIZE = 64 * 1024;           // 64 KiB per slab block

    struct slab *s = slab_create(OBJ_SIZE, BLK_SIZE);

    /* （可选）一次性预留至少 1 k 个对象，避免运行期多次 page fault */
    slab_reserve_unsafe(s, 1024);

    /* 分配若干对象 */
    struct myobj *a = slab_alloc_unsafe(s);
    struct myobj *b = slab_alloc_unsafe(s);
    a->id = 1; b->id = 2;

    printf("[unsafe] a->id=%d, b->id=%d\n", a->id, b->id);

    /* 归还对象 */
    slab_free_unsafe(s, b);
    slab_free_unsafe(s, a);

    /* 释放所有块（把 active → backup，可随时再复用）*/
    slab_free_all(s);

    /* 最终销毁 */
    slab_destroy(s);
}

/* ---------- 3. 多线程（safe）演示 ---------- */
#define N_THREADS 1
#define N_PER_THREAD 50000

static struct slab *g_slab;   // 全局 slab，线程共享

static void *worker(void *arg)
{
    long tid = (long)arg;
    for (int i = 0; i < N_PER_THREAD; i++) {
        struct myobj *o = slab_alloc_safe(g_slab);
        if (o) {
            o->id = (tid << 20) | i;
            /* ...业务逻辑... */
            slab_free_safe(g_slab, o);
        }
    }
    return NULL;
}

static void demo_safe(void)
{
    g_slab = slab_create(sizeof(struct myobj), 64 * 1024);

    pthread_t th[N_THREADS];
    for (long i = 0; i < N_THREADS; i++)
        pthread_create(&th[i], NULL, worker, (void *)i);

    for (int i = 0; i < N_THREADS; i++)
        pthread_join(th[i], NULL);

    printf("[safe] outstanding objs = %lu\n",
           slab_get_nalloc(g_slab));   // 理论输出 0

    slab_destroy(g_slab);
}

/* ---------- 4. 入口 ---------- */
int main(void)
{
    demo_unsafe();
    demo_safe();
    return 0;
}
