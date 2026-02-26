#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_rom_sys.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include <string.h>

#define FIREBASE_URL "https://vaxi-7f913-default-rtdb.firebaseio.com/device1/temp.json?auth=jtzovCmVoVHGQ54VtdONkTq0jLxJVpLgaiJTuD5d"

#define DS_PIN 6

static void ds_delay_us(int us)
{
    esp_rom_delay_us(us);
}

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

#define WIFI_SSID "1234"
#define WIFI_PASS "123456789"

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGI("WiFi", "Retrying connection...");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI("WiFi", "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

void wifi_init_sta(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
}

void send_temp_to_firebase(float temp)
{
    esp_http_client_config_t config = {
        .url = FIREBASE_URL,
        .method = HTTP_METHOD_PUT,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    char data[32];
    snprintf(data, sizeof(data), "%.2f", temp);

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, data, strlen(data));

    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        ESP_LOGI("Firebase", "Data sent successfully: %s", data);
    } else {
        ESP_LOGE("Firebase", "Failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}

float ds18b20_read_temp(void)
{
    if (!ds_reset()) return -1000;

    ds_write_byte(0xCC); // Skip ROM
    ds_write_byte(0x44); // Convert T

    vTaskDelay(pdMS_TO_TICKS(750));

    ds_reset();
    ds_write_byte(0xCC); // Skip ROM
    ds_write_byte(0xBE); // Read scratchpad

    uint8_t lsb = ds_read_byte();
    uint8_t msb = ds_read_byte();

    int16_t raw = (msb << 8) | lsb;
    return raw / 16.0;
}

void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize WiFi
    wifi_init_sta();

    // Give Wi-Fi a bit of time to connect before reading sensor layer loop
    vTaskDelay(pdMS_TO_TICKS(4000));

    gpio_set_pull_mode(DS_PIN, GPIO_PULLUP_ONLY);

    while (1) {
        float temp = ds18b20_read_temp();
        if (temp > -100) {
            printf("Temperature: %.2f °C\n", temp);
            send_temp_to_firebase(temp);
        } else {
            printf("Sensor not detected\n");
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
