#ifndef FIREBASE_TASK_H
#define FIREBASE_TASK_H

/* -------------------------------------------------------------------------
 * firebase_task – periodic upload of sensor data to Firebase RTDB
 *
 * Reads shared_temp_x10, shared_units (cells), shared_gps_lat/lon/ns/ew
 * every FIREBASE_UPLOAD_INTERVAL_MS and PUTs a JSON bundle via HTTPS.
 *
 * Requires WiFi to already be connected (OTA task handles connection).
 * Priority: 2 (lowest worker tier, same as peltier_task).
 * -------------------------------------------------------------------------*/

#define FIREBASE_UPLOAD_INTERVAL_MS  5000   /* upload every 5 seconds */

void firebase_task(void *pvParameters);

#endif /* FIREBASE_TASK_H */
