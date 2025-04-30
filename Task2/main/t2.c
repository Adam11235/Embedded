#include <stdio.h>
#include <string.h>     // For strlen, memcpy
#include <fcntl.h>      // For open/read/close (SPIFFS file serving)

// FreeRTOS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ESP-IDF Core
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
// #include "esp_flash.h" // Celowo usunięte

// Wi-Fi
#include "esp_wifi.h"

// SPIFFS
#include "esp_spiffs.h"
#include "esp_vfs.h"

// ADC Continuous Mode
#include "esp_adc/adc_continuous.h"

// HTTP Server
#include "esp_http_server.h"

// --- Logging TAGs ---
static const char *TAG_MAIN = "MAIN";
static const char *TAG_WIFI = "WIFI_AP";
static const char *TAG_SPIFFS = "SPIFFS";
static const char *TAG_ADC = "ADC_CONT";
static const char *TAG_WEB = "WEB_SRV";

// --- Wi-Fi AP Configuration ---
#define WIFI_AP_SSID         "ESP32_Sensor_AP"
#define WIFI_AP_PASS         "password123" // Minimum 8 characters (or "" for open)
#define WIFI_AP_CHANNEL      1
#define WIFI_AP_MAX_CONN     4

// --- ADC Configuration (ta, która działała) ---
#define ADC_READER_CHANNEL      ADC_CHANNEL_7       // GPIO35 (lub ADC_CHANNEL_6 dla GPIO34)
#define ADC_READER_ATTEN        ADC_ATTEN_DB_12     // Tłumienie 12dB (zakres ~0-3.3V)
#define ADC_READER_BITWIDTH     ADC_BITWIDTH_12     // Jawna rozdzielczość 12 bit
#define ADC_READER_READ_LEN     128                 // Mniejszy rozmiar odczytu DMA
#define ADC_READER_SAMPLE_FREQ  (20 * 1000)         // Częstotliwość próbkowania 20kHz
#define ADC_READER_BUF_SIZE     512                 // Mniejszy całkowity rozmiar bufora
#define ADC_READER_FRAME_SIZE   ADC_READER_READ_LEN // Rozmiar ramki = rozmiar odczytu (128)

// --- Web Server Configuration ---
#define FILE_PATH_MAX           550                 // Zwiększony rozmiar bufora na ścieżkę
#define SCRATCH_BUFSIZE         (10240)             // Bufor do odczytu plików (można zmniejszyć)

// --- Global Static Variables ---
static adc_continuous_handle_t s_adc_handle = NULL;
static volatile int s_latest_adc_value = 0;
static httpd_handle_t s_web_server_handle = NULL;

//==============================================================================
// Funkcje Pomocnicze i Callbacki (zdefiniowane przed użyciem)
//==============================================================================

/**
 * @brief ADC Continuous mode callback function.
 */
static bool IRAM_ATTR s_adc_conv_done_cb(adc_continuous_handle_t handle, const adc_continuous_evt_data_t *edata, void *user_data)
{
    uint8_t *result_buffer = edata->conv_frame_buffer;
    uint32_t result_len = edata->size;
    uint64_t sum = 0;
    uint32_t count = 0;

    for (int i = 0; i < result_len; i += SOC_ADC_DIGI_RESULT_BYTES) {
        adc_digi_output_data_t *p_data = (adc_digi_output_data_t*)&result_buffer[i];
        if (p_data->type1.channel == ADC_READER_CHANNEL) {
            sum += p_data->type1.data;
            count++;
        }
    }
    if (count > 0) {
        s_latest_adc_value = (int)(sum / count);
    }
    return true;
}

/**
 * @brief Wi-Fi event handler (simplified, no logging).
 */
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{ /* Empty */ }

/**
 * @brief Simple helper to set MIME type based on file extension
 */
static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filepath)
{
    const char *type = "text/plain";
    if (strstr(filepath, ".html")) type = "text/html";
    else if (strstr(filepath, ".js")) type = "application/javascript";
    else if (strstr(filepath, ".css")) type = "text/css";
    else if (strstr(filepath, ".png")) type = "image/png";
    else if (strstr(filepath, ".ico")) type = "image/x-icon";
    else if (strstr(filepath, ".jpg")) type = "image/jpeg";
    else if (strstr(filepath, ".json")) type = "application/json";
    return httpd_resp_set_type(req, type);
}


//==============================================================================
// ADC Reader Implementation
//==============================================================================

/**
 * @brief Gets the latest averaged ADC value.
 */
static int adc_reader_get_value(void) {
    return s_latest_adc_value;
}

/**
 * @brief Initializes ADC in continuous mode.
 */
