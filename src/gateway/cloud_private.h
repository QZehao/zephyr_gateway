/**
 * @file cloud_private.h
 * @brief 私有 MQTT Broker Provider
 *
 * 基于 protocol_mqtt 的通用 MQTT 能力，对接私有/自部署 MQTT Broker。
 * 这是默认的 Provider，行为与重构前的 cloud_upload 直接发 MQTT 完全一致。
 */

#ifndef CLOUD_PRIVATE_H
#define CLOUD_PRIVATE_H

#include "cloud_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 获取私有云 Provider 接口指针 */
const cloud_provider_t* cloud_private_get_provider(void);

#ifdef __cplusplus
}
#endif

#endif /* CLOUD_PRIVATE_H */
