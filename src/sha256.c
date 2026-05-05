/*
 * sha256.c — SHA-256 消息摘要算法实现 / SHA-256 message digest implementation
 *
 * 实现了 FIPS 180-4 标准的 SHA-256 哈希算法。用于对用户密码进行
 * 单向哈希处理: 明文密码 → sha256_hash() → 64 字符十六进制字符串存储。
 *
 * 核心组件:
 *   - sha256_transform(): 64 轮压缩函数 (处理 512 位数据块)
 *   - sha256_init(): 初始化 8 个 32 位哈希状态字 (取前 8 个质数平方根小数部分)
 *   - sha256_update(): 追加数据，满 64 字节自动触发压缩
 *   - sha256_final(): 填充 (1 + 0 填充 + 64 位长度) → 最终压缩 → 输出 256 位摘要
 *   - sha256_hash(): 便捷一步哈希
 *   - sha256_hex(): 字节→十六进制字符串
 *
 * Implements FIPS 180-4 SHA-256. Used for one-way password hashing:
 * plaintext → sha256_hash() → 64-char hex string stored in users.txt.
 */

#include "sha256.h"
#include <stdio.h>

/* ==================  SHA-256 核心运算宏 (FIPS 180-4 §4.1.2) ================== */

/* 循环右移 / Circular right shift */
#define ROTR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))

/* Ch: 选择函数 (x?y:z 按位版) / Choose: bitwise (x?y:z) */
#define CH(x,y,z)  (((x) & (y)) ^ (~(x) & (z)))

/* Maj: 多数函数 (3 选 2) / Majority: bitwise majority of 3 inputs */
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))

/* Σ0 (大写西格玛 0): 用于压缩函数中对 A 的变换 / Big Sigma 0 for A in compression */
#define EP0(x) (ROTR(x, 2) ^ ROTR(x,13) ^ ROTR(x,22))

/* Σ1 (大写西格玛 1): 用于压缩函数中对 E 的变换 / Big Sigma 1 for E in compression */
#define EP1(x) (ROTR(x, 6) ^ ROTR(x,11) ^ ROTR(x,25))

/* σ0 (小写西格玛 0): 用于消息调度 / Small Sigma 0 for message schedule */
#define SIG0(x) (ROTR(x, 7) ^ ROTR(x,18) ^ ((x) >> 3))

/* σ1 (小写西格玛 1): 用于消息调度 / Small Sigma 1 for message schedule */
#define SIG1(x) (ROTR(x,17) ^ ROTR(x,19) ^ ((x) >> 10))

/* 64 轮常量 K: 前 64 个质数的立方根小数部分的前 32 位
   64 round constants: first 32 bits of fractional parts of cube roots of first 64 primes */
static const uint32_t k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

/* ==================  SHA-256 压缩函数 / Compression Function ================== */

/* 处理 512 位 (64 字节) 数据块: 消息调度 → 64 轮压缩 → 累加到状态
   Process 512-bit (64-byte) block: message schedule → 64 rounds → add to state */
static void sha256_transform(SHA256_CTX *ctx, const uint8_t data[]) {
    uint32_t a,b,c,d,e,f,g,h,i,j,t1,t2,m[64];

    /* 消息调度: 前 16 字来自输入，后 48 字由递推公式生成
       Message schedule: first 16 words from input, next 48 from recurrence */
    for (i=0,j=0;i<16;++i,j+=4)
        m[i] = ((uint32_t)data[j]<<24)|((uint32_t)data[j+1]<<16)|((uint32_t)data[j+2]<<8)|(uint32_t)data[j+3];
    for (;i<64;++i)
        m[i] = SIG1(m[i-2])+m[i-7]+SIG0(m[i-15])+m[i-16];

    /* 初始化工作变量 / Initialize working variables */
    a=ctx->state[0];b=ctx->state[1];c=ctx->state[2];d=ctx->state[3];
    e=ctx->state[4];f=ctx->state[5];g=ctx->state[6];h=ctx->state[7];

    /* 64 轮压缩 / 64 compression rounds */
    for (i=0;i<64;++i) {
        t1=h+EP1(e)+CH(e,f,g)+k[i]+m[i];  /* T1 = h + Σ1(e) + Ch(e,f,g) + K[i] + W[i] */
        t2=EP0(a)+MAJ(a,b,c);             /* T2 = Σ0(a) + Maj(a,b,c) */
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }

    /* 中间哈希值累加 / Add compressed chunk to hash state */
    ctx->state[0]+=a;ctx->state[1]+=b;ctx->state[2]+=c;ctx->state[3]+=d;
    ctx->state[4]+=e;ctx->state[5]+=f;ctx->state[6]+=g;ctx->state[7]+=h;
}

