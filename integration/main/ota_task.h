#ifndef OTA_TASK_H
#define OTA_TASK_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* -------------------------------------------------------------------------
 * OTA Task – WiFi + HTTP OTA via embedded webpage (ESP32-S3)
 *
 * Priority model:
 *   - Idle/standby  : OTA_TASK_PRIORITY_LOW  (1) – lowest worker priority
 *   - Active update : OTA_TASK_PRIORITY_HIGH (6) – above every sensor task
 *
 * The task handle is exposed so other modules can query or manipulate the
 * task if needed (e.g. suspend during critical operations).
 * -------------------------------------------------------------------------*/

#define OTA_TASK_PRIORITY_LOW   1   /* Below all sensor tasks (2-5) */
#define OTA_TASK_PRIORITY_HIGH  6   /* Above all sensor tasks       */
#define OTA_TASK_STACK_SIZE     8192

/* Target WiFi credentials – edit before flashing */
#define OTA_WIFI_SSID     "1234"
#define OTA_WIFI_PASSWORD "123456789"

/* HTTP server port for the OTA webpage */
#define OTA_HTTP_PORT     80

/* Handle exposed for optional external control */
extern TaskHandle_t ota_task_handle;

/**
 * @brief  FreeRTOS task entry point for OTA.
 *
 * Connects to the configured WiFi AP, starts an httpd server that serves
 * an upload page, and awaits firmware.  On receiving a valid binary it
 * performs the OTA update and reboots.
 *
 * Launch with:
 *   xTaskCreatePinnedToCore(ota_task, "ota_task", OTA_TASK_STACK_SIZE,
 *                           NULL, OTA_TASK_PRIORITY_LOW,
 *                           &ota_task_handle, 0);
 */
void ota_task(void *pvParameters);

#endif /* OTA_TASK_H */
