/**
 * @file cloud_crypto.h
 * @brief 云平台认证加密工具接口
 *
 * 提供 HMAC-SHA1 / HMAC-SHA256 签名，输出小写十六进制字符串。
 * 用于阿里云（HMAC-SHA1）和腾讯云（HMAC-SHA256）一机一密签名计算。
 *
 * 依赖 CONFIG_MBEDTLS=y（mbedtls/md.h）。未启用时各函数返回 -ENOTSUP。
 *
 * @note 证书/密钥均为短期内存操作，不做持久化。调用方负责密钥生命周期管理。
 */

#ifndef CLOUD_CRYPTO_H
#define CLOUD_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 计算 HMAC-SHA1 并输出小写十六进制字符串
 *
 * @param key       HMAC 密钥（阿里云 DeviceSecret）
 * @param key_len   密钥长度（字节）
 * @param msg       待签名消息
 * @param msg_len   消息长度（字节）
 * @param out_hex   输出缓冲（至少 41 字节：SHA1=20B → 40 字符 + NUL）
 * @param out_size  输出缓冲大小
 * @return 0 成功；-ENOTSUP mbedtls 未启用；-EINVAL 参数无效；-EIO 签名计算失败
 *
 * @note 需 CONFIG_MBEDTLS=y 且 CONFIG_MBEDTLS_MD_C=y。
 */
int gateway_hmac_sha1_hex(const uint8_t* key, size_t key_len,
                           const uint8_t* msg, size_t msg_len,
                           char* out_hex, size_t out_size);

/**
 * @brief 计算 HMAC-SHA256 并输出小写十六进制字符串
 *
 * @param key       HMAC 密钥（腾讯云 DeviceSecret）
 * @param key_len   密钥长度（字节）
 * @param msg       待签名消息
 * @param msg_len   消息长度（字节）
 * @param out_hex   输出缓冲（至少 65 字节：SHA256=32B → 64 字符 + NUL）
 * @param out_size  输出缓冲大小
 * @return 0 成功；-ENOTSUP mbedtls 未启用；-EINVAL 参数无效；-EIO 签名计算失败
 *
 * @note 需 CONFIG_MBEDTLS=y 且 CONFIG_MBEDTLS_MD_C=y。
 */
int gateway_hmac_sha256_hex(const uint8_t* key, size_t key_len,
                             const uint8_t* msg, size_t msg_len,
                             char* out_hex, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* CLOUD_CRYPTO_H */
