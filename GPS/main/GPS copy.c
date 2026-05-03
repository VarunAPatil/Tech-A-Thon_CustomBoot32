#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_server.h"

#define UART_NUM UART_NUM_2
#define TXD_PIN 17
#define RXD_PIN 16
#define BUF_SIZE 1024

/* ------------ GLOBAL GPS DATA ------------ */

static char g_time[20] = "--";
static char g_lat[32]  = "----";
static char g_lon[32]  = "----";
static char g_fix[8]   = "0";
static char g_sats[8]  = "0";

/* ------------ GPS PARSER ------------ */

static void parse_gga(char *line)
{
    char *token;
    int field = 0;

    char time[16] = "";
    char lat[20]  = "";
    char ns[4]   = "";
    char lon[20] = "";
    char ew[4]   = "";
    char fix[4]  = "";
    char sats[4] = "";

    token = strtok(line, ",");

    while (token) {
        field++;

        if (field == 2) strcpy(time, token);
        if (field == 3) strcpy(lat, token);
        if (field == 4) strcpy(ns, token);
        if (field == 5) strcpy(lon, token);
        if (field == 6) strcpy(ew, token);
        if (field == 7) strcpy(fix, token);
        if (field == 8) strcpy(sats, token);

        token = strtok(NULL, ",");
    }

    if (time[0]) strcpy(g_time, time);
    if (lat[0])  snprintf(g_lat, sizeof(g_lat), "%s%s", lat, ns);
    if (lon[0])  snprintf(g_lon, sizeof(g_lon), "%s%s", lon, ew);
    if (fix[0])  strcpy(g_fix, fix);
    if (sats[0]) strcpy(g_sats, sats);
}

/* ------------ GPS TASK ------------ */

void gps_task(void *arg)
{
    uint8_t data[BUF_SIZE];
    char line[256];
    int idx = 0;

    while (1) {
        int len = uart_read_bytes(UART_NUM, data, BUF_SIZE, pdMS_TO_TICKS(100));

        for (int i = 0; i < len; i++) {
            char c = data[i];

            if (c == '\n') {
                line[idx] = 0;
                idx = 0;

                if (strncmp(line, "$GPGGA", 6) == 0) {
                    parse_gga(line);
                }
            }
            else if (idx < sizeof(line) - 1) {
                line[idx++] = c;
            }
        }
    }
}

/* ------------ WEB HANDLERS ------------ */

static esp_err_t root_handler(httpd_req_t *req)
{
    const char *html =
        "<html><body>"
        "<h2>Live GPS</h2>"
        "<p>TIME: <span id='t'>--</span></p>"
        "<p>LAT: <span id='lat'>--</span></p>"
        "<p>LON: <span id='lon'>--</span></p>"
        "<p>FIX: <span id='fix'>--</span></p>"
        "<p>SATS: <span id='sats'>--</span></p>"
        "<script>"
        "setInterval(()=>{"
        "fetch('/data').then(r=>r.json()).then(d=>{"
        "t.innerText=d.time;"
        "lat.innerText=d.lat;"
        "lon.innerText=d.lon;"
        "fix.innerText=d.fix;"
        "sats.innerText=d.sats;"
        "});"
        "},1000);"
        "</script></body></html>";

    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t data_handler(httpd_req_t *req)
{
    char json[256];

    snprintf(json, sizeof(json),
             "{\"time\":\"%s\",\"lat\":\"%s\",\"lon\":\"%s\",\"fix\":\"%s\",\"sats\":\"%s\"}",
             g_time, g_lat, g_lon, g_fix, g_sats);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server;

    httpd_start(&server, &config);

    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler
    };

    httpd_uri_t data = {
        .uri = "/data",
        .method = HTTP_GET,
        .handler = data_handler
    };

    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &data);
}

/* ------------ WIFI ------------ */

void wifi_init(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "1234",
            .password = "123456789"
        }
    };

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
    esp_wifi_connect();
}

/* ------------ MAIN ------------ */

void app_main(void)
{
    nvs_flash_init();
    wifi_init();
    start_webserver();

    uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };

    uart_driver_install(UART_NUM, BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(UART_NUM, &uart_config);
    uart_set_pin(UART_NUM, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    xTaskCreate(gps_task, "gps_task", 4096, NULL, 5, NULL);
}