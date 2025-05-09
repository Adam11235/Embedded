#include <esp_wifi.h>
#include <errno.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include <esp_http_server.h>
#include <esp_ota_ops.h> // Keep OTA headers if needed later
#include <string.h>
#include <driver/gpio.h>
#include <esp_spiffs.h>
#include <cJSON.h> // Using cJSON for JSON handling

static const char *TAG = "receiver";

#define WIFI_SSID      "NadawcaOTA"
#define WIFI_PASSWORD  "password123"
#define LED_GPIO       GPIO_NUM_2 // LED GPIO

// Volatile global values updated by the sender
volatile bool global_led_state = false; // false for off, true for on
volatile char global_message[100] = "Initial message"; // Example size

// Struct definition (should match the sender)
typedef struct {
    bool toggle;
    char message[100];
} control_data_t;

static esp_err_t http_server_control_handler(httpd_req_t *req)
{
    char content[150]; // Buffer to hold the incoming JSON data
    // int received = 0; // Unused variable removed

    // Read the data sent by the client
    int ret = httpd_req_recv(req, content, sizeof(content) -1); // Ensure space for null terminator
    if (ret <= 0) {  // 0 for finished request, < 0 for error
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "httpd_req_recv timeout");
            httpd_resp_send_408(req); // Request timed out
        } else {
            ESP_LOGE(TAG, "httpd_req_recv error: %d", ret);
        }
        return ESP_FAIL;
    }

    // Null-terminate the received data
    content[ret] = '\0';
    ESP_LOGI(TAG, "Received data: %s", content);

    // Parse JSON
    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL) {
            ESP_LOGE(TAG, "Failed to parse JSON: %s", error_ptr);
        } else {
            ESP_LOGE(TAG, "Failed to parse JSON: Unknown error");
        }
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to parse JSON");
        return ESP_FAIL;
    }

    cJSON *toggle_json = cJSON_GetObjectItemCaseSensitive(root, "toggle");
    cJSON *message_json = cJSON_GetObjectItemCaseSensitive(root, "message");

    if (cJSON_IsBool(toggle_json) && message_json != NULL && cJSON_IsString(message_json) && (message_json->valuestring != NULL)) {
        // Update global variables
        global_led_state = cJSON_IsTrue(toggle_json);
        strncpy((char*)global_message, message_json->valuestring, sizeof(global_message) - 1);
        global_message[sizeof(global_message) - 1] = '\0'; // Ensure null termination

        ESP_LOGI(TAG, "Updated: toggle=%d, message='%s'", global_led_state, global_message);

        httpd_resp_sendstr(req, "Data received and processed");
    } else {
        ESP_LOGE(TAG, "Invalid JSON format or missing fields");
        if (!cJSON_IsBool(toggle_json)) ESP_LOGE(TAG, "'toggle' field is missing or not a boolean");
        if (message_json == NULL || !cJSON_IsString(message_json) || message_json->valuestring == NULL) ESP_LOGE(TAG, "'message' field is missing, not a string, or null");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON format");
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t http_server_message_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Message requested");

    // Send the current global message
    // Set content type to plain text
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, (const char*)global_message);

    return ESP_OK;
}


static const httpd_uri_t control_uri = {
    .uri       = "/control",
    .method    = HTTP_POST,
    .handler   = http_server_control_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t message_uri = {
    .uri       = "/message",
    .method    = HTTP_GET,
    .handler   = http_server_message_handler,
    .user_ctx  = NULL
};


static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true; // Enable LRU purge for old connections
    config.stack_size = 8192; // Increase stack size if handling complex requests or many connections

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        // Register URI handlers
        ESP_LOGI(TAG, "Registering URI handlers");
        esp_err_t ret_control = httpd_register_uri_handler(server, &control_uri);
        if (ret_control != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register /control handler: %s", esp_err_to_name(ret_control));
        }
        esp_err_t ret_message = httpd_register_uri_handler(server, &message_uri);
         if (ret_message != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register /message handler: %s", esp_err_to_name(ret_message));
        }
        return server;
    }

    ESP_LOGE(TAG, "Error starting server!");
    return NULL;
}

