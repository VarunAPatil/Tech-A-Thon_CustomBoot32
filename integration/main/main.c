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

/* ---- Shared sensor data ------------------------------------------------ */
SemaphoreHandle_t sensor_data_mutex;

int  shared_temp_x100 = 0;
int  shared_units    = 0;
char shared_gps_lat[20]  = "----";
char shared_gps_lon[20]  = "----";
char shared_gps_ns[4]    = "";
char shared_gps_ew[4]    = "";
int  shared_gps_sats     = 0;

/* ---- Per-sensor "go" semaphores ----------------------------------------
 * Initially given (count=1) so each sensor task can run its FIRST reading
 * immediately.  After that, firebase_task gives them after every upload.
 * -----------------------------------------------------------------------*/
SemaphoreHandle_t sem_temp_go;
SemaphoreHandle_t sem_loadcell_go;
SemaphoreHandle_t sem_gps_go;

/* ---- Ready flags (set by sensor tasks, cleared by firebase_task) ------- */
volatile int ready_temp     = 0;
volatile int ready_loadcell = 0;
volatile int ready_gps      = 0;

SemaphoreHandle_t ready_mutex;

void app_main(void)
{
    printf("Starting Dual-Core Integration Project...\n");

    /* --- Create shared data mutex --------------------------------------- */
    sensor_data_mutex = xSemaphoreCreateMutex();
    if (sensor_data_mutex == NULL) {
        printf("Failed to create sensor_data_mutex!\n");
        return;
    }

    /* --- Create ready-flag mutex ---------------------------------------- */
    ready_mutex = xSemaphoreCreateMutex();
    if (ready_mutex == NULL) {
        printf("Failed to create ready_mutex!\n");
        return;
    }

    /* --- Create per-sensor go-semaphores (binary, start GIVEN = 1) ------ */
    sem_temp_go     = xSemaphoreCreateBinary();
    sem_loadcell_go = xSemaphoreCreateBinary();
    sem_gps_go      = xSemaphoreCreateBinary();

    if (!sem_temp_go || !sem_loadcell_go || !sem_gps_go) {
        printf("Failed to create go semaphores!\n");
        return;
    }

    /* Pre-give once so sensors can run their very first reading immediately */
    xSemaphoreGive(sem_temp_go);
    xSemaphoreGive(sem_loadcell_go);
    xSemaphoreGive(sem_gps_go);

    /* --- Start tasks ---------------------------------------------------- */

    // Peltier Task on Core 0, Priority 2
    xTaskCreatePinnedToCore(peltier_task, "peltier_task", 4096, NULL, 2, NULL, 0);

    // Load Cell Task on Core 1, Priority 5
    xTaskCreatePinnedToCore(load_cell_task, "load_cell_task", 4096, NULL, 5, NULL, 1);

    // Temperature Task on Core 0, Priority 4
    xTaskCreatePinnedToCore(temp_task, "temp_task", 4096, NULL, 4, NULL, 0);

    // GPS Task on Core 1, Priority 4
    xTaskCreatePinnedToCore(gps_task, "gps_task", 4096, NULL, 4, NULL, 1);

    // LCD Task on Core 1, Priority 3 (larger stack for LVGL)
    xTaskCreatePinnedToCore(lcd_task, "lcd_task", 8192, NULL, 3, NULL, 1);

    // OTA Task on Core 0 at lowest priority (raises itself when flashing)
    xTaskCreatePinnedToCore(ota_task, "ota_task", OTA_TASK_STACK_SIZE,
                            NULL, OTA_TASK_PRIORITY_LOW,
                            &ota_task_handle, 0);

    // Firebase Task on Core 0, Priority 2
    // Waits until all three sensors signal ready, then snapshots + uploads,
    // then gives all go-semaphores so sensors start their next reading.
    xTaskCreatePinnedToCore(firebase_task, "firebase_task", 6144,
                            NULL, 2, NULL, 0);
}
