/**
 * @file cloud_aliyun.h
 * @brief 阿里云 IoT 平台 Provider
 *
 * 对接阿里云物联网平台，支持一机一密 HMAC-SHA1 认证、Alink JSON、物模型 Topic。
 * TODO: 认证密码需在 mqtt_connect 前动态计算（HMAC-SHA1(DeviceSecret, content)）。
 */

#ifndef CLOUD_ALIYUN_H
#define CLOUD_ALIYUN_H

#include "cloud_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 获取阿里云 Provider 接口指针 */
const cloud_provider_t* cloud_aliyun_get_provider(void);

#ifdef __cplusplus
}
#endif

#endif /* CLOUD_ALIYUN_H */
