#pragma once

#include <stdint.h>

// MurmurHash3_x86_32 - 针对 x86 32 位平台优化的版本
// key  : 输入键
// len  : 键的长度 (字节)
// seed : 种子值
// out  : 输出的 32 位哈希值
void MurmurHash3_x86_32(const void* key, int len, uint32_t seed, void* out);
