/* ESP HTTP Client Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "protocol_examples_common.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"

#include "esp_http_client.h"

#define MAX_HTTP_RECV_BUFFER 512
#define MAX_HTTP_OUTPUT_BUFFER 2048

#define GPIO_RPM GPIO_NUM_35
#define GPIO_PRESS GPIO_NUM_36
#define GPIO_END GPIO_NUM_39

#define DEFAULT_VREF    1100        //Use adc2_vref_to_gpio() to obtain a better estimate
#define NO_OF_SAMPLES   64          //Multisampling


static const char *TAG = "HTTP_CLIENT";
//adc
static esp_adc_cal_characteristics_t *adc_chars;
static const adc_channel_t channel = ADC_CHANNEL_0;
static const adc_bits_width_t width = ADC_WIDTH_BIT_12;
static const adc_atten_t atten = ADC_ATTEN_DB_11;
static const adc_unit_t unit = ADC_UNIT_1;

uint8_t final_rpm = 0;
uint8_t final_pression = 0;
uint8_t final_end = 0;

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    static char *output_buffer;  // Buffer to store response of http request from event handler
    static int output_len;       // Stores number of bytes read
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            /*
             *  Check for chunked encoding is added as the URL for chunked encoding used in this example returns binary data.
             *  However, event handler can also be used in case chunked encoding is used.
             */
            if (!esp_http_client_is_chunked_response(evt->client)) {
                // If user_data buffer is configured, copy the response into the buffer
                if (evt->user_data) {
                    memcpy(evt->user_data + output_len, evt->data, evt->data_len);
                } else {
                    if (output_buffer == NULL) {
                        output_buffer = (char *) malloc(esp_http_client_get_content_length(evt->client));
                        output_len = 0;
                        if (output_buffer == NULL) {
                            ESP_LOGE(TAG, "Failed to allocate memory for output buffer");
                            return ESP_FAIL;
                        }
                    }
                    memcpy(output_buffer + output_len, evt->data, evt->data_len);
                }
                output_len += evt->data_len;
            }

            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            if (output_buffer != NULL) {
                // Response is accumulated in output_buffer. Uncomment the below line to print the accumulated response
                // ESP_LOG_BUFFER_HEX(TAG, output_buffer, output_len);
                free(output_buffer);
                output_buffer = NULL;
            }
            output_len = 0;
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            int mbedtls_err = 0;
            esp_err_t err = esp_tls_get_and_clear_last_error(evt->data, &mbedtls_err, NULL);
            if (err != 0) {
                ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
                ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
            }
            if (output_buffer != NULL) {
                free(output_buffer);
                output_buffer = NULL;
            }
            output_len = 0;
            break;
    }
    return ESP_OK;
}
static void check_efuse(void)
{
#if CONFIG_IDF_TARGET_ESP32
    //Check if TP is burned into eFuse
    if (esp_adc_cal_check_efuse(ESP_ADC_CAL_VAL_EFUSE_TP) == ESP_OK) {
        printf("eFuse Two Point: Supported\n");
    } else {
        printf("eFuse Two Point: NOT supported\n");
    }
    //Check Vref is burned into eFuse
    if (esp_adc_cal_check_efuse(ESP_ADC_CAL_VAL_EFUSE_VREF) == ESP_OK) {
        printf("eFuse Vref: Supported\n");
    } else {
        printf("eFuse Vref: NOT supported\n");
    }
#elif CONFIG_IDF_TARGET_ESP32S2
    if (esp_adc_cal_check_efuse(ESP_ADC_CAL_VAL_EFUSE_TP) == ESP_OK) {
        printf("eFuse Two Point: Supported\n");
    } else {
        printf("Cannot retrieve eFuse Two Point calibration values. Default calibration values will be used.\n");
    }
#else
#error "This example is configured for ESP32/ESP32S2."
#endif
}
static void print_char_val_type(esp_adc_cal_value_t val_type)
{
    if (val_type == ESP_ADC_CAL_VAL_EFUSE_TP) {
        printf("Characterized using Two Point Value\n");
    } else if (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF) {
        printf("Characterized using eFuse Vref\n");
    } else {
        printf("Characterized using Default Vref\n");
    }
}
void http_rest_with_url(void *pvParameters)
{
    char local_response_buffer[MAX_HTTP_OUTPUT_BUFFER] = {0};
    /**
     * NOTE: All the configuration parameters for http_client must be spefied either in URL or as host and path parameters.
     * If host and path parameters are not set, query parameter will be ignored. In such cases,
     * query parameter should be specified in URL.
     *
     * If URL as well as host and path parameters are specified, values of host and path will be considered.
     */
    esp_http_client_config_t config = {
        .host = "192.168.2.9",
        .port = 5000,
        .path = "/api/equipments",
        .query = "esp",
        .event_handler = _http_event_handler,
        .user_data = local_response_buffer,        // Pass address of local buffer to get response
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    // GET
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d",
                esp_http_client_get_status_code(client),
                esp_http_client_get_content_length(client));
    } else {
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "%s", local_response_buffer);
    /*//POST
    const char *post_data = "{\"fleet\": 4233,\"model\": \"HidroROLL\",\"lat\": 0,\"lng\": 0,\"engaged\": true,\"pression\": 50,\"speed\": 90,\"op\": 6656,\"group\": 1}";
    esp_http_client_set_url(client, "http://192.168.2.9:5000/api/equipments");
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));
    err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTP POST Status = %d, content_length = %d",
                esp_http_client_get_status_code(client),
                esp_http_client_get_content_length(client));
    } else {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
    }*/
    //ESP_LOGI(TAG, "%*.s", local_response_buffer);
    //ESP_LOG_BUFFER_CHAR_LEVEL(TAG, local_response_buffer, esp_http_client_get_content_length(client), esp_http_client_get_content_length(client));
    while(true){
        //PATCH
        char *hello_world = (char*)malloc(1024 * sizeof(char));
        time_t t;
        srand((unsigned) time(&t));
        const char *tt = (final_end) ? "true" : "false";
        
        // Prints "Hello world!" on hello_world
        sprintf(hello_world, "{\"fleet\": 4200,\"model\": \"HidroROLL\",\"lat\": 0,\"lng\": 0,\"engaged\": %s,\"pression\": %d,\"speed\": %d,\"op\": 6656,\"group\": 1}", tt, final_pression, final_rpm);
        esp_http_client_set_url(client, "http://192.168.2.9:5000/api/equipments/4200");
        esp_http_client_set_method(client, HTTP_METHOD_PATCH);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, hello_world, strlen(hello_world));
        err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "HTTP PATCH Status = %d, content_length = %d",
                    esp_http_client_get_status_code(client),
                    esp_http_client_get_content_length(client));
        } else {
            ESP_LOGE(TAG, "HTTP PATCH request failed: %s", esp_err_to_name(err));
        }
        free(hello_world);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    esp_http_client_cleanup(client);
}
void pheriperals_handle(void *pvParameters){
    int8_t last_pin_value = gpio_get_level(GPIO_RPM);
    uint8_t pin_count = 0;
    uint64_t last_time = 0;
    while(true)
    {
        int8_t pin = gpio_get_level(GPIO_RPM);
        if(pin != last_pin_value)
        {
          pin_count++;
          if(pin_count==30){
            int32_t one_rot =  (esp_timer_get_time() - last_time) / 1000;
            int32_t rpm = (60000 / one_rot);
            final_rpm = rpm;
            ESP_LOGI(TAG, "RPM %d ", rpm);
            pin_count=0;
            last_time = esp_timer_get_time();
          }
          ESP_LOGI(TAG, "pincout %d ", pin_count);
          last_pin_value = pin;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
void adc_read(void *pvParameters)
{
    while(true)
    {
        uint32_t adc_reading = 0;
        //Multisampling
        for (int i = 0; i < NO_OF_SAMPLES; i++) {
            adc_reading += adc1_get_raw((adc1_channel_t)channel);
        }
        adc_reading /= NO_OF_SAMPLES;
        //Convert adc_reading to voltage in mV
        uint32_t voltage = esp_adc_cal_raw_to_voltage(adc_reading, adc_chars);
        final_end = gpio_get_level(GPIO_END);
        final_pression = (100/4095) * adc_reading;
        ESP_LOGI(TAG, "Raw: %d\tVoltage: %dmV", adc_reading, voltage);
        ESP_LOGI(TAG, "Fim de curso %d", final_end);
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
esp_err_t setup_pheriperals()
{
    esp_err_t err = gpio_set_direction(GPIO_END, GPIO_MODE_INPUT);
    err = gpio_set_direction(GPIO_END, GPIO_MODE_INPUT);
    if(err != ESP_OK)
    {
          ESP_LOGI(TAG, "ERROR gpio_set_direction GPIO_END");
          return err;
    }
    err = gpio_set_direction(GPIO_RPM, GPIO_MODE_INPUT);
    if(err != ESP_OK)
    {
          ESP_LOGI(TAG, "ERROR gpio_set_direction GPIO_RPM");
          return err;
    }
    err = adc1_config_width(width);
    if(err != ESP_OK)
    {
        ESP_LOGI(TAG, "ERROR adc1_config_width ");
        return err;
    }
    err = adc1_config_channel_atten(channel, atten);
    if(err != ESP_OK)
    {
        ESP_LOGI(TAG, "ERROR adc1_config_channel_atten ");
        return err;
    }
     //Characterize ADC
    adc_chars = calloc(1, sizeof(esp_adc_cal_characteristics_t));
    esp_adc_cal_value_t val_type = esp_adc_cal_characterize(unit, atten, width, DEFAULT_VREF, adc_chars);
    print_char_val_type(val_type);
    return err;
}
void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    //adc check
    check_efuse();

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());
    ESP_LOGI(TAG, "Connected to AP, begin setup pheriperals");
    ESP_ERROR_CHECK(setup_pheriperals());
    ESP_LOGI(TAG, "Setup pheriperals ok, begin http / read pheripearals data");

    xTaskCreate(&http_rest_with_url, "http_test_task", 8192, NULL, 5, NULL);
    xTaskCreate(&pheriperals_handle, "pheriperals_handle_task", 8192, NULL, 5, NULL);
    xTaskCreate(&adc_read, "adc_task", 8192, NULL, 5, NULL);
}
    