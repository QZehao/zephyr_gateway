/**
 * @file cloud_tencent.h
 * @brief 腾讯云 IoT Hub Provider
 *
 * 对接腾讯云物联网通信平台，支持密钥认证、腾讯云 Topic 格式。
 * TODO: 实现设备密钥签名和腾讯云特定 Topic 构造。
 */

#ifndef CLOUD_TENCENT_H
#define CLOUD_TENCENT_H

#include "cloud_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 获取腾讯云 Provider 接口指针 */
const cloud_provider_t* cloud_tencent_get_provider(void);

#ifdef __cplusplus
}
#endif

#endif /* CLOUD_TENCENT_H */
