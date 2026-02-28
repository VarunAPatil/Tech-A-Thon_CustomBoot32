#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "shared_data.h"

// Fan PWM Settings
#define PWM_GPIO        7
#define PWM_FREQ        25000
#define PWM_RESOLUTION  LEDC_TIMER_8_BIT
#define PWM_CHANNEL     LEDC_CHANNEL_0
#define PWM_TIMER       LEDC_TIMER_0
#define PWM_MODE        LEDC_LOW_SPEED_MODE

// Fan auto-speed curve
// Below FAN_TEMP_MIN_C → FAN_MIN_PCT %;  Above FAN_TEMP_MAX_C → 100 %
// Linearly interpolated in between.
#define FAN_TEMP_MIN_C  15    // °C at which fan runs at minimum speed
#define FAN_TEMP_MAX_C  20    // °C at which fan runs at full speed
#define FAN_MIN_PCT     10    // % minimum fan speed (keeps bearing lubricated)

static void fan_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = PWM_MODE,
        .timer_num       = PWM_TIMER,
        .duty_resolution = PWM_RESOLUTION,
        .freq_hz         = PWM_FREQ,
        .clk_cfg         = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t channel = {
        .gpio_num   = PWM_GPIO,
        .speed_mode = PWM_MODE,
        .channel    = PWM_CHANNEL,
        .timer_sel  = PWM_TIMER,
        .duty       = 0,
        .hpoint     = 0
    };
    ledc_channel_config(&channel);
}

static void set_fan(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    uint32_t duty = (255 * percent) / 100;
    ledc_set_duty(PWM_MODE, PWM_CHANNEL, duty);
    ledc_update_duty(PWM_MODE, PWM_CHANNEL);
}

/* Returns fan speed (0-100 %) proportional to temperature.
 * Below FAN_TEMP_MIN_C → FAN_MIN_PCT %
 * Above FAN_TEMP_MAX_C → 100 %
 * Linearly interpolated between the two bounds. */
static int compute_fan_percent(int temp_x100)
{
    int temp_min_x100 = FAN_TEMP_MIN_C * 100;
    int temp_max_x100 = FAN_TEMP_MAX_C * 100;

    if (temp_x100 <= temp_min_x100) return FAN_MIN_PCT;
    if (temp_x100 >= temp_max_x100) return 100;

    int pct = FAN_MIN_PCT +
              ((100 - FAN_MIN_PCT) * (temp_x100 - temp_min_x100)) /
              (temp_max_x100 - temp_min_x100);
    return pct;
}

void peltier_task(void *pvParameters)
{
    fan_init();
    set_fan(FAN_MIN_PCT); // safe default until first temp reading

    printf("\n--- Fan Auto-Speed Control ---\n");
    printf("Fan curve: %d%% @ %d C  ->  100%% @ %d C\n",
           FAN_MIN_PCT, FAN_TEMP_MIN_C, FAN_TEMP_MAX_C);

    while (1) {
        /* ---- Read the spike-filtered temp from shared_data ------------- */
        int temp_x100 = -1;
        if (sensor_data_mutex != NULL &&
            xSemaphoreTake(sensor_data_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            temp_x100 = shared_temp_x100;
            xSemaphoreGive(sensor_data_mutex);
        }

        /* ---- Guard: no usable value — keep fan at minimum ------------- */
        if (temp_x100 <= 0) {
            set_fan(FAN_MIN_PCT);
            printf("[FAN] No valid temp — fan -> %d%%\n", FAN_MIN_PCT);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        /* ---- Set fan speed based on temperature ----------------------- */
        int fan_pct = compute_fan_percent(temp_x100);
        set_fan(fan_pct);
        printf("[FAN] Temp: %d.%02d C  ->  %d%%\n",
               temp_x100 / 100, temp_x100 % 100, fan_pct);

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}