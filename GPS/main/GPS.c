#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"

#define UART_NUM UART_NUM_2
#define TXD_PIN 17
#define RXD_PIN 18
#define BUF_SIZE 1024

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

    printf("TIME:%s  LAT:%s%s  LON:%s%s  FIX:%s  SATS:%s\n",
           time[0]?time:"--",
           lat[0]?lat:"----", ns,
           lon[0]?lon:"----", ew,
           fix[0]?fix:"0",
           sats[0]?sats:"0");
}

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

                if (strstr(line, "$GPGGA")) {
                    parse_gga(line);
                }
            } 
            else if (idx < sizeof(line) - 1) {
                line[idx++] = c;
            }
        }
    }
}

void app_main(void)
{
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