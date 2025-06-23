#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// 模拟相关的定义
typedef uint8_t u8;
typedef uint32_t u32;

#define SST_VLEN_TS ((0x10000u))     // 墓碑标记 (tomb stone) = 65536
#define SST_VLEN_MASK ((0xffffu))    // 实际值长度掩码 = 65535

// 模拟 kv 结构体
struct kv {
    u32 klen;     // 键长度
    u32 vlen;     // 值长度 (可能包含标志位)
    u8 kv[0];     // 柔性数组：键值数据
};

// sst_kv_size 函数
size_t
sst_kv_size(const struct kv * const kv)
{
    return sizeof(*kv) + kv->klen + (kv->vlen & SST_VLEN_MASK);
}

// 创建一个普通的 KV 对
struct kv* create_normal_kv(const char* key, const char* value) {
    u32 klen = strlen(key);
    u32 vlen = strlen(value);
    struct kv* kv = malloc(sizeof(struct kv) + klen + vlen);
    kv->klen = klen;
    kv->vlen = vlen;  // 普通值，没有标志位
    memcpy(kv->kv, key, klen);
    memcpy(kv->kv + klen, value, vlen);
    return kv;
}

// 创建一个墓碑 KV 对 (删除标记)
struct kv* create_tombstone_kv(const char* key) {
    u32 klen = strlen(key);
    struct kv* kv = malloc(sizeof(struct kv) + klen);
    kv->klen = klen;
    kv->vlen = SST_VLEN_TS;  // 设置墓碑标记，没有实际值
    memcpy(kv->kv, key, klen);
    return kv;
}

// 创建一个带墓碑标记但有值的 KV 对 (理论情况)
struct kv* create_tombstone_with_value_kv(const char* key, const char* value) {
    u32 klen = strlen(key);
    u32 vlen = strlen(value);
    struct kv* kv = malloc(sizeof(struct kv) + klen + vlen);
    kv->klen = klen;
    kv->vlen = SST_VLEN_TS | vlen;  // 墓碑标记 + 值长度
    memcpy(kv->kv, key, klen);
    memcpy(kv->kv + klen, value, vlen);
    return kv;
}

void print_kv_info(const char* description, struct kv* kv) {
    printf("\n=== %s ===\n", description);
    printf("键: \"%.*s\"\n", kv->klen, kv->kv);
    printf("klen: %u\n", kv->klen);
    printf("vlen (原始): 0x%x (%u)\n", kv->vlen, kv->vlen);
    printf("vlen & SST_VLEN_MASK: %u\n", kv->vlen & SST_VLEN_MASK);
    printf("是否为墓碑: %s\n", (kv->vlen & SST_VLEN_TS) ? "是" : "否");
    printf("sizeof(struct kv): %zu\n", sizeof(struct kv));
    printf("sst_kv_size() 计算的总大小: %zu\n", sst_kv_size(kv));

    // 如果有值，显示值
    u32 actual_vlen = kv->vlen & SST_VLEN_MASK;
    if (actual_vlen > 0) {
        printf("值: \"%.*s\"\n", actual_vlen, kv->kv + kv->klen);
    } else {
        printf("值: (无)\n");
    }
}

int main() {
    printf("SST KV 大小计算示例\n");
    printf("SST_VLEN_TS = 0x%x (%u)\n", SST_VLEN_TS, SST_VLEN_TS);
    printf("SST_VLEN_MASK = 0x%x (%u)\n", SST_VLEN_MASK, SST_VLEN_MASK);

    // 示例 1: 普通的键值对
    struct kv* normal_kv = create_normal_kv("user123", "Alice");

    // 调试用：创建指向键和值的指针
    char* key_ptr = (char*)normal_kv->kv;
    char* value_ptr = (char*)(normal_kv->kv + normal_kv->klen);

    print_kv_info("普通键值对", normal_kv);

    // 示例 2: 墓碑标记 (删除的键)
    struct kv* tombstone_kv = create_tombstone_kv("user456");
    print_kv_info("墓碑标记 (删除的键)", tombstone_kv);

    // 示例 3: 理论情况 - 带墓碑标记但有值
    struct kv* tombstone_with_value = create_tombstone_with_value_kv("user789", "Bob");
    print_kv_info("墓碑标记但有值 (理论情况)", tombstone_with_value);

    printf("\n=== 关键要点 ===\n");
    printf("1. 对于普通键值对，vlen 直接存储值长度\n");
    printf("2. 对于墓碑标记，vlen 的高位被设置为 SST_VLEN_TS\n");
    printf("3. sst_kv_size() 使用 (vlen & SST_VLEN_MASK) 来获取真实的值长度\n");
    printf("4. 这样确保了无论是否有墓碑标记，计算的大小都是准确的\n");

    // 验证不同计算方式的差异
    printf("\n=== 计算方式对比 (墓碑示例) ===\n");
    printf("错误计算 (不使用掩码): sizeof(kv) + klen + vlen = %zu + %u + %u = %zu\n",
           sizeof(struct kv), tombstone_kv->klen, tombstone_kv->vlen,
           sizeof(struct kv) + tombstone_kv->klen + tombstone_kv->vlen);
    printf("正确计算 (使用掩码): sizeof(kv) + klen + (vlen & MASK) = %zu + %u + %u = %zu\n",
           sizeof(struct kv), tombstone_kv->klen, tombstone_kv->vlen & SST_VLEN_MASK,
           sst_kv_size(tombstone_kv));

    free(normal_kv);
    free(tombstone_kv);
    free(tombstone_with_value);

    return 0;
}
