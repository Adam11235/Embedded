// main/main.c (or securedOTA.c - ensure your filename matches)
#include <stdio.h>
#include <string.h>
#include <sys/stat.h> 
#include <fcntl.h>    
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_flash_partitions.h"
#include "esp_partition.h"
#include "errno.h" // For strerror
#include "esp_spiffs.h"    
#include "cJSON.h"         
#include "esp_https_ota.h" 
#include "sdkconfig.h"     // For Kconfig defines like CONFIG_SPIFFS_OBJ_NAME_LEN

// --- Wi-Fi AP Configuration Macros ---
// These macros will be used for AP setup.
#define WIFI_AP_SSID                "MySimpleESP_AP"
#define WIFI_AP_PASSWORD            "simplepass123" // Use "" for an open network
#define WIFI_AP_CHANNEL             6  // Wi-Fi channel (1-13)
#define WIFI_AP_MAX_CONNECTIONS     2  // Maximum number of clients that can connect

// Firmware version - CHANGE THIS FOR OTA UPDATES
#define FIRMWARE_VERSION "1.0.0" 

#ifndef OTA_BUF_SIZE
#define OTA_BUF_SIZE 2048
#endif

// Using CONFIG_SPIFFS_OBJ_NAME_LEN from sdkconfig.h for this, as it's a system limit.
// Ensure sdkconfig.h is generated and CONFIG_SPIFFS_OBJ_NAME_LEN is defined.
// If it's not, you might need to define a fixed size here, e.g., (64 + 8 + 1)
#if defined(CONFIG_SPIFFS_OBJ_NAME_LEN)
#define MAX_FILE_PATH_LEN (CONFIG_SPIFFS_OBJ_NAME_LEN + sizeof(SPIFFS_MOUNT_POINT) + 1)
#else
#define MAX_FILE_PATH_LEN (64 + 8 + 1) // Fallback: 64 for name, 8 for /spiffs/, 1 for null
#endif


#ifndef MIN
#define MIN(a,b) (((a)<(b))?(a):(b))
#endif

static const char *TAG = "ota_app_spiffs";
static const char *SPIFFS_MOUNT_POINT = "/spiffs";

#define NVS_MESSAGE_KEY "custom_msg"
char current_custom_message[128] = "Hello from ESP32 via SPIFFS!"; 

// --- SPIFFS Initialization ---
static esp_err_t init_spiffs(void) {
    ESP_LOGI(TAG, "Initializing SPIFFS");
    esp_vfs_spiffs_conf_t conf = {
        .base_path = SPIFFS_MOUNT_POINT,
        .partition_label = "storage", 
        .max_files = 5,               
        .format_if_mount_failed = true 
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) ESP_LOGE(TAG, "Failed to mount or format filesystem");
        else if (ret == ESP_ERR_NOT_FOUND) ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        else ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        return ESP_FAIL;
    }
    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK) ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    else ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    return ESP_OK;
}

// --- NVS Functions ---
esp_err_t save_custom_message_nvs(const char* message) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle); 
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_str(nvs_handle, NVS_MESSAGE_KEY, message);
    if (err == ESP_OK) err = nvs_commit(nvs_handle);
    if (err != ESP_OK) ESP_LOGE(TAG, "Failed to set/commit NVS: %s", esp_err_to_name(err));
    nvs_close(nvs_handle);
    if (err == ESP_OK) {
        strncpy(current_custom_message, message, sizeof(current_custom_message) - 1);
        current_custom_message[sizeof(current_custom_message) - 1] = '\0';
        ESP_LOGI(TAG, "Custom message saved to NVS: %s", message);
    }
    return err;
}

esp_err_t load_custom_message_nvs() {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "NVS not initialized or key not found. Using default message.");
        return err; 
    }
    size_t required_size = sizeof(current_custom_message);
    err = nvs_get_str(nvs_handle, NVS_MESSAGE_KEY, current_custom_message, &required_size);
    nvs_close(nvs_handle);
    switch (err) {
        case ESP_OK: ESP_LOGI(TAG, "Custom message loaded from NVS: %s", current_custom_message); break;
        case ESP_ERR_NVS_NOT_FOUND: ESP_LOGI(TAG, "NVS key '%s' not found. Using default.", NVS_MESSAGE_KEY); break;
        default: ESP_LOGE(TAG, "Error (%s) reading NVS. Using default.", esp_err_to_name(err));
    }
    return ESP_OK; 
}

