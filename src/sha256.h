/*
 * sha256.h — SHA-256 密码哈希模块 / SHA-256 password hashing module
 *
 * 本模块实现了 FIPS 180-4 标准的 SHA-256 消息摘要算法，用于对用户密码
 * 进行单向哈希处理。系统在用户注册/登录时将明文密码转为 64 字符十六进制
 * 哈希字符串存储，避免明文密码泄露风险。
 *
 * This module implements the FIPS 180-4 SHA-256 message digest algorithm
 * for one-way hashing of user passwords. Passwords are stored as 64-char
 * hex strings, never as plaintext.
 */

#ifndef SHA256_H
#define SHA256_H

#include <stdint.h>
#include <string.h>

/* SHA-256 输出: 32 字节原始哈希 / 32-byte raw digest */
#define SHA256_DIGEST_SIZE 32
/* SHA-256 输出: 64 字符十六进制字符串 + '\0' = 65 字节 */
#define SHA256_HEX_SIZE    65

/* SHA-256 上下文，保存运算过程中的中间状态
   SHA-256 context holding intermediate computation state */
typedef struct {
    uint8_t  data[64];    /* 当前 512 位数据块 / current 512-bit data block */
    uint32_t datalen;     /* data[] 中已缓存的字节数 / bytes buffered in data[] */
    uint64_t bitlen;      /* 已处理的总位数 / total bits processed so far */
    uint32_t state[8];    /* 8 个 32 位哈希状态字 (A-H) / 8×32-bit hash state words */
} SHA256_CTX;

/* 初始化 SHA-256 上下文 / Initialize SHA-256 context */
void sha256_init(SHA256_CTX *ctx);

/* 追加数据到哈希运算中，可多次调用 / Feed data into the hash, callable multiple times */
void sha256_update(SHA256_CTX *ctx, const uint8_t *data, size_t len);

/* 完成哈希运算，输出 32 字节原始摘要 / Finalize and output 32-byte raw digest */
void sha256_final(SHA256_CTX *ctx, uint8_t hash[SHA256_DIGEST_SIZE]);

/* 便捷函数: 一步完成 init→update→final / Convenience: one-shot hash */
void sha256_hash(const uint8_t *data, size_t len, uint8_t hash[SHA256_DIGEST_SIZE]);

/* 将 32 字节哈希转为 64 字符十六进制字符串 (小写) / Convert 32-byte hash to 64-char lowercase hex string */
void sha256_hex(const uint8_t hash[SHA256_DIGEST_SIZE], char hex[SHA256_HEX_SIZE]);

#endif
