#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "load_cell.h"
#include "temp.h"
#include "peltier.h"
#include "lcd_task.h"
#include "gps_task.h"
#include "shared_data.h"
#include "ota_task.h"
#include "firebase_task.h"

SemaphoreHandle_t sensor_data_mutex;
int shared_temp_x10 = 0;
int shared_units = 0;
char shared_gps_time[16] = "--";
char shared_gps_lat[20] = "----";
char shared_gps_lon[20] = "----";
char shared_gps_ns[4] = "";
char shared_gps_ew[4] = "";

void app_main(void)
{
    printf("Starting Dual-Core Integration Project...\n");

    // Initialize shared data mutex
    sensor_data_mutex = xSemaphoreCreateMutex();
    if (sensor_data_mutex == NULL) {
        printf("Failed to create sensor data mutex!\n");
        return;
    }

    // Start Peltier Task on Core 0 (PRO_CPU) with Low Priority (2)
    xTaskCreatePinnedToCore(peltier_task, "peltier_task", 4096, NULL, 2, NULL, 0);

    // Start Load Cell Task on Core 1 (APP_CPU) with High Priority (5)
    xTaskCreatePinnedToCore(load_cell_task, "load_cell_task", 4096, NULL, 5, NULL, 1);

    // Start Temperature Task on Core 0 (PRO_CPU) with High Priority (4)
    xTaskCreatePinnedToCore(temp_task, "temp_task", 4096, NULL, 4, NULL, 0);

    // Start GPS Task on Core 1
    xTaskCreatePinnedToCore(gps_task, "gps_task", 4096, NULL, 4, NULL, 1);

    // Start LCD Task on Core 1 (APP_CPU) with Normal Priority (3)
    // Allocating a larger stack for LVGL (e.g., 8192)
    xTaskCreatePinnedToCore(lcd_task, "lcd_task", 8192, NULL, 3, NULL, 1);

    // Start OTA Task on Core 0 at the LOWEST priority (1).
    // It idles on the webserver; runtime raises to priority 6 when
    // an update is actually being flashed (beats all sensor tasks).
    xTaskCreatePinnedToCore(ota_task, "ota_task", OTA_TASK_STACK_SIZE,
                            NULL, OTA_TASK_PRIORITY_LOW,
                            &ota_task_handle, 0);

    // Start Firebase Task on Core 0 at priority 2.
    // Snapshots shared sensor data every 5 s and PUTs to Firebase RTDB.
    xTaskCreatePinnedToCore(firebase_task, "firebase_task", 6144,
                            NULL, 2, NULL, 0);
}