// --- HTTP Server Handlers ---
static esp_err_t root_get_handler(httpd_req_t *req) {
    char filepath[MAX_FILE_PATH_LEN]; 
    snprintf(filepath, sizeof(filepath), "%s/index.html", SPIFFS_MOUNT_POINT);
    ESP_LOGI(TAG, "Serving file: %s", filepath);
    struct stat st;
    if (stat(filepath, &st) == -1) {
        ESP_LOGE(TAG, "File %s not found, error %d (%s)", filepath, errno, strerror(errno));
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }
    int fd = open(filepath, O_RDONLY, 0);
    if (fd == -1) {
        ESP_LOGE(TAG, "Failed to open file: %s, error %d (%s)", filepath, errno, strerror(errno));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read file");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/html");
    char *chunk = (char *)malloc(OTA_BUF_SIZE);
    if (!chunk) {
        ESP_LOGE(TAG, "Failed to allocate memory for file chunk");
        close(fd);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation error");
        return ESP_FAIL;
    }
    ssize_t read_bytes;
    do {
        read_bytes = read(fd, chunk, OTA_BUF_SIZE);
        if (read_bytes > 0) {
            if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK) {
                ESP_LOGE(TAG, "File sending failed!");
                close(fd); free(chunk); return ESP_FAIL;
            }
        } else if (read_bytes < 0) {
            ESP_LOGE(TAG, "Error reading file: %s, error %d (%s)", filepath, errno, strerror(errno));
            close(fd); free(chunk); return ESP_FAIL;
        }
    } while (read_bytes > 0);
    close(fd); free(chunk);
    if (httpd_resp_send_chunk(req, NULL, 0) != ESP_OK) { 
        ESP_LOGE(TAG, "Failed to send final chunk."); return ESP_FAIL;
    }
    ESP_LOGI(TAG, "File sending complete: %s", filepath);
    return ESP_OK;
}

static esp_err_t api_status_get_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "/api/status called");
    httpd_resp_set_type(req, "application/json");
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON creation failed (root)"); return ESP_FAIL; }
    if(cJSON_AddStringToObject(root, "firmware_version", FIRMWARE_VERSION) == NULL) {
        cJSON_Delete(root); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON creation failed (version)"); return ESP_FAIL;
    }
    if(cJSON_AddStringToObject(root, "custom_message", current_custom_message) == NULL) {
        cJSON_Delete(root); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON creation failed (message)"); return ESP_FAIL;
    }
    char *json_string = cJSON_PrintUnformatted(root);
    if (json_string == NULL) {
        ESP_LOGE(TAG, "Failed to print cJSON object");
        cJSON_Delete(root); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON stringification failed"); return ESP_FAIL;
    }
    httpd_resp_send(req, json_string, strlen(json_string));
    free(json_string); cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t update_message_post_handler(httpd_req_t *req) {
    char buf[200]; 
    int ret, remaining = req->content_len;
    ESP_LOGI(TAG, "/update-message called, content_len: %d", remaining);
    if (remaining >= sizeof(buf)) { 
        ESP_LOGE(TAG, "Payload too large for buffer (max %d, got %d)", (int)sizeof(buf)-1, remaining);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload too large"); return ESP_FAIL;
    }
    int received = 0;
    while (remaining > 0) {
        ret = httpd_req_recv(req, buf + received, MIN(remaining, (int)sizeof(buf) - 1 - received));
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) httpd_resp_send_408(req);
            ESP_LOGE(TAG, "Failed to receive POST data (ret: %d)", ret); return ESP_FAIL;
        }
        received += ret; remaining -= ret;
    }
    buf[received] = '\0'; 
    ESP_LOGD(TAG, "Received data for /update-message: %s", buf);
    char message_val[sizeof(current_custom_message)] = {0}; 
    if (httpd_query_key_value(buf, "message", message_val, sizeof(message_val)) == ESP_OK) {
        ESP_LOGI(TAG, "Extracted message for update: '%s'", message_val);
        if (save_custom_message_nvs(message_val) == ESP_OK) api_status_get_handler(req); 
        else httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save message");
    } else {
        ESP_LOGE(TAG, "Failed to parse 'message' from POST data: %s", buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'message' parameter or parse error"); return ESP_FAIL;
    }
    return ESP_OK;
}

// --- OTA Logic ---
static void ota_task(void *pvParameter) {
    char *ota_url = (char *)pvParameter;
    ESP_LOGI(TAG, "Starting OTA task for URL: %s. Current FW: %s", ota_url, FIRMWARE_VERSION);
    esp_http_client_config_t http_client_config = {
        .url = ota_url, .timeout_ms = 15000, .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_config = { .http_config = &http_client_config, };
    esp_err_t ret = esp_https_ota(&ota_config); 
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA Succeeded, Rebooting...");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA Failed... (%s)", esp_err_to_name(ret));
        if (ret == ESP_ERR_OTA_VALIDATE_FAILED) ESP_LOGE(TAG, "Image validation failed, possibly signature mismatch or corrupted image.");
    }
    free(ota_url); 
    vTaskDelete(NULL); 
}

