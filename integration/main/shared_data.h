#ifndef SHARED_DATA_H
#define SHARED_DATA_H

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Mutex to protect access to the shared sensor variables
extern SemaphoreHandle_t sensor_data_mutex;

// Shared sensor variables
extern int shared_temp_x10;
extern int shared_units;

// Shared GPS variables
extern char shared_gps_time[16];
extern char shared_gps_lat[20];
extern char shared_gps_lon[20];
extern char shared_gps_ns[4];
extern char shared_gps_ew[4];

#endif // SHARED_DATA_H
