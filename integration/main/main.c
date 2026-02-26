#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "load_cell.h"
#include "temp.h"

void app_main(void)
{
    printf("Starting Integration Project (Load Cell + Temp)...\n");

    // Start Load Cell Task (higher priority than temp for more strict timing, or equal)
    // Allocating 4096 bytes for stack
    xTaskCreate(load_cell_task, "load_cell_task", 4096, NULL, 5, NULL);

    // Start Temperature Sensor task
    xTaskCreate(temp_task, "temp_task", 4096, NULL, 4, NULL);

    // The main task will exit or idle, but the created tasks will continue running.
}
