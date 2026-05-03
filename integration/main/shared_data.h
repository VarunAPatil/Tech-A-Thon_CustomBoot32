#ifndef SHARED_DATA_H
#define SHARED_DATA_H

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* ---- Mutex protecting the shared sensor variables ----------------------- */
extern SemaphoreHandle_t sensor_data_mutex;

/* ---- Per-sensor "go" semaphores -----------------------------------------
 * Flow per cycle:
 *   1. Sensor takes its sem (initially given → runs once immediately).
 *   2. Sensor reads hardware, writes shared_data, then sets its ready bit.
 *   3. firebase_task waits for all 3 ready bits, snapshots, uploads.
 *   4. firebase_task gives all 3 semaphores → sensors unblock and repeat.
 *
 * This guarantees:
 *   - Only ONE set of readings is in-flight at a time.
 *   - firebase_task always uploads a consistent, same-cycle snapshot.
 *   - Sensors don't start the next reading until the previous one is sent.
 * -------------------------------------------------------------------------*/
extern SemaphoreHandle_t sem_temp_go;       // given by firebase after upload
extern SemaphoreHandle_t sem_loadcell_go;   // given by firebase after upload
extern SemaphoreHandle_t sem_gps_go;        // given by firebase after upload

/* ---- Ready flags (written by sensors, read+cleared by firebase_task) ---- */
extern volatile int ready_temp;
extern volatile int ready_loadcell;
extern volatile int ready_gps;

/* ---- Mutex protecting the ready flags ----------------------------------- */
extern SemaphoreHandle_t ready_mutex;

/* ---- Shared sensor variables -------------------------------------------- */
extern int shared_temp_x100;   // temperature × 100  (e.g. 2344 = 23.44 °C)
extern int shared_units;

/* ---- Shared GPS variables ----------------------------------------------- */
extern char shared_gps_lat[20];
extern char shared_gps_lon[20];
extern char shared_gps_ns[4];
extern char shared_gps_ew[4];
extern int  shared_gps_sats;   // number of satellites in view (0 = no fix)

#endif // SHARED_DATA_H