static esp_err_t adc_reader_init(void)
{
    esp_err_t ret = ESP_OK;
    adc_continuous_handle_cfg_t adc_handle_cfg = {
        .max_store_buf_size = ADC_READER_BUF_SIZE,
        .conv_frame_size = ADC_READER_FRAME_SIZE,
    };
    ret = adc_continuous_new_handle(&adc_handle_cfg, &s_adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_ADC, "Failed to create handle: %s", esp_err_to_name(ret));
        s_adc_handle = NULL; return ret;
    }

    adc_continuous_config_t adc_run_cfg = {
        .sample_freq_hz = ADC_READER_SAMPLE_FREQ,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE1,
    };
    adc_digi_pattern_config_t adc_pattern[1] = {0};
    adc_pattern[0].atten = ADC_READER_ATTEN;
    adc_pattern[0].channel = ADC_READER_CHANNEL;
    adc_pattern[0].unit = ADC_UNIT_1;
    adc_pattern[0].bit_width = ADC_READER_BITWIDTH;
    adc_run_cfg.pattern_num = 1;
    adc_run_cfg.adc_pattern = adc_pattern;

    ret = adc_continuous_config(s_adc_handle, &adc_run_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_ADC, "Failed to configure params: %s", esp_err_to_name(ret));
        adc_continuous_deinit(s_adc_handle); s_adc_handle = NULL; return ret;
    }

    // s_adc_conv_done_cb musi być zdefiniowany przed tą linią
    adc_continuous_evt_cbs_t cbs = { .on_conv_done = s_adc_conv_done_cb };
    ret = adc_continuous_register_event_callbacks(s_adc_handle, &cbs, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_ADC, "Failed to register callback: %s", esp_err_to_name(ret));
        adc_continuous_deinit(s_adc_handle); s_adc_handle = NULL; return ret;
    }

    ret = adc_continuous_start(s_adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_ADC, "Failed to start: %s", esp_err_to_name(ret));
        adc_continuous_deinit(s_adc_handle); s_adc_handle = NULL; return ret;
    }
    ESP_LOGI(TAG_ADC, "ADC Continuous mode initialized and started successfully.");
    return ESP_OK;
}

/**
 * @brief Deinitializes ADC continuous mode. (unused)
 */
static esp_err_t adc_reader_deinit(void) {
    if (s_adc_handle) {
        esp_err_t ret_stop = adc_continuous_stop(s_adc_handle);
        if (ret_stop != ESP_OK) ESP_LOGE(TAG_ADC, "Failed to stop ADC: %s", esp_err_to_name(ret_stop));
        esp_err_t ret_del = adc_continuous_deinit(s_adc_handle);
        if (ret_del != ESP_OK) {
            ESP_LOGE(TAG_ADC, "Failed to deinit ADC: %s", esp_err_to_name(ret_del));
            s_adc_handle = NULL; return ret_del;
        }
        s_adc_handle = NULL;
        ESP_LOGI(TAG_ADC, "ADC Continuous mode deinitialized.");
        return ret_stop;
    }
    return ESP_OK;
}

//==============================================================================
// SPIFFS Initialization Implementation
//==============================================================================
static esp_err_t init_spiffs(void)
{
    ESP_LOGI(TAG_SPIFFS, "Initializing SPIFFS");
    esp_vfs_spiffs_conf_t conf = {
      .base_path = "/storage", .partition_label = "storage",
      .max_files = 5, .format_if_mount_failed = true };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) ESP_LOGE(TAG_SPIFFS, "Failed to mount or format filesystem");
        else if (ret == ESP_ERR_NOT_FOUND) ESP_LOGE(TAG_SPIFFS, "Failed to find SPIFFS partition");
        else ESP_LOGE(TAG_SPIFFS, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        return ret;
    }
    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK) ESP_LOGE(TAG_SPIFFS, "Failed to get SPIFFS info (%s)", esp_err_to_name(ret));
    else ESP_LOGI(TAG_SPIFFS, "Partition size: total: %d, used: %d", total, used);
    return ESP_OK;
}

//==============================================================================
// Wi-Fi AP Implementation
//==============================================================================
static void wifi_init_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    // wifi_event_handler musi być zdefiniowany przed tą linią
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    wifi_config_t wifi_config = { .ap = { .ssid = WIFI_AP_SSID, .ssid_len = strlen(WIFI_AP_SSID), .channel = WIFI_AP_CHANNEL,
        .password = WIFI_AP_PASS, .max_connection = WIFI_AP_MAX_CONN, .authmode = WIFI_AUTH_WPA_WPA2_PSK } };
    if (strlen(WIFI_AP_PASS) == 0) wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG_WIFI, "SoftAP configured. SSID:%s password:%s", WIFI_AP_SSID, WIFI_AP_PASS);
    esp_netif_ip_info_t ip_info;
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (netif != NULL) {
         esp_netif_get_ip_info(netif, &ip_info);
         ESP_LOGI(TAG_WIFI, "AP IP Address: " IPSTR, IP2STR(&ip_info.ip));
    }
}

