/**
 * @file gateway_events.c
 * @brief 工业网关事件数据格式辅助函数实现
 */

#include "gateway_events.h"
#include <stdio.h>
#include <string.h>

int gateway_sensor_to_json(const gateway_sensor_data_t* data, char* buf, size_t buf_len)
{
    if (data == NULL || buf == NULL || buf_len == 0) {
        return -1;
    }

    const char* type_str = gateway_sensor_type_str(data->sensor_type);

    return snprintf(buf, buf_len,
                    "{\"ts\":%lu,\"ch\":%u,\"type\":\"%s\",\"val\":%.2f,\"raw\":%u}",
                    (unsigned long)data->timestamp,
                    (unsigned)data->channel_id,
                    type_str,
                    (double)data->value,
                    (unsigned)data->raw_u16);
}

int gateway_anomaly_to_json(const gateway_anomaly_event_t* evt, char* buf, size_t buf_len)
{
    if (evt == NULL || buf == NULL || buf_len == 0) {
        return -1;
    }

    const char* level_str = gateway_anomaly_level_str(evt->level);
    const char* type_str = gateway_sensor_type_str(evt->sensor_type);

    return snprintf(buf, buf_len,
                    "{\"ts\":%lu,\"sensor\":\"%s\",\"lvl\":\"%s\","
                    "\"val\":%.2f,\"mean\":%.2f,\"std\":%.2f,\"sigma\":%.1f}",
                    (unsigned long)evt->timestamp,
                    type_str,
                    level_str,
                    (double)evt->current_value,
                    (double)evt->baseline_mean,
                    (double)evt->baseline_stddev,
                    (double)evt->threshold_sigma);
}

const char* gateway_sensor_type_str(uint8_t sensor_type)
{
    switch (sensor_type) {
    case SENSOR_TYPE_CURRENT:
        return "current";
    case SENSOR_TYPE_TEMPERATURE:
        return "temperature";
    case SENSOR_TYPE_VOLTAGE:
        return "voltage";
    case SENSOR_TYPE_PRESSURE:
        return "pressure";
    case SENSOR_TYPE_HUMIDITY:
        return "humidity";
    default:
        return "unknown";
    }
}

const char* gateway_anomaly_level_str(uint8_t level)
{
    switch (level) {
    case 0:
        return "warning";
    case 1:
        return "critical";
    case 2:
        return "emergency";
    default:
        return "unknown";
    }
}