/* 初始化: 设置初始哈希值 H(0) (取前 8 个质数平方根小数部分的前 32 位)
   Initialize: set initial hash values (fractional parts of square roots of first 8 primes) */
void sha256_init(SHA256_CTX *ctx) {
    ctx->datalen=0;ctx->bitlen=0;
    ctx->state[0]=0x6a09e667;ctx->state[1]=0xbb67ae85;ctx->state[2]=0x3c6ef372;ctx->state[3]=0xa54ff53a;
    ctx->state[4]=0x510e527f;ctx->state[5]=0x9b05688c;ctx->state[6]=0x1f83d9ab;ctx->state[7]=0x5be0cd19;
}

/* 追加数据: 逐字节填入缓冲区，每满 64 字节触发一次压缩
   Update: buffer input bytes, trigger transform every 64 bytes */
void sha256_update(SHA256_CTX *ctx, const uint8_t *data, size_t len) {
    for (size_t i=0;i<len;++i) {
        ctx->data[ctx->datalen]=data[i];
        ctx->datalen++;
        if (ctx->datalen==64) {
            sha256_transform(ctx,ctx->data);
            ctx->bitlen+=512;
            ctx->datalen=0;
        }
    }
}

/* 完成哈希: 填充 (0x80 + 零填充 + 64 位长度) → 最终压缩 → 输出 32 字节摘要
   Finalize: padding (0x80 + zero pad + 64-bit length) → final transform → output 32-byte digest */
void sha256_final(SHA256_CTX *ctx, uint8_t hash[SHA256_DIGEST_SIZE]) {
    uint32_t i=ctx->datalen;
    ctx->data[i++]=0x80;    /* 追加 1 位后接 0 / append bit '1' followed by zeros */
    if (i>56) {             /* 如果剩余空间不够放 64 位长度，先压缩当前块 */
        while (i<64) ctx->data[i++]=0x00;
        sha256_transform(ctx,ctx->data);
        i=0;
    }
    while (i<56) ctx->data[i++]=0x00; /* 零填充至 56 字节 / zero-pad to 56 bytes */
    ctx->bitlen+=ctx->datalen*8;      /* 更新总位数 / update total bit count */
    /* 64 位大端序消息长度写入最后 8 字节 / 64-bit big-endian message length */
    ctx->data[56]=(uint8_t)(ctx->bitlen>>56);
    ctx->data[57]=(uint8_t)(ctx->bitlen>>48);
    ctx->data[58]=(uint8_t)(ctx->bitlen>>40);
    ctx->data[59]=(uint8_t)(ctx->bitlen>>32);
    ctx->data[60]=(uint8_t)(ctx->bitlen>>24);
    ctx->data[61]=(uint8_t)(ctx->bitlen>>16);
    ctx->data[62]=(uint8_t)(ctx->bitlen>>8);
    ctx->data[63]=(uint8_t)(ctx->bitlen);
    sha256_transform(ctx,ctx->data);
    /* 8 个状态字 → 32 字节大端序输出 / 8 state words → 32-byte big-endian output */
    for (i=0;i<4;++i) {
        hash[i]    =(uint8_t)((ctx->state[0]>>(24-i*8))&0xff);
        hash[i+4]  =(uint8_t)((ctx->state[1]>>(24-i*8))&0xff);
        hash[i+8]  =(uint8_t)((ctx->state[2]>>(24-i*8))&0xff);
        hash[i+12] =(uint8_t)((ctx->state[3]>>(24-i*8))&0xff);
        hash[i+16] =(uint8_t)((ctx->state[4]>>(24-i*8))&0xff);
        hash[i+20] =(uint8_t)((ctx->state[5]>>(24-i*8))&0xff);
        hash[i+24] =(uint8_t)((ctx->state[6]>>(24-i*8))&0xff);
        hash[i+28] =(uint8_t)((ctx->state[7]>>(24-i*8))&0xff);
    }
}

/* 一步哈希: init → update → final / One-shot hash: init → update → final */
void sha256_hash(const uint8_t *data, size_t len, uint8_t hash[SHA256_DIGEST_SIZE]) {
    SHA256_CTX ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, hash);
}

/* 32 字节 → 64 字符小写十六进制字符串 / 32 bytes → 64-char lowercase hex string */
void sha256_hex(const uint8_t hash[SHA256_DIGEST_SIZE], char hex[SHA256_HEX_SIZE]) {
    for (int i=0;i<SHA256_DIGEST_SIZE;i++)
        sprintf(hex+i*2,"%02x",hash[i]);
    hex[64]='\0';
}
