#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "shared_data.h"
#include "gps_task.h"

#define UART_PORT UART_NUM_2
#define TXD_PIN   17
#define RXD_PIN   18
#define BUF_SIZE  1024

static const char *TAG = "GPS";

/* -------------------- GGA PARSER -------------------- */

/* Returns 1 if a GGA sentence was successfully parsed and shared_data was
 * updated, 0 otherwise (no fix / parse error). */
static int parse_gga(char *line)
{
    char lat[32]  = {0};
    char ns[4]    = {0};
    char lon[32]  = {0};
    char ew[4]    = {0};
    char fix[4]   = {0};
    char sats[4]  = {0};

    int field = 0;
    char *token;
    char *rest = line;

    while ((token = strtok_r(rest, ",", &rest))) {
        field++;

        switch (field) {
            case 3: strncpy(lat,  token, sizeof(lat)-1);  break;
            case 4: strncpy(ns,   token, sizeof(ns)-1);   break;
            case 5: strncpy(lon,  token, sizeof(lon)-1);  break;
            case 6: strncpy(ew,   token, sizeof(ew)-1);   break;
            case 7: strncpy(fix,  token, sizeof(fix)-1);  break;
            case 8: strncpy(sats, token, sizeof(sats)-1); break;
        }
    }

    /* If no fix, fill placeholders */
    if (fix[0] == '0' || fix[0] == '\0') {
        strcpy(lat, "NO SATT");
        strcpy(lon, "NO SATT");
        ns[0] = '\0';
        ew[0] = '\0';
    }

    /* Update shared data safely */
    if (sensor_data_mutex &&
        xSemaphoreTake(sensor_data_mutex, pdMS_TO_TICKS(20)) == pdTRUE)
    {
        strncpy(shared_gps_lat, lat, sizeof(shared_gps_lat) - 1);
        strncpy(shared_gps_ns,  ns,  sizeof(shared_gps_ns)  - 1);
        strncpy(shared_gps_lon, lon, sizeof(shared_gps_lon) - 1);
        strncpy(shared_gps_ew,  ew,  sizeof(shared_gps_ew)  - 1);
        shared_gps_sats = sats[0] ? atoi(sats) : 0;

        xSemaphoreGive(sensor_data_mutex);
    }

    ESP_LOGI(TAG, "LAT:%s%s LON:%s%s FIX:%s SAT:%d",
             lat, ns, lon, ew,
             fix[0]  ? fix  : "0",
             sats[0] ? atoi(sats) : 0);

    return 1; // sentence was processed
}

/* -------------------- GPS TASK -------------------- */

void gps_task(void *pvParameters)
{
    uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };

    uart_driver_install(UART_PORT, BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(UART_PORT, &uart_config);
    uart_set_pin(UART_PORT, TXD_PIN, RXD_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    ESP_LOGI(TAG, "GPS UART Initialized");

    uint8_t data[BUF_SIZE];
    char line[256];
    int  idx = 0;

    while (1) {
        /* ---- Wait for firebase_task to release us for the next cycle --- */
        // Pre-given in main for first iteration; firebase gives it after upload.
        if (sem_gps_go != NULL) {
            xSemaphoreTake(sem_gps_go, portMAX_DELAY);
        }

        /* ---- Read UART until we get one valid GGA sentence ------------- */
        int got_gga = 0;
        idx = 0;

        while (!got_gga) {
            int len = uart_read_bytes(UART_PORT, data, BUF_SIZE,
                                      pdMS_TO_TICKS(200));

            for (int i = 0; i < len && !got_gga; i++) {
                char c = data[i];

                if (c == '\n') {
                    line[idx] = '\0';
                    idx = 0;

                    if (strstr(line, "GGA")) {
                        parse_gga(line);
                        got_gga = 1; // stop after first GGA — one per firebase cycle
                    }
                } else if (idx < (int)sizeof(line) - 1) {
                    line[idx++] = c;
                }
            }
        }

        /* ---- Signal firebase_task: GPS reading is ready ---------------- */
        if (ready_mutex != NULL &&
            xSemaphoreTake(ready_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {

            ready_gps = 1;
            xSemaphoreGive(ready_mutex);
        }
    }
}