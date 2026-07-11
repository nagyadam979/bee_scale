/*
 * esp_log.h — host-oldali stub (node_queue_sim tesztpad).
 * Az ESP_LOGx makrók stderr-re printelnek, ha g_sim_log_enabled != 0
 * (SIM_LOG=1 env a fuzz futtatásnál). Alapból némák, hogy a fuzz gyors legyen.
 */
#ifndef MOCK_ESP_LOG_H
#define MOCK_ESP_LOG_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

extern int g_sim_log_enabled;

#ifdef __cplusplus
}
#endif

#define SIM_LOG_IMPL(lvl, tag, fmt, ...) \
    do { \
        if (g_sim_log_enabled) { \
            fprintf(stderr, "%s (%s): " fmt "\n", lvl, tag, ##__VA_ARGS__); \
        } \
    } while (0)

#define ESP_LOGE(tag, fmt, ...) SIM_LOG_IMPL("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) SIM_LOG_IMPL("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) SIM_LOG_IMPL("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) SIM_LOG_IMPL("D", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) SIM_LOG_IMPL("V", tag, fmt, ##__VA_ARGS__)

#endif /* MOCK_ESP_LOG_H */
