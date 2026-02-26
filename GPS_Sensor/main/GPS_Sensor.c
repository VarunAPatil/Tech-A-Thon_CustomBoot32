#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#define DT_PIN   4
#define SCK_PIN  5

static const char *TAG = "HX711";

/* ===== CHANGE THESE AFTER MEASUREMENT ===== */
#define OFFSET   -2760

static const float SCALE = 
    (-40128.0f - (-2760.0f)) / 49.975f;
/* ========================================= */
/* ========================================= */

int32_t hx711_read_raw()
{
    int32_t data = 0;

    // wait until HX711 ready (DT LOW)
    while (gpio_get_level(DT_PIN));

    for (int i = 0; i < 24; i++) {
        gpio_set_level(SCK_PIN, 1);
        esp_rom_delay_us(1);

        data = (data << 1) | gpio_get_level(DT_PIN);

        gpio_set_level(SCK_PIN, 0);
        esp_rom_delay_us(1);
    }

    // 25th clock pulse (gain = 128)
    gpio_set_level(SCK_PIN, 1);
    esp_rom_delay_us(1);
    gpio_set_level(SCK_PIN, 0);

    // sign extend 24-bit value
    if (data & 0x800000)
        data |= 0xFF000000;

    return data;
}

int32_t hx711_read_avg(int samples)
{
    int64_t sum = 0;
    for (int i = 0; i < samples; i++) {
        sum += hx711_read_raw();
    }
    return sum / samples;
}

float hx711_get_weight()
{
    int32_t raw = hx711_read_avg(10);   // smooth output
    // ESP_LOGI("RAW", "%ld", raw);
    return (raw - OFFSET) / SCALE;
}

void app_main(void)
{
    gpio_config_t io = {0};

    io.pin_bit_mask = (1ULL << SCK_PIN);
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);

    io.pin_bit_mask = (1ULL << DT_PIN);
    io.mode = GPIO_MODE_INPUT;
    gpio_config(&io);

    ESP_LOGI(TAG, "HX711 calibrated scale started");

    while (1) {
        float weight = hx711_get_weight();
        ESP_LOGI("WEIGHT", "%.2f grams", weight);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}