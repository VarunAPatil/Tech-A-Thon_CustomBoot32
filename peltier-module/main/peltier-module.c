#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "driver/gpio.h"

// Fan PWM Settings
#define PWM_GPIO        7
#define PWM_FREQ        25000
#define PWM_RESOLUTION  LEDC_TIMER_8_BIT
#define PWM_CHANNEL     LEDC_CHANNEL_0
#define PWM_TIMER       LEDC_TIMER_0
#define PWM_MODE        LEDC_LOW_SPEED_MODE

// Peltier ON/OFF Settings
#define PELTIER_GPIO    15

void hardware_init()
{
    // --- Initialize Fan PWM ---
    ledc_timer_config_t timer = {
        .speed_mode = PWM_MODE,
        .timer_num = PWM_TIMER,
        .duty_resolution = PWM_RESOLUTION,
        .freq_hz = PWM_FREQ,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t channel = {
        .gpio_num = PWM_GPIO,
        .speed_mode = PWM_MODE,
        .channel = PWM_CHANNEL,
        .timer_sel = PWM_TIMER,
        .duty = 0,
        .hpoint = 0
    };
    ledc_channel_config(&channel);

    // --- Initialize Peltier GPIO ---
    gpio_reset_pin(PELTIER_GPIO);
    gpio_set_direction(PELTIER_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(PELTIER_GPIO, 0); // Start with Peltier OFF
}

void app_main(void)
{
    hardware_init();

    char input_buffer[16];
    int buffer_idx = 0;

    printf("\n--- Thermal Control Ready ---\n");
    printf("Enter 0-100: Set Fan Speed\n");
    printf("Enter 101:   Peltier ON\n");
    printf("Enter 102:   Peltier OFF\n");

    while (1)
    {
        int c = getchar();

        if (c != EOF) 
        {
            if (c == '\n' || c == '\r') 
            {
                if (buffer_idx > 0) 
                {
                    input_buffer[buffer_idx] = '\0';
                    int val = atoi(input_buffer);

                    if (val >= 0 && val <= 100) {
                        // Fan Control
                        uint32_t duty = (255 * val) / 100;
                        ledc_set_duty(PWM_MODE, PWM_CHANNEL, duty);
                        ledc_update_duty(PWM_MODE, PWM_CHANNEL);
                        printf("\n[FAN] Speed set to %d%%\n", val);
                    } 
                    else if (val == 101) {
                        // Peltier ON
                        gpio_set_level(PELTIER_GPIO, 1);
                        printf("\n[PELTIER] State: ON\n");
                    }
                    else if (val == 102) {
                        // Peltier OFF
                        gpio_set_level(PELTIER_GPIO, 0);
                        printf("\n[PELTIER] State: OFF\n");
                    }
                    else {
                        printf("\nInvalid command.\n");
                    }
                    buffer_idx = 0;
                }
            } 
            else if (c >= '0' && c <= '9' && buffer_idx < sizeof(input_buffer) - 1) 
            {
                input_buffer[buffer_idx++] = (char)c;
                putchar(c); 
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}