#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "temp.h"
#include "shared_data.h"

#define DS_PIN 6

static void ds_delay_us(int us)
{
    esp_rom_delay_us(us);
}

static void ds_write_bit(int bit)
{
    gpio_set_direction(DS_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(DS_PIN, 0);
    ds_delay_us(bit ? 6 : 60);
    gpio_set_level(DS_PIN, 1);
    ds_delay_us(bit ? 64 : 10);
}

static int ds_read_bit(void)
{
    int bit;
    gpio_set_direction(DS_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(DS_PIN, 0);
    ds_delay_us(6);

    gpio_set_direction(DS_PIN, GPIO_MODE_INPUT);
    ds_delay_us(9);
    bit = gpio_get_level(DS_PIN);
    ds_delay_us(55);
    return bit;
}

static void ds_write_byte(uint8_t byte)
{
    for (int i = 0; i < 8; i++) {
        ds_write_bit(byte & 0x01);
        byte >>= 1;
    }
}

static uint8_t ds_read_byte(void)
{
    uint8_t byte = 0;
    for (int i = 0; i < 8; i++) {
        byte >>= 1;
        if (ds_read_bit()) byte |= 0x80;
    }
    return byte;
}

static int ds_reset(void)
{
    gpio_set_direction(DS_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(DS_PIN, 0);
    ds_delay_us(480);

    gpio_set_direction(DS_PIN, GPIO_MODE_INPUT);
    ds_delay_us(70);

    int presence = !gpio_get_level(DS_PIN);
    ds_delay_us(410);
    return presence;
}

static float ds18b20_read_temp(void)
{
    if (!ds_reset()) return -1000;

    ds_write_byte(0xCC); // Skip ROM
    ds_write_byte(0x44); // Convert T

    // 12-bit conversion takes up to 750 ms
    vTaskDelay(pdMS_TO_TICKS(750));

    ds_reset();
    ds_write_byte(0xCC); // Skip ROM
    ds_write_byte(0xBE); // Read scratchpad

    uint8_t lsb = ds_read_byte();
    uint8_t msb = ds_read_byte();

    int16_t raw = (msb << 8) | lsb;
    return raw / 16.0;
}

void temp_init(void)
{
    gpio_set_pull_mode(DS_PIN, GPIO_PULLUP_ONLY);
}

void temp_task(void *pvParameters)
{
    temp_init();

    while (1) {
        /* ---- Wait for firebase_task to release us ---------------------- */
        // On first iteration the semaphore is pre-given in main, so we run
        // immediately. After that we block here until firebase signals done.
        if (sem_temp_go != NULL) {
            xSemaphoreTake(sem_temp_go, portMAX_DELAY);
        }

        /* ---- Take a fresh temperature reading (~750 ms) --------------- */
        float temp = ds18b20_read_temp();

        /* ---- Write to shared_data ------------------------------------- */
        if (sensor_data_mutex != NULL &&
            xSemaphoreTake(sensor_data_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {

            if (temp > -100) {
                shared_temp_x100 = (int)(temp * 100);
                printf("Temperature: %.2f C\n", temp);
            } else {
                printf("Temp Sensor not detected\n");
            }
            xSemaphoreGive(sensor_data_mutex);
        }

        /* ---- Signal firebase_task: temp reading is ready -------------- */
        if (ready_mutex != NULL &&
            xSemaphoreTake(ready_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {

            ready_temp = 1;
            xSemaphoreGive(ready_mutex);
        }
    }
}
