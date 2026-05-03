/*
 * ota_task.c – WiFi-based OTA update for ESP32-S3
 * --------------------------------------------------
 * Priority model:
 *   Idle/standby : OTA_TASK_PRIORITY_LOW  (1)  – below all sensor tasks
 *   Active update: OTA_TASK_PRIORITY_HIGH (6)  – above all sensor tasks
 *
 * How it works:
 *   1. Connects to the configured WiFi AP.
 *   2. Starts an HTTP server and serves a simple upload webpage.
 *   3. When the user POSTs a binary via the webpage the task temporarily
 *      raises its own priority to OTA_TASK_PRIORITY_HIGH, streams the
 *      firmware into the inactive OTA partition, validates it, then
 *      reboots into the new firmware.
 *   4. If validation fails the old firmware is retained automatically
 *      because esp_ota_end() returns an error and we do NOT call
 *      esp_ota_set_boot_partition().
 */

#include "ota_task.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"

static const char *TAG = "OTA";

/* ---- FreeRTOS event bits -------------------------------------------- */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAX_RETRIES   10

static EventGroupHandle_t s_wifi_event_group = NULL;
static int                s_retry_count       = 0;

/* Exposed task handle (declared extern in header) */
TaskHandle_t ota_task_handle = NULL;

/* =========================================================================
 * Embedded HTML upload page
 * =========================================================================*/
static const char *OTA_HTML_PAGE =
    "<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'>"
    "<title>ESP32-S3 OTA Update</title>"
    "<style>"
    "  body{font-family:sans-serif;display:flex;flex-direction:column;"
    "       align-items:center;justify-content:center;height:100vh;margin:0;"
    "       background:#1a1a2e;color:#eee;}"
    "  h1{color:#e94560;margin-bottom:20px;}"
    "  form{background:#16213e;padding:30px;border-radius:10px;"
    "       box-shadow:0 4px 20px rgba(0,0,0,0.5);}"
    "  input[type=file]{margin:10px 0;color:#eee;}"
    "  input[type=submit]{"
    "    background:#e94560;color:#fff;border:none;padding:10px 24px;"
    "    border-radius:6px;cursor:pointer;font-size:1rem;margin-top:10px;}"
    "  input[type=submit]:hover{background:#c73652;}"
    "  #status{margin-top:16px;font-size:0.9rem;}"
    "</style></head><body>"
    "<h1>Firmware Update</h1>"
    "<form id='form' action='/update' method='POST' enctype='multipart/form-data'>"
    "  <label>Select .bin firmware file:</label><br>"
    "  <input type='file' name='firmware' accept='.bin' required><br>"
    "  <input type='submit' value='Upload &amp; Flash'>"
    "</form>"
    "<p id='status'></p>"
    "<script>"
    "  document.getElementById('form').addEventListener('submit', function(e){"
    "    document.getElementById('status').innerText = 'Uploading… do not power off.';"
    "  });"
    "</script>"
    "</body></html>";

/* =========================================================================
 * HTTP handlers
 * =========================================================================*/

/* GET / → serve the upload page */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, OTA_HTML_PAGE, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* POST /update → receive firmware binary and flash it */
static esp_err_t update_post_handler(httpd_req_t *req)
{
    /* ---- Priority boost: OTA is now the most important task ---- */
    vTaskPrioritySet(NULL, OTA_TASK_PRIORITY_HIGH);
    ESP_LOGI(TAG, "OTA update started – priority raised to %d",
             OTA_TASK_PRIORITY_HIGH);

    esp_ota_handle_t      ota_handle = 0;
    const esp_partition_t *update_partition =
        esp_ota_get_next_update_partition(NULL);

    if (update_partition == NULL) {
        ESP_LOGE(TAG, "No OTA partition found");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "No OTA partition");
        goto done;
    }

    ESP_LOGI(TAG, "Writing to partition: %s at offset 0x%" PRIx32,
             update_partition->label, update_partition->address);

    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES,
                                  &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "OTA begin failed");
        goto done;
    }

    /* ---- Stream the multipart body into the OTA partition ---- */
    char   buf[16384]; /* 16 KB buffer – max practical on ESP32-S3; flash write speed is bottleneck beyond this */
    int    total_size = req->content_len;
    int    remaining  = total_size;
    bool   header_skipped = false;
    int    received = 0;
    int    chunk_num = 0;
    ESP_LOGI(TAG, "Firmware size: %d bytes (%.1f KB)", total_size, total_size / 1024.0f);

    while (remaining > 0) {
        int to_read = (remaining < (int)sizeof(buf)) ? remaining
                                                     : (int)sizeof(buf);
        int ret = httpd_req_recv(req, buf, to_read);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue; /* retry on timeout */
            }
            ESP_LOGE(TAG, "Connection closed during OTA");
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "Connection lost");
            goto done;
        }

        /*
         * Skip the multipart header on the very first chunk.
         * The header ends at the first double-CRLF (\r\n\r\n).
         */
        const char *data   = buf;
        int         length = ret;

        if (!header_skipped) {
            const char *body_start = strstr(buf, "\r\n\r\n");
            if (body_start) {
                body_start += 4; /* skip past \r\n\r\n */
                length  -= (int)(body_start - buf);
                data     = body_start;
                header_skipped = true;
            } else {
                remaining -= ret;
                continue; /* still inside the header */
            }
        }

        if (length > 0) {
            err = esp_ota_write(ota_handle, data, length);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write failed: %s",
                         esp_err_to_name(err));
                esp_ota_abort(ota_handle);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                    "Write failed");
                goto done;
            }
            received += length;
            chunk_num++;
            int progress = (total_size > 0)
                           ? (int)((received * 100LL) / total_size)
                           : 0;
            ESP_LOGI(TAG, "Chunk #%3d | %5d B | total %6d B | %3d%%",
                     chunk_num, length, received, progress);
        }
        remaining -= ret;
    }

    ESP_LOGI(TAG, "Received %d bytes of firmware", received);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Firmware validation failed");
        goto done;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s",
                 esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Boot partition set failed");
        goto done;
    }

    const char *success_msg =
        "<html><body style='background:#1a1a2e;color:#eee;"
        "font-family:sans-serif;text-align:center;padding-top:15vh;'>"
        "<h1 style='color:#4caf50;'>Update Successful!</h1>"
        "<p>Device will reboot in 3 seconds…</p>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, success_msg, HTTPD_RESP_USE_STRLEN);

    ESP_LOGI(TAG, "OTA success – rebooting in 3 s");
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();

