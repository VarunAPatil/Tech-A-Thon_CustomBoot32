#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "shared_data.h"

// Fan PWM Settings
#define PWM_GPIO        7
#define PWM_FREQ        25000
#define PWM_RESOLUTION  LEDC_TIMER_8_BIT
#define PWM_CHANNEL     LEDC_CHANNEL_0
#define PWM_TIMER       LEDC_TIMER_0
#define PWM_MODE        LEDC_LOW_SPEED_MODE

// Peltier ON/OFF Settings
#define PELTIER_GPIO    15

// Temperature thresholds (stored as temp × 100)
#define TEMP_ON_X100   2200   // Turn Peltier ON  above 22.00 °C
#define TEMP_OFF_X100  2000   // Turn Peltier OFF below 20.00 °C

static void hardware_init(void)
{
    // --- Fan PWM ---
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

    // --- Peltier GPIO ---
    gpio_reset_pin(PELTIER_GPIO);
    gpio_set_direction(PELTIER_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(PELTIER_GPIO, 0); // start OFF
}

static void set_fan(int percent)
{
    uint32_t duty = (255 * percent) / 100;
    ledc_set_duty(PWM_MODE, PWM_CHANNEL, duty);
    ledc_update_duty(PWM_MODE, PWM_CHANNEL);
}

void peltier_task(void *pvParameters)
{
    hardware_init();

    // Start fan at 30% idle speed
    set_fan(30);

    int peltier_state = 0; // 0 = OFF, 1 = ON

    printf("\n--- Thermal Control (AUTO) ---\n");
    printf("ON  threshold: %.2f C\n", TEMP_ON_X100  / 100.0f);
    printf("OFF threshold: %.2f C\n", TEMP_OFF_X100 / 100.0f);

    while (1) {
        // Read temperature from shared data
        int temp_x100 = 0;
        if (sensor_data_mutex != NULL &&
            xSemaphoreTake(sensor_data_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            temp_x100 = shared_temp_x100;
            xSemaphoreGive(sensor_data_mutex);
        }

        if (temp_x100 > TEMP_ON_X100 && peltier_state == 0) {
            // Too warm — turn Peltier ON, ramp fan to 100%
            gpio_set_level(PELTIER_GPIO, 1);
            peltier_state = 1;
            set_fan(100);
            printf("[PELTIER AUTO] ON  — Temp: %d.%02d C\n",
                   temp_x100 / 100, temp_x100 % 100);

        } else if (temp_x100 < TEMP_OFF_X100 && peltier_state == 1) {
            // Cool enough — turn Peltier OFF, reduce fan to 30%
            gpio_set_level(PELTIER_GPIO, 0);
            peltier_state = 0;
            set_fan(30);
            printf("[PELTIER AUTO] OFF — Temp: %d.%02d C\n",
                   temp_x100 / 100, temp_x100 % 100);
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}  