/**
 * @file cloud_aws.h
 * @brief AWS IoT Core Provider
 *
 * 对接 AWS IoT Core，支持 X.509 证书认证、Device Shadow Topic。
 * TODO: 需配置 TLS（CONFIG_NET_TLS、CONFIG_MBEDTLS）和证书链加载。
 */

#ifndef CLOUD_AWS_H
#define CLOUD_AWS_H

#include "cloud_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 获取 AWS Provider 接口指针 */
const cloud_provider_t* cloud_aws_get_provider(void);

#ifdef __cplusplus
}
#endif

#endif /* CLOUD_AWS_H */