done:
    /* Restore low priority once the handler exits (error path) */
    vTaskPrioritySet(NULL, OTA_TASK_PRIORITY_LOW);
    return ESP_OK;
}

/* =========================================================================
 * HTTP server start / stop helpers
 * =========================================================================*/
static httpd_handle_t s_httpd = NULL;

static void start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port    = OTA_HTTP_PORT;
    config.stack_size     = 32768;  /* 32 KB stack to safely hold 16 KB recv buf on ESP32-S3 */

    if (httpd_start(&s_httpd, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    static const httpd_uri_t root = {
        .uri     = "/",
        .method  = HTTP_GET,
        .handler = root_get_handler,
    };
    static const httpd_uri_t update = {
        .uri     = "/update",
        .method  = HTTP_POST,
        .handler = update_post_handler,
    };

    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &update);

    ESP_LOGI(TAG, "HTTP server started on port %d", OTA_HTTP_PORT);
}

/* =========================================================================
 * WiFi event handler
 * =========================================================================*/
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < WIFI_MAX_RETRIES) {
            s_retry_count++;
            ESP_LOGW(TAG, "WiFi disconnected – retry %d/%d",
                     s_retry_count, WIFI_MAX_RETRIES);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* =========================================================================
 * WiFi initialisation (STA mode)
 * =========================================================================*/
static bool wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t wifi_instance;
    esp_event_handler_instance_t ip_instance;

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL,
        &wifi_instance));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL,
        &ip_instance));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid     = OTA_WIFI_SSID,
            .password = OTA_WIFI_PASSWORD,
            /* Require PMF if the AP supports it */
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable  = true,
                .required = false,
            },
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Block until connected or max retries exceeded */
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        portMAX_DELAY);

    /* Clean up handlers – we only need the IP once */
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                          wifi_instance);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                          ip_instance);
    vEventGroupDelete(s_wifi_event_group);
    s_wifi_event_group = NULL;

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to WiFi SSID: %s", OTA_WIFI_SSID);
        return true;
    }

    ESP_LOGE(TAG, "Failed to connect to WiFi after %d attempts",
             WIFI_MAX_RETRIES);
    return false;
}

/* =========================================================================
 * OTA FreeRTOS Task
 * =========================================================================*/
void ota_task(void *pvParameters)
{
    ESP_LOGI(TAG, "OTA task started (priority=%d)", OTA_TASK_PRIORITY_LOW);

    /* NVS must be initialised for WiFi to work */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    /* Connect to WiFi */
    if (!wifi_init_sta()) {
        ESP_LOGE(TAG, "WiFi connection failed – OTA task exiting");
        vTaskDelete(NULL);
        return;
    }

    /* Print OTA URL for the user */
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        ESP_LOGI(TAG, "OTA page: http://" IPSTR ":%d",
                 IP2STR(&ip_info.ip), OTA_HTTP_PORT);
    }

    /* Start the HTTP server – stays up permanently */
    start_http_server();

    /*
     * The task parks itself here at low priority.
     * The actual CPU work happens inside the HTTP handler callbacks
     * when the user triggers an upload (at which point priority is
     * temporarily raised inside update_post_handler).
     */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000)); /* sleep 10 s, minimal CPU usage */
    }
}
