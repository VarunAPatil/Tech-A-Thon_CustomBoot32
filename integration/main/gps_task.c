#include <stdio.h>
#include <string.h>
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

static void parse_gga(char *line)
{
    char time[16] = {0};
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
            case 2: strncpy(time, token, sizeof(time)-1); break;
            case 3: strncpy(lat,  token, sizeof(lat)-1);  break;
            case 4: strncpy(ns,   token, sizeof(ns)-1);   break;
            case 5: strncpy(lon,  token, sizeof(lon)-1);  break;
            case 6: strncpy(ew,   token, sizeof(ew)-1);   break;
            case 7: strncpy(fix,  token, sizeof(fix)-1);  break;
            case 8: strncpy(sats, token, sizeof(sats)-1); break;
        }
    }

    /* If no fix */
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
        if (time[0])
            strncpy(shared_gps_time, time, 15);

        strncpy(shared_gps_lat, lat, 31);
        strncpy(shared_gps_ns,  ns,  3);
        strncpy(shared_gps_lon, lon, 31);
        strncpy(shared_gps_ew,  ew,  3);

        xSemaphoreGive(sensor_data_mutex);
    }

    ESP_LOGI(TAG, "TIME:%s LAT:%s%s LON:%s%s FIX:%s SAT:%s",
             time, lat, ns, lon, ew,
             fix[0] ? fix : "0",
             sats[0] ? sats : "0");
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
    int idx = 0;

    while (1) {

        int len = uart_read_bytes(UART_PORT, data, BUF_SIZE,
                                  pdMS_TO_TICKS(100));

        for (int i = 0; i < len; i++) {
            char c = data[i];

            if (c == '\n') {
                line[idx] = '\0';
                idx = 0;

                /* Handle both GPGGA and GNGGA */
                if (strstr(line, "GGA")) {
                    parse_gga(line);
                }
            }
            else if (idx < sizeof(line) - 1) {
                line[idx++] = c;
            }
        }
    }
}