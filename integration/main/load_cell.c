#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "math.h"
#include "load_cell.h"

#define DT_PIN   4
#define SCK_PIN  5
#define CELL_WEIGHT 35.0f
#define MAX_CELLS   50

static const char *TAG = "HX711";
static int last_cells = 0;
#define OFFSET   -2760

static const float SCALE = 
    (-40128.0f - (-2760.0f)) / 49.975f;

void load_cell_init(void)
{
    gpio_config_t io = {0};

    io.pin_bit_mask = (1ULL << SCK_PIN);
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);

    io.pin_bit_mask = (1ULL << DT_PIN);
    io.mode = GPIO_MODE_INPUT;
    gpio_config(&io);

    ESP_LOGI(TAG, "HX711 calibrated scale started");
}

static int32_t hx711_read_raw()
{
    int32_t data = 0;

    // Wait until HX711 ready (DT LOW)
    // Using a timeout to prevent absolute lockup
    int timeout = 100;
    while (gpio_get_level(DT_PIN) && timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(1)); // Yield instead of busy-looping completely
        timeout--;
    }

    if (timeout <= 0) {
       // ESP_LOGE(TAG, "HX711 Timeout waiting for DT LOW");
       return 0; // Or some error indicator
    }

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

static int32_t hx711_read_avg(int samples)
{
    int64_t sum = 0;
    int valid_samples = 0;
    for (int i = 0; i < samples; i++) {
        int32_t val = hx711_read_raw();
        if(val != 0) { // Naive timeout check based on our implementation above
            sum += val;
            valid_samples++;
        }
    }
    if (valid_samples == 0) return 0;
    return sum / valid_samples;
}

static float hx711_get_weight()
{
    int32_t raw = hx711_read_avg(12);   // smooth output
    return (raw - OFFSET) / SCALE;
}

void load_cell_task(void *pvParameters)
{
    load_cell_init();

    while (1) {
        float weight = hx711_get_weight();

        float net = weight - 70.0f;
        if (net < 0)
            net = 0;

        int best_cells = last_cells;
        float smallest_error = 1e9;

        for (int i = 0; i <= MAX_CELLS; i++) {
            float expected = i * CELL_WEIGHT;
            float error = fabs(net - expected);

            if (error < smallest_error) {
                smallest_error = error;
                best_cells = i;
            }
        }

        /* Reject unrealistic jumps (noise protection) */
        if (best_cells > last_cells) {
            last_cells++;      // increase slowly
        }
        else if (best_cells < last_cells) {
            last_cells--;      // decrease slowly
        }

        /* Optional: reject if too far from any expected weight */
        if (smallest_error > 15.0f) {
            best_cells = last_cells;
        }

        last_cells = best_cells;
        ESP_LOGI(TAG, "Weight: %.2f", weight);
        ESP_LOGI(TAG, "Cells: %d", last_cells);

        // Proper FreeRTOS delay, yielding execution to other tasks
        vTaskDelay(pdMS_TO_TICKS(500)); 
    }
}
