/**
 * @file cloud_crypto.c
 * @brief 云平台认证加密工具实现
 *
 * 使用 mbedtls HMAC/MD 模块计算 HMAC-SHA1 / HMAC-SHA256，
 * 输出小写十六进制字符串，供阿里云/腾讯云一机一密签名使用。
 *
 * @note 整个实现用 CONFIG_MBEDTLS 守护。未启用时各函数返回 -ENOTSUP，
 *       cloud provider 检测到 -ENOTSUP 后回退占位密码并 LOG_WRN 告知。
 */

#include "cloud_crypto.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

LOG_MODULE_REGISTER(cloud_crypto, CONFIG_SYS_LOG_LEVEL);

/* =============================================================================
 * HMAC 实现（mbedtls 守护）
 * ============================================================================= */

#if defined(CONFIG_MBEDTLS)

#include <mbedtls/md.h>

/**
 * @brief 通用 HMAC 计算，输出小写十六进制字符串
 *
 * @param md_type   mbedtls MD 类型（MBEDTLS_MD_SHA1 / MBEDTLS_MD_SHA256）
 * @param key       HMAC 密钥
 * @param key_len   密钥长度（字节）
 * @param msg       消息
 * @param msg_len   消息长度（字节）
 * @param out_hex   输出缓冲（调用方保证足够大）
 * @param out_size  输出缓冲大小
 * @return 0 成功；-EINVAL 参数无效；-EIO mbedtls 计算失败
 */
static int hmac_to_hex(mbedtls_md_type_t md_type,
                        const uint8_t* key, size_t key_len,
                        const uint8_t* msg, size_t msg_len,
                        char* out_hex, size_t out_size)
{
    if (key == NULL || msg == NULL || out_hex == NULL ||
        key_len == 0 || out_size == 0) {
        return -EINVAL;
    }

    const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(md_type);
    if (md_info == NULL) {
        LOG_ERR("mbedtls_md_info_from_type 失败，MD 类型 %d 不支持", (int)md_type);
        return -EIO;
    }

    /* 确认输出缓冲足够容纳十六进制字符串 */
    size_t digest_size = (size_t)mbedtls_md_get_size(md_info);
    size_t hex_need = digest_size * 2 + 1; /* 每字节两个 hex 字符 + NUL */
    if (out_size < hex_need) {
        LOG_ERR("输出缓冲不足：需要 %zu 字节，实际 %zu 字节", hex_need, out_size);
        return -EINVAL;
    }

    uint8_t digest[32]; /* SHA256=32B，SHA1=20B，此数组均满足 */
    int ret = mbedtls_md_hmac(md_info,
                               key, key_len,
                               msg, msg_len,
                               digest);
    if (ret != 0) {
        LOG_ERR("mbedtls_md_hmac 失败: %d", ret);
        return -EIO;
    }

    /* 转换为小写十六进制字符串 */
    static const char hex_chars[] = "0123456789abcdef";
    for (size_t i = 0; i < digest_size; i++) {
        out_hex[i * 2]     = hex_chars[(digest[i] >> 4) & 0x0F];
        out_hex[i * 2 + 1] = hex_chars[digest[i] & 0x0F];
    }
    out_hex[digest_size * 2] = '\0';

    return 0;
}

int gateway_hmac_sha1_hex(const uint8_t* key, size_t key_len,
                           const uint8_t* msg, size_t msg_len,
                           char* out_hex, size_t out_size)
{
    return hmac_to_hex(MBEDTLS_MD_SHA1, key, key_len, msg, msg_len, out_hex, out_size);
}

int gateway_hmac_sha256_hex(const uint8_t* key, size_t key_len,
                             const uint8_t* msg, size_t msg_len,
                             char* out_hex, size_t out_size)
{
    return hmac_to_hex(MBEDTLS_MD_SHA256, key, key_len, msg, msg_len, out_hex, out_size);
}

#else /* !CONFIG_MBEDTLS */

int gateway_hmac_sha1_hex(const uint8_t* key, size_t key_len,
                           const uint8_t* msg, size_t msg_len,
                           char* out_hex, size_t out_size)
{
    ARG_UNUSED(key);
    ARG_UNUSED(key_len);
    ARG_UNUSED(msg);
    ARG_UNUSED(msg_len);
    ARG_UNUSED(out_hex);
    ARG_UNUSED(out_size);
    return -ENOTSUP;
}

int gateway_hmac_sha256_hex(const uint8_t* key, size_t key_len,
                             const uint8_t* msg, size_t msg_len,
                             char* out_hex, size_t out_size)
{
    ARG_UNUSED(key);
    ARG_UNUSED(key_len);
    ARG_UNUSED(msg);
    ARG_UNUSED(msg_len);
    ARG_UNUSED(out_hex);
    ARG_UNUSED(out_size);
    return -ENOTSUP;
}

#endif /* CONFIG_MBEDTLS */