static esp_err_t ota_post_handler(httpd_req_t *req) {
    char buf[256]; 
    int ret, remaining = req->content_len;
    if (remaining >= sizeof(buf)) {
        ESP_LOGE(TAG, "OTA URL payload too large");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "URL too long"); return ESP_FAIL;
    }
    int received = 0;
    while (remaining > 0) {
        ret = httpd_req_recv(req, buf + received, MIN(remaining, (int)sizeof(buf) - 1 - received));
        if (ret <= 0) { 
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) httpd_resp_send_408(req);
            ESP_LOGE(TAG, "Failed to receive OTA URL data"); return ESP_FAIL; 
        }
        received += ret; remaining -= ret;
    }
    buf[received] = '\0';
    char firmware_url[200] = {0}; 
    if (httpd_query_key_value(buf, "url", firmware_url, sizeof(firmware_url)) == ESP_OK) {
        ESP_LOGI(TAG, "OTA URL received: %s", firmware_url);
        char *url_for_task = strdup(firmware_url); 
        if (url_for_task == NULL) {
            ESP_LOGE(TAG, "Failed to allocate memory for OTA URL task argument");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed"); return ESP_FAIL;
        }
        if (xTaskCreate(&ota_task, "ota_task", 8192, (void *)url_for_task, 5, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create OTA task");
            free(url_for_task); 
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to start OTA process"); return ESP_FAIL;
        }
        httpd_resp_send(req, "OTA process initiated.", HTTPD_RESP_USE_STRLEN);
    } else {
        ESP_LOGE(TAG, "Failed to parse 'url' from OTA POST data: %s", buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'url' parameter for OTA"); return ESP_FAIL;
    }
    return ESP_OK;
}

// --- HTTP Server Setup ---
static httpd_handle_t start_webserver(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 10; 
    config.stack_size = 8192;    
    httpd_uri_t root_uri = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL};
    httpd_uri_t api_status_uri = { .uri = "/api/status", .method = HTTP_GET, .handler = api_status_get_handler, .user_ctx = NULL};
    httpd_uri_t update_msg_uri = { .uri = "/update-message", .method = HTTP_POST, .handler = update_message_post_handler, .user_ctx = NULL};
    httpd_uri_t ota_uri = { .uri = "/ota", .method = HTTP_POST, .handler = ota_post_handler, .user_ctx = NULL};
    ESP_LOGI(TAG, "Starting HTTP server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &root_uri);
        httpd_register_uri_handler(server, &api_status_uri);
        httpd_register_uri_handler(server, &update_msg_uri);
        httpd_register_uri_handler(server, &ota_uri);
        return server;
    }
    ESP_LOGE(TAG, "Error starting HTTP server!");
    return NULL;
}

// --- Wi-Fi AP Setup ---
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_id == WIFI_EVENT_AP_STACONNECTED) ESP_LOGI(TAG, "Station connected");
    else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) ESP_LOGI(TAG, "Station disconnected");
}

void wifi_init_softap(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t* ap_netif = esp_netif_create_default_wifi_ap(); 
    assert(ap_netif); 

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL, NULL));

    wifi_config_t wifi_config = {
        .ap = {
            // Using hardcoded macros for AP settings for simplicity
            .ssid = WIFI_AP_SSID,                       // Uses macro from top of file
            .ssid_len = strlen(WIFI_AP_SSID),
            .channel = WIFI_AP_CHANNEL,                 // Uses macro from top of file
            .password = WIFI_AP_PASSWORD,               // Uses macro from top of file
            .max_connection = WIFI_AP_MAX_CONNECTIONS,  // Uses macro from top of file
            .authmode = WIFI_AUTH_WPA2_PSK, 
            .pmf_cfg = { .required = false }, 
        },
    };

    // If the hardcoded password is empty, set auth mode to open
    if (strlen(WIFI_AP_PASSWORD) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
        ESP_LOGI(TAG, "Configuring AP as OPEN network (no password).");
    } else if (strlen(WIFI_AP_PASSWORD) < 8 && wifi_config.ap.authmode == WIFI_AUTH_WPA2_PSK) {
         // WPA2-PSK passwords should be 8-63 characters.
         ESP_LOGW(TAG, "Password is less than 8 characters. WPA2_PSK may not work as expected or may be rejected by clients.");
    }


    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_softap finished. SSID:'%s' password:'%s' channel:%d max_conn:%d",
             WIFI_AP_SSID,
             strlen(WIFI_AP_PASSWORD) > 0 ? WIFI_AP_PASSWORD : "[NO PASSWORD/OPEN]", 
             WIFI_AP_CHANNEL,
             WIFI_AP_MAX_CONNECTIONS);

    esp_netif_ip_info_t ip_info;
    ESP_ERROR_CHECK(esp_netif_get_ip_info(ap_netif, &ip_info)); 
    ESP_LOGI(TAG, "AP IP Address: " IPSTR, IP2STR(&ip_info.ip));
}

// --- Main Application ---
void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    load_custom_message_nvs();
    ESP_ERROR_CHECK(init_spiffs()); 
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "Running partition: %s. Firmware Version: %s", running->label, FIRMWARE_VERSION);
    wifi_init_softap();
    if(start_webserver() == NULL) {
        ESP_LOGE(TAG, "Failed to start webserver. Halting.");
        while(1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }
    // Use the WIFI_AP_SSID macro here as well for consistency in logging
    ESP_LOGI(TAG, "ESP32 OTA Signed App (SPIFFS) Initialized. Connect to AP: '%s'", WIFI_AP_SSID);
}