//==============================================================================
// Web Server Implementation (Uproszczone handlery)
//==============================================================================

/**
 * @brief Handler tylko dla roota '/', serwuje /storage/index.html
 * *** BEZ OCHRONY FLASH GUARD - RYZYKO CRASHU POZOSTAJE ***
 */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];
    snprintf(filepath, sizeof(filepath), "/storage/index.html");

    // Bezpośredni dostęp do SPIFFS - może powodować konflikt z ADC ISR
    int fd = open(filepath, O_RDONLY, 0);
    if (fd == -1) {
        ESP_LOGE(TAG_WEB, "Failed to open %s", filepath);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    // set_content_type_from_file musi być zdefiniowana przed tą linią
    set_content_type_from_file(req, filepath);

    char *chunk = (char *)malloc(SCRATCH_BUFSIZE);
    if (!chunk) {
         ESP_LOGE(TAG_WEB, "Failed to allocate scratch buffer");
         close(fd);
         httpd_resp_send_500(req);
         return ESP_FAIL;
    }

    ssize_t read_bytes;
    esp_err_t send_err = ESP_OK;
    do {
        read_bytes = read(fd, chunk, SCRATCH_BUFSIZE); // Odczyt może powodować konflikt
        if (read_bytes == -1) {
            ESP_LOGE(TAG_WEB, "Error reading file : %s", filepath);
            send_err = ESP_FAIL; break;
        } else if (read_bytes > 0) {
            if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK) {
                ESP_LOGE(TAG_WEB, "File sending failed!");
                send_err = ESP_FAIL; break;
            }
        }
    } while (read_bytes > 0);

    close(fd);
    free(chunk);

    if (send_err == ESP_OK && read_bytes == 0) {
         ESP_LOGI(TAG_WEB, "File '%s' sending complete", filepath);
         httpd_resp_send_chunk(req, NULL, 0);
         return ESP_OK;
    } else {
         httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
         return ESP_FAIL;
    }
}

/**
 * @brief Handler danych JSON (endpoint /data)
 */
static esp_err_t data_get_handler(httpd_req_t *req)
{
    // adc_reader_get_value musi być zdefiniowana przed tą linią
    ESP_LOGI(TAG_WEB, "/data handler entered"); // Log testowy
    httpd_resp_set_type(req, "application/json");
    int adc_val = adc_reader_get_value();
    char resp_str[64];
    snprintf(resp_str, sizeof(resp_str), "{\"adcValue\": %d}", adc_val);
    httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/**
 * @brief Starts the HTTP web server (uproszczona wersja)
 */
static esp_err_t start_webserver(void)
{
    // Handlery root_get_handler i data_get_handler muszą być zdefiniowane PRZED tą funkcją
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    ESP_LOGI(TAG_WEB, "Starting server on port: '%d'", config.server_port);
    esp_err_t ret = httpd_start(&s_web_server_handle, &config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG_WEB, "Registering URI handlers");

        // Handler dla /data
        httpd_uri_t data_uri = { .uri = "/data", .method = HTTP_GET, .handler = data_get_handler };
        httpd_register_uri_handler(s_web_server_handle, &data_uri);

        // Handler dla roota '/'
        httpd_uri_t root_uri = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
        httpd_register_uri_handler(s_web_server_handle, &root_uri);

        return ESP_OK;
    }
    ESP_LOGE(TAG_WEB, "Error starting server: %s", esp_err_to_name(ret));
    return ret;
}

/**
 * @brief Stops the HTTP web server. (unused)
 */
static esp_err_t stop_webserver(void)
{
    if (s_web_server_handle) {
        ESP_LOGI(TAG_WEB,"Stopping web server");
        esp_err_t ret = httpd_stop(s_web_server_handle);
        if (ret == ESP_OK) { s_web_server_handle = NULL; return ESP_OK; }
        else { ESP_LOGE(TAG_WEB,"Failed to stop web server: %s", esp_err_to_name(ret)); return ret; }
    }
     return ESP_OK;
}

//==============================================================================
// Main Application Entry Point (musi być na końcu)
//==============================================================================
void app_main(void)
{
    // Upewnij się, że wszystkie funkcje init... i start_webserver są zdefiniowane PRZED app_main
    ESP_LOGI(TAG_MAIN, "Starting Application");

    // 1. NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_LOGW(TAG_MAIN, "NVS erase..."); ESP_ERROR_CHECK(nvs_flash_erase()); ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. SPIFFS
    ESP_ERROR_CHECK(init_spiffs());

    // 3. Wi-Fi
    wifi_init_softap();

    // 4. ADC
    ESP_ERROR_CHECK(adc_reader_init());

    // 5. Web Server
    ESP_ERROR_CHECK(start_webserver());

    ESP_LOGI(TAG_MAIN, "Initialization finished. System running.");
}