/*
 * firebase_task.c – periodic sensor upload to Firebase Realtime DB
 * -----------------------------------------------------------------
 * Each cycle:
 *   1. Wait up to SENSOR_TIMEOUT_MS for each sensor's ready flag.
 *      If a sensor doesn't respond in time, use the last known /
 *      default value (0.0 for temp, 0 for cells, "--" for GPS).
 *   2. Snapshot shared_data.
 *   3. Clear ready flags.
 *   4. PUT JSON to Firebase.
 *   5. Give all three go-semaphores so sensors start their next reading.
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
#include "esp_timer.h"

static const char *TAG = "Firebase";

/* Max time (ms) to wait for a single sensor to set its ready flag.
 * temp_task alone takes ~750 ms (DS18B20 conversion), so 2000 ms is generous. */
#define SENSOR_TIMEOUT_MS  2000

/* Overall upload interval: gives sensors time to settle between cycles. */
#define UPLOAD_INTERVAL_MS 1000

/* ---- Firebase endpoint ------------------------------------------------- */
/* POST to readings.json → Firebase appends a new node per upload (history log) */
#define FIREBASE_BASE_URL \
    "https://vaxi-7f913-default-rtdb.firebaseio.com/readings.json?auth=jtzovCmVoVHGQ54VtdONkTq0jLxJVpLgaiJTuD5d"

/* =========================================================================
 * wait_for_flag – poll one volatile ready flag with a timeout.
 * Returns 1 if the flag was set before the timeout, 0 if timed out.
 * =========================================================================*/
static int wait_for_flag(volatile int *flag, int timeout_ms)
{
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        int val = 0;
        if (ready_mutex &&
            xSemaphoreTake(ready_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            val = *flag;
            xSemaphoreGive(ready_mutex);
        }
        if (val) return 1;
        vTaskDelay(pdMS_TO_TICKS(50));
        elapsed += 50;
    }
    return 0; // timed out – sensor didn't respond
}

/* =========================================================================
 * send_to_firebase – build JSON and POST to Firebase (appends new log entry)
 * =========================================================================*/
static void send_to_firebase(float temp, const char *lat, const char *ns,
                              const char *lon, const char *ew, int cells)
{
    uint64_t elapsed_ms = esp_timer_get_time() / 1000;

    /* Build JSON payload — POST creates a new child node each time:
     * {
     *   "elapsed_ms": 12345,
     *   "Temperature": 27.44,
     *   "Location": { "Latitude": "1234.5678N", "Longitude": "09876.5432E" },
     *   "Vaccine_Units": 3
     * }
     */
    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"elapsed_ms\":%llu,"
             "\"Temperature\":%.2f,"
             "\"Location\":{\"Latitude\":\"%s%s\",\"Longitude\":\"%s%s\"},"
             "\"Vaccine_Units\":%d}",
             elapsed_ms, temp, lat, ns, lon, ew, cells);

    esp_http_client_config_t cfg = {
        .url               = FIREBASE_BASE_URL,
        .method            = HTTP_METHOD_POST,  // POST appends; PUT overwrites
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { ESP_LOGE(TAG, "Failed to init HTTP client"); return; }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, payload, strlen(payload));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "Logged (HTTP %d) -> %s",
                 esp_http_client_get_status_code(client), payload);
    else
        ESP_LOGE(TAG, "HTTP POST failed: %s", esp_err_to_name(err));

    esp_http_client_cleanup(client);
}

/* =========================================================================
 * firebase_task – FreeRTOS entry point
 * =========================================================================*/
void firebase_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Firebase task started (sensor timeout = %d ms)",
             SENSOR_TIMEOUT_MS);

    /* Give WiFi (started by ota_task) time to connect */
    vTaskDelay(pdMS_TO_TICKS(6000));

    while (1) {

        /* ---- Wait for each sensor (with timeout) ----------------------- */
        int temp_ok     = wait_for_flag(&ready_temp,     SENSOR_TIMEOUT_MS);
        int loadcell_ok = wait_for_flag(&ready_loadcell, SENSOR_TIMEOUT_MS);
        int gps_ok      = wait_for_flag(&ready_gps,      SENSOR_TIMEOUT_MS);

        if (!temp_ok)     ESP_LOGW(TAG, "Temp timeout – using last/default");
        if (!loadcell_ok) ESP_LOGW(TAG, "Load-cell timeout – using last/default");
        if (!gps_ok)      ESP_LOGW(TAG, "GPS timeout – using default (--)");

        /* ---- Snapshot shared_data (defaults already set at init) ------- */
        float temp  = 0.0f;
        int   cells = 0;
        char  lat[20] = "--";
        char  lon[20] = "--";
        char  ns[4]   = "";
        char  ew[4]   = "";

        if (sensor_data_mutex &&
            xSemaphoreTake(sensor_data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {

            temp  = shared_temp_x100 / 100.0f;
            cells = shared_units;

            /* Only use GPS strings if GPS was ready; otherwise keep "--" */
            if (gps_ok) {
                strncpy(lat, shared_gps_lat, sizeof(lat) - 1);
                strncpy(lon, shared_gps_lon, sizeof(lon) - 1);
                strncpy(ns,  shared_gps_ns,  sizeof(ns)  - 1);
                strncpy(ew,  shared_gps_ew,  sizeof(ew)  - 1);
            }

            xSemaphoreGive(sensor_data_mutex);
        }

        /* ---- Clear ready flags ----------------------------------------- */
        if (ready_mutex &&
            xSemaphoreTake(ready_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            ready_temp     = 0;
            ready_loadcell = 0;
            ready_gps      = 0;
            xSemaphoreGive(ready_mutex);
        }

        /* ---- Upload ------------------------------------------------------ */
        send_to_firebase(temp, lat, ns, lon, ew, cells);

        /* ---- Release sensors for next cycle ----------------------------- */
        if (sem_temp_go)     xSemaphoreGive(sem_temp_go);
        if (sem_loadcell_go) xSemaphoreGive(sem_loadcell_go);
        if (sem_gps_go)      xSemaphoreGive(sem_gps_go);

        /* Small gap before polling for the next cycle */
        vTaskDelay(pdMS_TO_TICKS(UPLOAD_INTERVAL_MS));
    }
}
