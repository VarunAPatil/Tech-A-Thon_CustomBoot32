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

/* DS18B20 known-bad output values:
 *   =  0.00 C  →  0x0000  power / comms fault
 *   = 85.00 C  →  0x0550  power-on reset (POR) value
 *   <=-50.00 C →  sensor not responding (ds18b20_read_temp returns -1000)
 */
static int temp_is_plausible(float t)
{
    if (t <= 0.0f)  return 0;   // power/comms fault
    if (t >= 85.0f) return 0;   // DS18B20 POR value
    return 1;
}

/* Max allowed UPWARD jump between readings (5.00 °C).
 * Downward jumps (cooling) are always accepted — no limit.
 * This catches spikes like 21°C → 29°C that would falsely trigger peltier ON. */
#define SPIKE_UP_LIMIT_X100  500

void temp_task(void *pvParameters)
{
    temp_init();

    int prev_valid_x100 = 0; // 0 = no reading yet

    while (1) {
        /* ---- Wait for firebase_task to release us ---------------------- */
        if (sem_temp_go != NULL) {
            xSemaphoreTake(sem_temp_go, portMAX_DELAY);
        }

        /* ---- Take a fresh temperature reading (~750 ms) --------------- */
        float temp = ds18b20_read_temp();

        /* ---- Stage 1: absolute plausibility filter --------------------- */
        int use_temp_x100;
        if (!temp_is_plausible(temp)) {
            use_temp_x100 = prev_valid_x100;
            printf("[TEMP] Bad reading (%.2f C) — keeping prev: %d.%02d C\n",
                   temp, use_temp_x100 / 100, use_temp_x100 % 100);

        /* ---- Stage 2: directional spike filter ------------------------- */
        // Reject sudden UPWARD jumps (> 5°C) — those are sensor glitches.
        // Downward drops of any size are legitimate (active peltier cooling).
        } else if (prev_valid_x100 > 0 &&
                   (int)(temp * 100) - prev_valid_x100 >= SPIKE_UP_LIMIT_X100) {
            use_temp_x100 = prev_valid_x100;
            printf("[TEMP] Spike UP rejected! %.2f C vs prev %d.%02d C — keeping prev\n",
                   temp, prev_valid_x100 / 100, prev_valid_x100 % 100);

        /* ---- Stage 3: valid reading ------------------------------------ */
        } else {
            use_temp_x100  = (int)(temp * 100);
            prev_valid_x100 = use_temp_x100;
            printf("[TEMP] %.2f C\n", temp);
        }

        /* ---- Write to shared_data ------------------------------------- */
        if (sensor_data_mutex != NULL &&
            xSemaphoreTake(sensor_data_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {

            shared_temp_x100 = use_temp_x100;
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
