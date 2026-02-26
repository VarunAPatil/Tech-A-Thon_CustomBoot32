/*
 * firebase_task.c – periodic sensor data upload to Firebase Realtime DB
 * -----------------------------------------------------------------------
 * Reads from shared_data.h (protected by sensor_data_mutex):
 *   shared_temp_x10   → temperature in °C × 10  (int)
 *   shared_units       → cell count              (int)
 *   shared_gps_lat     → latitude string         e.g. "1234.5678"
 *   shared_gps_lon     → longitude string        e.g. "09876.5432"
 *   shared_gps_ns      → N/S indicator           e.g. "N"
 *   shared_gps_ew      → E/W indicator           e.g. "E"
 *
 * Firebase node written:
 *   device1/sensors
 *   {
 *     "temp"  : 27.44,
 *     "lat"   : "1234.5678N",
 *     "lon"   : "09876.5432E",
 *     "cells" : 3
 *   }
 */

#include "firebase_task.h"
#include "shared_data.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

static const char *TAG = "Firebase";

/* ---- Firebase endpoint ------------------------------------------------- */
#define FIREBASE_BASE_URL "https://vaxi-7f913-default-rtdb.firebaseio.com/device1/temp.json?auth=jtzovCmVoVHGQ54VtdONkTq0jLxJVpLgaiJTuD5d"

/* =========================================================================
 * send_to_firebase – build JSON from snapshot and PUT to Firebase
 * =========================================================================*/
static void send_to_firebase(float temp, const char *lat, const char *ns,
                              const char *lon, const char *ew, int cells)
{
    /* Build JSON payload */
    char payload[128];
    snprintf(payload, sizeof(payload),
             "{\"temp\":%.2f,\"lat\":\"%s%s\",\"lon\":\"%s%s\",\"cells\":%d}",
             temp, lat, ns, lon, ew, cells);

    esp_http_client_config_t cfg = {
        .url               = FIREBASE_BASE_URL,
        .method            = HTTP_METHOD_PUT,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, payload, strlen(payload));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Sent (HTTP %d) → %s", status, payload);
    } else {
        ESP_LOGE(TAG, "HTTP PUT failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}

/* =========================================================================
 * firebase_task – FreeRTOS entry point
 * =========================================================================*/
void firebase_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Firebase task started – uploading every %d ms",
             FIREBASE_UPLOAD_INTERVAL_MS);

    /* Give WiFi (started by ota_task) a moment to connect on first boot */
    vTaskDelay(pdMS_TO_TICKS(6000));

    while (1) {
        /* ---- Snapshot shared data under mutex -------------------------- */
        float temp  = 0.0f;
        int   cells = 0;
        char  lat[20] = "----";
        char  lon[20] = "----";
        char  ns[4]   = "";
        char  ew[4]   = "";

        if (sensor_data_mutex != NULL &&
            xSemaphoreTake(sensor_data_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {

            temp  = shared_temp_x10 / 10.0f;
            cells = shared_units;
            strncpy(lat, shared_gps_lat, sizeof(lat) - 1);
            strncpy(lon, shared_gps_lon, sizeof(lon) - 1);
            strncpy(ns,  shared_gps_ns,  sizeof(ns)  - 1);
            strncpy(ew,  shared_gps_ew,  sizeof(ew)  - 1);

            xSemaphoreGive(sensor_data_mutex);
        } else {
            ESP_LOGW(TAG, "Could not acquire mutex – skipping upload");
            vTaskDelay(pdMS_TO_TICKS(FIREBASE_UPLOAD_INTERVAL_MS));
            continue;
        }

        /* ---- Upload to Firebase ---------------------------------------- */
        send_to_firebase(temp, lat, ns, lon, ew, cells);

        vTaskDelay(pdMS_TO_TICKS(FIREBASE_UPLOAD_INTERVAL_MS));
    }
}
