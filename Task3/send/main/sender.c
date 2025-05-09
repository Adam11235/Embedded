#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_http_client.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include <esp_spiffs.h>
#include <cJSON.h> 
#include <string.h> 
#include <errno.h>

static const char *TAG = "sender";

#define EXAMPLE_ESP_WIFI_SSID      "NadawcaOTA"
#define EXAMPLE_ESP_WIFI_PASS      "password123"
#define EXAMPLE_MAX_STA_CONN       4

// Receiver's IP address (default for ESP32 AP) and the endpoint for data
#define RECEIVER_CONTROL_URL   "http://192.168.4.2/control" 

// Struct to send to the receiver
typedef struct {
    bool toggle;
    char message[100];
} control_data_t;

control_data_t current_control_data = {
    .toggle = false,
    .message = "Hello from sender!"
};

static esp_err_t http_client_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "HTTP_EVENT_ERROR: %s", esp_err_to_name(esp_http_client_get_errno(evt->client)));
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            if (evt->data_len > 0 && evt->data != NULL) {
                 ESP_LOGI(TAG, "Received data: %.*s", evt->data_len, (char*)evt->data);
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
        case HTTP_EVENT_REDIRECT:
            ESP_LOGI(TAG, "HTTP_EVENT_REDIRECT");
            break;
    }
    return ESP_OK;
}

void send_control_data_task(void *pvParameter)
{
    esp_http_client_config_t config = {
        .url = RECEIVER_CONTROL_URL,
        .method = HTTP_METHOD_POST,
        .event_handler = http_client_event_handler,
        .transport_type = HTTP_TRANSPORT_OVER_TCP, 
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        vTaskDelete(NULL);
        return; 
    }

    bool toggle_state = false;
    int message_counter = 0;

    while (1) {
        current_control_data.toggle = toggle_state;
        snprintf(current_control_data.message, sizeof(current_control_data.message), "Message #%d", message_counter++);

        cJSON *root = cJSON_CreateObject();
        if (root == NULL) {
            ESP_LOGE(TAG, "Failed to create JSON object");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        cJSON_AddBoolToObject(root, "toggle", current_control_data.toggle);
        cJSON_AddStringToObject(root, "message", current_control_data.message);

        char *post_data = cJSON_PrintUnformatted(root);
        if (post_data == NULL) {
            ESP_LOGE(TAG, "Failed to print JSON");
            cJSON_Delete(root);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        ESP_LOGI(TAG, "Sending data: %s", post_data);

        esp_err_t set_post_err = esp_http_client_set_post_field(client, post_data, strlen(post_data));
        if (set_post_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set post field: %s", esp_err_to_name(set_post_err));
            cJSON_Delete(root);
            cJSON_free(post_data);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        
        esp_err_t set_header_err = esp_http_client_set_header(client, "Content-Type", "application/json");
         if (set_header_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set Content-Type header: %s", esp_err_to_name(set_header_err));
        }

        esp_err_t err = esp_http_client_perform(client);

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "HTTP POST Status = %d, content_length = %lld",
                     (int)esp_http_client_get_status_code(client), 
                     esp_http_client_get_content_length(client));
        } else {
            ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
        }

        cJSON_Delete(root);
        cJSON_free(post_data); 

        toggle_state = !toggle_state;
        vTaskDelay(pdMS_TO_TICKS(5000)); 
    }

    esp_http_client_cleanup(client);
    vTaskDelete(NULL);
}


static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "WiFi AP started");
        xTaskCreate(&send_control_data_task, "send_data_task", 4096, NULL, 5, NULL);
    }
    else if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "Station joined, AID=%d", event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "Station left, AID=%d", event->aid);
    }
}

void wifi_init_softap(void)
{
    // Initialize TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());
    
    // Create default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize Wi-Fi with default configuration
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg)); // Initialize Wi-Fi stack FIRST

    // Create default Wi-Fi AP network interface AFTER esp_wifi_init()
    esp_netif_t *p_netif_ap = esp_netif_create_default_wifi_ap();
    if (p_netif_ap == NULL) {
        ESP_LOGE(TAG, "Failed to create default Wi-Fi AP interface");
        // Handle error appropriately, perhaps by aborting or retrying
        abort(); 
    }
    
    // Register Wi-Fi event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    // Configure Wi-Fi AP settings
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .ssid_len = strlen(EXAMPLE_ESP_WIFI_SSID),
            .channel = 1, 
            .password = EXAMPLE_ESP_WIFI_PASS,
            .max_connection = EXAMPLE_MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
            //.pmf_cfg = { // Optional: For WPA3/WPA2 mixed mode, if AP supports PMF
            //    .required = false,
            //},
        },
    };
    // If password is empty, set auth mode to open
    if (strlen(EXAMPLE_ESP_WIFI_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    // Set Wi-Fi mode to AP
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP)); // This is line 206 in the original sender.c
    
    // Set Wi-Fi configuration
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    
    // Start Wi-Fi
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_softap finished. SSID:%s password:%s channel:%d",
             EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS, wifi_config.ap.channel);
}

void spiffs_init(void)
{
    ESP_LOGI(TAG, "Initializing SPIFFS");

    esp_vfs_spiffs_conf_t conf = {
      .base_path = "/spiffs",
      .partition_label = "storage", 
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
        ESP_LOGI(TAG, "SPIFFS Partition size: total: %zu, used: %zu", total, used); 
    }

    FILE* f = fopen("/spiffs/hello.txt", "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s (errno %d)", strerror(errno), errno);
        return;
    }
    fprintf(f, "Hello SPIFFS from sender!");
    fclose(f);
    ESP_LOGI(TAG, "File written to /spiffs/hello.txt");
}


void app_main(void)
{
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_LOGW(TAG, "NVS: %s. Erasing NVS.", esp_err_to_name(ret));
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized.");

    ESP_LOGI(TAG, "ESP_WIFI_MODE_AP - Initializing SoftAP");
    wifi_init_softap(); // Initialize Wi-Fi and start AP mode

    // Initialize SPIFFS *after* Wi-Fi and other core services if it's not critical for boot
    // Or if it depends on resources that are set up by Wi-Fi/Netif tasks.
    // In this case, it seems independent enough to be here.
    spiffs_init();

    ESP_LOGI(TAG, "Sender initialization complete. Waiting for AP to start and data task to run.");
}

