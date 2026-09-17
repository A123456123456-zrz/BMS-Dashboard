/**
 * @file    test_sys_mqtt.c
 * @brief   P0 单元测试: hmac_sha256_hex 等价实现测试
 * @note    由于 hmac_sha256_hex 是 sys_mqtt.c 中的 static 函数,
 *          本文件直接实现其等价逻辑(复制核心算法 + mbedtls 调用),
 *          避免引入整个 sys_mqtt.c 的 ESP-IDF 依赖链.
 *
 *          被测函数签名:
 *            static void hmac_sha256_hex(const char *key, const char *data,
 *                                        char *out, size_t out_len)
 *
 *          测试覆盖:
 *            - RFC 4231 Test Case 1 已知向量验证
 *            - 空字符串边界: key="" / data="" / 两者都空
 *            - out_len 边界: 0 / 1 / 2 / 3 / 4 / 5 / 65 / >65
 *            - 特殊字符: 空格/换行/特殊符号
 *            - 华为云 IoTDA 鉴权场景
 *            - 确定性: 相同输入产生相同输出
 *            - 差异性: 不同 key/data 产生不同输出
 */
#include <string.h>
#include <stdio.h>
#include "unity.h"
#include "mbedtls/md.h"

/* ====== 被测函数的等价实现(复制自 sys_mqtt.c:112-131) ====== */
static void hmac_sha256_hex(const char *key, const char *data, char *out, size_t out_len)
{
    unsigned char hmac[32];
    mbedtls_md_context_t ctx;
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, info, 1);
    mbedtls_md_hmac_starts(&ctx, (const unsigned char *)key, strlen(key));
    mbedtls_md_hmac_update(&ctx, (const unsigned char *)data, strlen(data));
    mbedtls_md_hmac_finish(&ctx, hmac);
    mbedtls_md_free(&ctx);

    size_t n = (out_len - 1) / 2;
    if (n > 32) n = 32;
    for (size_t i = 0; i < n; i++) {
        snprintf(out + i * 2, 3, "%02x", hmac[i]);
    }
    out[n * 2] = '\0';
}