static void led_toggle_task(void *pvParameter)
{
    // Configure the GPIO
    gpio_reset_pin(LED_GPIO); // Reset to default state before configuration
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);

    while (1) {
        if (global_led_state) {
            gpio_set_level(LED_GPIO, 1); // Turn LED on
        } else {
            gpio_set_level(LED_GPIO, 0); // Turn LED off
        }
        vTaskDelay(pdMS_TO_TICKS(100)); // Check state and update LED (reduced delay for responsiveness)
    }
}

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WIFI_EVENT_STA_START: attempting to connect...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Wi-Fi disconnected, trying to reconnect...");
        // Add a small delay before attempting to reconnect to avoid spamming connection requests
        vTaskDelay(pdMS_TO_TICKS(5000)); 
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP address:" IPSTR, IP2STR(&event->ip_info.ip));

        // Start the HTTP server after getting IP
        // Check if server is already running, if needed, though start_webserver should handle it.
        if (start_webserver() == NULL) {
            ESP_LOGE(TAG, "Failed to start webserver after getting IP.");
        }
    }
}

void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif); // Ensure netif was created

    // Register event handlers
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .sta = {
            // .ssid and .password are set below using strncpy for safety
            .threshold.authmode = WIFI_AUTH_WPA2_PSK, // Standard WPA2-PSK
            // PMF (Protected Management Frames) settings for WPA2, can enhance security
            // .pmf_cfg = { // This structure is correct for ESP-IDF v4.x and v5.x
            //     .capable = true,
            //     .required = false // Set to true if AP requires PMF
            // },
        },
    };
    // Safely copy SSID and password
    strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) -1);
    strncpy((char *)wifi_config.sta.password, WIFI_PASSWORD, sizeof(wifi_config.sta.password) -1);
    wifi_config.sta.ssid[sizeof(wifi_config.sta.ssid)-1] = '\0'; // Ensure null termination
    wifi_config.sta.password[sizeof(wifi_config.sta.password)-1] = '\0'; // Ensure null termination


    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished. Attempting to connect to SSID: %s", WIFI_SSID);
}

void spiffs_init(void)
{
    ESP_LOGI(TAG, "Initializing SPIFFS");

    esp_vfs_spiffs_conf_t conf = {
      .base_path = "/spiffs",
      .partition_label = "spiffs", // Ensure this label matches your partition table
      .max_files = 5,
      .format_if_mount_failed = true
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition. Check partition_label.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s). Label: %s", 
                 esp_err_to_name(ret), conf.partition_label ? conf.partition_label : "NULL");
    } else {
        ESP_LOGI(TAG, "Partition size: total: %zu, used: %zu", total, used); // Use %zu for size_t
    }

    // Example: Write and read a dummy file
    FILE* f = fopen("/spiffs/my_data.txt", "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", strerror(errno));
        return;
    }
    fprintf(f, "Receiver data stored here.");
    fclose(f);
    ESP_LOGI(TAG, "File written to /spiffs/my_data.txt");

    char line[128];
    f = fopen("/spiffs/my_data.txt", "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for reading: %s", strerror(errno));
        return;
    }
    if (fgets(line, sizeof(line), f) != NULL) {
        // Remove newline character if present
        line[strcspn(line, "\n")] = 0;
        ESP_LOGI(TAG, "Read from file: '%s'", line);
    } else {
        ESP_LOGE(TAG, "Failed to read from file or file empty.");
    }
    fclose(f);
}


void app_main(void)
{
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta(); // Initializes Wi-Fi and starts connection attempts

    spiffs_init();

    // Start the LED toggling task
    xTaskCreate(&led_toggle_task, "led_toggle_task", 2048, NULL, 10, NULL);

    // The HTTP server will start once Wi-Fi connects and gets an IP (handled in event_handler)
    ESP_LOGI(TAG, "Receiver initialization complete. Waiting for Wi-Fi connection and IP.");
}

