#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "sdkconfig.h" // To bring in Kconfig defines

#define BLINK_GPIO GPIO_NUM_2 // GPIO2 is often the built-in LED on many ESP32 dev boards
static const char *TAG = "led_blink_app";

// --- Firmware Version for this Update ---
// It's good practice to have a version for your update binaries.
// This is different from the FIRMWARE_VERSION in your main OTA host project.
#define LED_BLINK_FIRMWARE_VERSION "2.0.0_LED_Blink"

void app_main(void)
{
    ESP_LOGI(TAG, "Initializing LED Blink Application - Version: %s", LED_BLINK_FIRMWARE_VERSION);
    ESP_LOGI(TAG, "This firmware is intended for OTA update.");
    ESP_LOGI(TAG, "Targeting GPIO: %d", BLINK_GPIO);

    // Configure the GPIO
    gpio_reset_pin(BLINK_GPIO);
    /* Set the GPIO as a push/pull output */
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);

    ESP_LOGI(TAG, "GPIO configured. Starting blink loop.");

    while(1) {
        /* Blink off (output low) */
        ESP_LOGD(TAG, "Turning LED OFF");
        gpio_set_level(BLINK_GPIO, 0);
        vTaskDelay(1000 / portTICK_PERIOD_MS); // Delay for 1 second

        /* Blink on (output high) */
        ESP_LOGD(TAG, "Turning LED ON");
        gpio_set_level(BLINK_GPIO, 1);
        vTaskDelay(1000 / portTICK_PERIOD_MS); // Delay for 1 second
    }
}