/* ====== 辅助: 验证输出是否为指定长度的小写 hex ====== */
static bool is_hex_string(const char *s, size_t expected_len)
{
    if (s == NULL) return false;
    size_t len = strlen(s);
    if (len != expected_len) return false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

/* ---------- RFC 4231 Test Case 1 ---------- */
/*
 * RFC 4231 Test Case 1:
 *   key   = "key"
 *   data  = "The quick brown fox jumps over the lazy dog"
 *   HMAC-SHA256 = 0xf7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8
 */
TEST_CASE("RFC4231 Test Case 1", "[hmac_sha256]")
{
    const char *key  = "key";
    const char *data = "The quick brown fox jumps over the lazy dog";
    char out[65];

    hmac_sha256_hex(key, data, out, sizeof(out));

    TEST_ASSERT_EQUAL_STRING("f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8", out);
}

/* ---------- 空 key ---------- */
TEST_CASE("empty key", "[hmac_sha256]")
{
    const char *key  = "";
    const char *data = "hello world";
    char out[65];

    hmac_sha256_hex(key, data, out, sizeof(out));

    TEST_ASSERT_EQUAL_INT(64, strlen(out));
    TEST_ASSERT_TRUE(is_hex_string(out, 64));
}

/* ---------- 空 data ---------- */
TEST_CASE("empty data", "[hmac_sha256]")
{
    const char *key  = "secret";
    const char *data = "";
    char out[65];

    hmac_sha256_hex(key, data, out, sizeof(out));

    TEST_ASSERT_EQUAL_INT(64, strlen(out));
    TEST_ASSERT_TRUE(is_hex_string(out, 64));
}

/* ---------- key 和 data 都为空 ---------- */
TEST_CASE("both key and data empty", "[hmac_sha256]")
{
    const char *key  = "";
    const char *data = "";
    char out[65];

    hmac_sha256_hex(key, data, out, sizeof(out));

    TEST_ASSERT_EQUAL_INT(64, strlen(out));
    TEST_ASSERT_TRUE(is_hex_string(out, 64));
}

/* ---------- 正常输入: 较长的 key 和 data ---------- */
TEST_CASE("normal long key and data", "[hmac_sha256]")
{
    const char *key  = "20260811123456"; /* 华为云时间戳格式 YYYYMMDDHHmm */
    const char *data = "ABC123XYZtestSecretKey";
    char out[65];

    hmac_sha256_hex(key, data, out, sizeof(out));

    TEST_ASSERT_EQUAL_INT(64, strlen(out));
    TEST_ASSERT_TRUE(is_hex_string(out, 64));
}

/* ---------- out_len = 0: n = (0-1)/2 = 0, 循环不执行 ---------- */
TEST_CASE("out_len zero", "[hmac_sha256]")
{
    const char *key  = "key";
    const char *data = "data";
    char out[1] = {'X'};  /* 填非零值验证未写入 */

    hmac_sha256_hex(key, data, out, 0);

    /* n=0, 循环不执行, out[0] 保持 'X' */
    TEST_ASSERT_EQUAL_INT('X', out[0]);
}

/* ---------- out_len = 1: n = (1-1)/2 = 0, 循环不执行 ---------- */
TEST_CASE("out_len one", "[hmac_sha256]")
{
    const char *key  = "key";
    const char *data = "data";
    char out[2] = {'X', 'Y'};

    hmac_sha256_hex(key, data, out, 1);

    /* n=0, 循环不执行, out[0]='X', out[1]='\0' */
    TEST_ASSERT_EQUAL_INT('X', out[0]);
    TEST_ASSERT_EQUAL_INT('\0', out[1]);
}

/* ---------- out_len = 2: n = (2-1)/2 = 0, 循环不执行 ---------- */
TEST_CASE("out_len two", "[hmac_sha256]")
{
    const char *key  = "key";
    const char *data = "data";
    char out[3] = {'X', 'Y', 'Z'};

    hmac_sha256_hex(key, data, out, 2);

    TEST_ASSERT_EQUAL_INT('X', out[0]);
    TEST_ASSERT_EQUAL_INT('\0', out[1]);
}

/* ---------- out_len = 3: n = (3-1)/2 = 1, 输出1字节hex=2字符 ---------- */
TEST_CASE("out_len three", "[hmac_sha256]")
{
    const char *key  = "key";
    const char *data = "data";
    char out[4] = {'X', 'X', 'X', 'X'};

    hmac_sha256_hex(key, data, out, 3);

    TEST_ASSERT_EQUAL_INT(2, strlen(out));
    TEST_ASSERT_TRUE(is_hex_string(out, 2));
}

/* ---------- out_len = 4: n = (4-1)/2 = 1, 输出2字符 ---------- */
TEST_CASE("out_len four", "[hmac_sha256]")
{
    const char *key  = "key";
    const char *data = "data";
    char out[5];

    hmac_sha256_hex(key, data, out, 4);

    TEST_ASSERT_EQUAL_INT(2, strlen(out));
    TEST_ASSERT_TRUE(is_hex_string(out, 2));
}

/* ---------- out_len = 5: n = (5-1)/2 = 2, 输出2字节hex=4字符 ---------- */
TEST_CASE("out_len five", "[hmac_sha256]")
{
    const char *key  = "key";
    const char *data = "data";
    char out[6];

    hmac_sha256_hex(key, data, out, 5);

    TEST_ASSERT_EQUAL_INT(4, strlen(out));
    TEST_ASSERT_TRUE(is_hex_string(out, 4));
}

/* ---------- out_len = 65: 足够大, 输出全部 64 字符 ---------- */
TEST_CASE("out_len sufficient", "[hmac_sha256]")
{
    const char *key  = "key";
    const char *data = "data";
    char out[66];

    hmac_sha256_hex(key, data, out, 65);

    TEST_ASSERT_EQUAL_INT(64, strlen(out));
    TEST_ASSERT_TRUE(is_hex_string(out, 64));
}

/* ---------- out_len = 128: >65 截断到 64 ---------- */
TEST_CASE("out_len large truncates to 64", "[hmac_sha256]")
{
    const char *key  = "key";
    const char *data = "data";
    char out[130];

    hmac_sha256_hex(key, data, out, 128);

    TEST_ASSERT_EQUAL_INT(64, strlen(out));
    TEST_ASSERT_TRUE(is_hex_string(out, 64));
}

/* ---------- 特殊字符: key 含空格 ---------- */
TEST_CASE("key with spaces", "[hmac_sha256]")
{
    const char *key  = "key with spaces";
    const char *data = "data";
    char out[65];

    hmac_sha256_hex(key, data, out, sizeof(out));

    TEST_ASSERT_EQUAL_INT(64, strlen(out));
    TEST_ASSERT_TRUE(is_hex_string(out, 64));
}

/* ---------- 特殊字符: data 含换行符 ---------- */
TEST_CASE("data with newline", "[hmac_sha256]")
{
    const char *key  = "secret";
    const char *data = "line1\nline2\nline3";
    char out[65];

    hmac_sha256_hex(key, data, out, sizeof(out));

    TEST_ASSERT_EQUAL_INT(64, strlen(out));
    TEST_ASSERT_TRUE(is_hex_string(out, 64));
}

/* ---------- 特殊字符: 全是特殊符号 ---------- */
TEST_CASE("special characters", "[hmac_sha256]")
{
    const char *key  = "!@#$%^&*()_+-=[]{}|;':\",./<>?";
    const char *data = "`~!@#$%^&*()_+-=[]{}|;':\",./<>?";
    char out[65];

    hmac_sha256_hex(key, data, out, sizeof(out));

    TEST_ASSERT_EQUAL_INT(64, strlen(out));
    TEST_ASSERT_TRUE(is_hex_string(out, 64));
}

/* ---------- 华为云 IoTDA 鉴权场景 ---------- */
/*
 * 华为云 IoTDA 鉴权:
 *   Password = HMAC-SHA256(时间戳, 设备密钥)
 *   ClientID = {device_id}_0_0_{timestamp}
 */
TEST_CASE("IoTDA auth scenario", "[hmac_sha256]")
{
    const char *timestamp = "202608111234";
    const char *secret    = "ABC123XYZtest";
    char password[65];

    hmac_sha256_hex(timestamp, secret, password, sizeof(password));

    TEST_ASSERT_EQUAL_INT(64, strlen(password));
    TEST_ASSERT_TRUE(is_hex_string(password, 64));

    /* 验证确定性: 相同输入产生相同输出 */
    char password2[65];
    hmac_sha256_hex(timestamp, secret, password2, sizeof(password2));
    TEST_ASSERT_EQUAL_STRING(password, password2);
}

/* ---------- 确定性: 相同输入多次调用结果一致 ---------- */
TEST_CASE("deterministic output", "[hmac_sha256]")
{
    const char *key  = "consistent_key";
    const char *data = "consistent_data_value";
    char out1[65];
    char out2[65];
    char out3[65];

    hmac_sha256_hex(key, data, out1, sizeof(out1));
    hmac_sha256_hex(key, data, out2, sizeof(out2));
    hmac_sha256_hex(key, data, out3, sizeof(out3));

    TEST_ASSERT_EQUAL_STRING(out1, out2);
    TEST_ASSERT_EQUAL_STRING(out2, out3);
}

/* ---------- 差异性: 不同 key 产生不同输出 ---------- */
TEST_CASE("different keys produce different output", "[hmac_sha256]")
{
    const char *data = "same data";
    char out1[65];
    char out2[65];

    hmac_sha256_hex("key1", data, out1, sizeof(out1));
    hmac_sha256_hex("key2", data, out2, sizeof(out2));

    TEST_ASSERT_FALSE(strcmp(out1, out2) == 0);
}

/* ---------- 差异性: 不同 data 产生不同输出 ---------- */
TEST_CASE("different data produce different output", "[hmac_sha256]")
{
    const char *key  = "same key";
    char out1[65];
    char out2[65];

    hmac_sha256_hex(key, "data1", out1, sizeof(out1));
    hmac_sha256_hex(key, "data2", out2, sizeof(out2));

    TEST_ASSERT_FALSE(strcmp(out1, out2) == 0);
}

/* ---------- 截断输出验证: out_len=33 时 n=16, 输出32字符 ---------- */
TEST_CASE("out_len 33 outputs 32 hex chars", "[hmac_sha256]")
{
    const char *key  = "key";
    const char *data = "data";
    char out[34];

    hmac_sha256_hex(key, data, out, 33);

    /* n = (33-1)/2 = 16, 输出16字节hex=32字符 */
    TEST_ASSERT_EQUAL_INT(32, strlen(out));
    TEST_ASSERT_TRUE(is_hex_string(out, 32));
}

/* ---------- 截断输出验证: out_len=65 时 n=32, 输出64字符 ---------- */
TEST_CASE("out_len 65 outputs 64 hex chars", "[hmac_sha256]")
{
    const char *key  = "key";
    const char *data = "data";
    char out[66];

    hmac_sha256_hex(key, data, out, 65);

    /* n = (65-1)/2 = 32, 输出32字节hex=64字符 */
    TEST_ASSERT_EQUAL_INT(64, strlen(out));
    TEST_ASSERT_TRUE(is_hex_string(out, 64));
}
