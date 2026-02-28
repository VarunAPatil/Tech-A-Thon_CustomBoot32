/*
 * fan-low-test: Fan runs at ~10% PWM, DS18B20 temp is read and printed every second.
 *
 * Pinout:
 *   Fan PWM  → GPIO 7
 *   DS18B20  → GPIO 6 (with 4.7kΩ pull-up to 3.3V)
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

// Fan PWM Settings
#define PWM_GPIO        7
#define PWM_FREQ        25000
#define PWM_RESOLUTION  LEDC_TIMER_8_BIT
#define PWM_CHANNEL     LEDC_CHANNEL_0
#define PWM_TIMER       LEDC_TIMER_0
#define PWM_MODE        LEDC_LOW_SPEED_MODE

// DS18B20 Settings
#define DS_PIN          6

// Fan duty at 10%  (255 * 10 / 100 = 25)
#define FAN_PCT         10
#define FAN_DUTY        ((255 * FAN_PCT) / 100)

// ---- DS18B20 bit-bang driver ----------------------------------------

static void ds_delay_us(int us) { esp_rom_delay_us(us); }

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

/* Returns temperature in °C, or -1000.0 on sensor error. */
static float ds18b20_read_temp(void)
{
    if (!ds_reset()) return -1000.0f;
    ds_write_byte(0xCC); // Skip ROM
    ds_write_byte(0x44); // Start conversion
    vTaskDelay(pdMS_TO_TICKS(750)); // Wait for 12-bit conversion

    if (!ds_reset()) return -1000.0f;
    ds_write_byte(0xCC); // Skip ROM
    ds_write_byte(0xBE); // Read scratchpad

    uint8_t lsb = ds_read_byte();
    uint8_t msb = ds_read_byte();
    int16_t raw = (int16_t)((msb << 8) | lsb);
    return raw / 16.0f;
}

// ---- Hardware init -------------------------------------------------

static void hardware_init(void)
{
    // Fan PWM
    ledc_timer_config_t timer = {
        .speed_mode      = PWM_MODE,
        .timer_num       = PWM_TIMER,
        .duty_resolution = PWM_RESOLUTION,
        .freq_hz         = PWM_FREQ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t channel = {
        .gpio_num   = PWM_GPIO,
        .speed_mode = PWM_MODE,
        .channel    = PWM_CHANNEL,
        .timer_sel  = PWM_TIMER,
        .duty       = FAN_DUTY,
        .hpoint     = 0,
    };
    ledc_channel_config(&channel);

    // DS18B20 pull-up
    gpio_set_pull_mode(DS_PIN, GPIO_PULLUP_ONLY);
}

// ---- Main ----------------------------------------------------------

void app_main(void)
{
    hardware_init();

    printf("\n=== FAN LOW TEST (%d%% PWM) ===\n", FAN_PCT);
    printf("Fan GPIO : %d  |  Duty: %d/255 (%d%%)\n", PWM_GPIO, FAN_DUTY, FAN_PCT);
    printf("Temp GPIO: %d  (DS18B20)\n\n", DS_PIN);

    while (1) {
        float temp = ds18b20_read_temp();
        uint64_t elapsed_ms = esp_timer_get_time() / 1000;
        if (temp <= -999.0f) {
            printf("[LOG] t=%llu ms  TEMP=ERROR\n", elapsed_ms);
        } else {
            printf("[LOG] t=%llu ms  TEMP=%.2f C\n", elapsed_ms, temp);
        }
        // ds18b20_read_temp() already delays ~750ms
    }
}
