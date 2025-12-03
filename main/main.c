#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_sntp.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "driver/gpio.h"
#include "cJSON.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "esp_system.h"
#include "stdlib.h"

// Configuración de WiFi
#define WIFI_SSID "A54 de Erwin"
#define WIFI_PASSWORD "1234567000E"

#define MQTT_URI "mqtts://n2f66ba0.ala.us-east-1.emqxsl.com:8883"
#define MQTT_USERNAME "Erwin"
#define MQTT_PASSWORD "170804E"
#define MQTT_TOPIC "esp32/telemetria"

#define URL_SERVIDOR "https://telemetria-rest.onrender.com/api/telemetria/guardar-telemetria" // API del servidor BackEnd para guardar los datos
//#define URL_SERVIDOR "http://10.98.44.3:3100/api/telemetria/guardar-telemetria" // API del servidor BackEnd para guardar los datos
#define DHT_PIN GPIO_NUM_4
// #define FF 180000 // Intervalo de 3min entre lecturas

static const char *TAG = "ESP32";
extern const char ca_cert_pem[] asm("_binary_certificado_ca_pem_start"); // Certificado CA
extern const uint8_t emqx_ca_cert_pem_start[] asm("_binary_emqxsl_ca_pem_start");
extern const uint8_t emqx_ca_cert_pem_end[]   asm("_binary_emqxsl_ca_pem_end");

#define DHT_MAX_TIMINGS 85
#define DHT_TIMEOUT 1000

uint32_t intervalo_ms = 0; // variable dinámica
// Función para obtener intervalo aleatorio entre 10s y 60s
uint32_t generar_intervalo() {
    return (10 + rand() % 51) * 1000; 
}

bool dht22_leer(float *temperatura, float *humedad) {
    uint8_t data[5] = {0, 0, 0, 0, 0};
    uint8_t byte = 0, bit = 7;

    gpio_set_direction(DHT_PIN, GPIO_MODE_OUTPUT);

    // Señal de inicio
    gpio_set_level(DHT_PIN, 0);
    esp_rom_delay_us(1200); // 1.2ms
    gpio_set_level(DHT_PIN, 1);
    esp_rom_delay_us(30);

    gpio_set_direction(DHT_PIN, GPIO_MODE_INPUT);

    int timeout = 0;

    // Esperar a que el sensor responda (LOW)
    while (gpio_get_level(DHT_PIN) == 1) {
        if (++timeout > 200)
            return false;
        esp_rom_delay_us(1);
    }

    // Esperar HIGH
    timeout = 0;
    while (gpio_get_level(DHT_PIN) == 0) {
        if (++timeout > 200)
            return false;
        esp_rom_delay_us(1);
    }

    timeout = 0; // Esperar LOW
    while (gpio_get_level(DHT_PIN) == 1) {
        if (++timeout > 200)
            return false;
        esp_rom_delay_us(1);
    }

    // Leer 40 bits
    for (int i = 0; i < 40; i++) {
        timeout = 0; // LOW inicial
        while (gpio_get_level(DHT_PIN) == 0) {
            if (++timeout > 200)
                return false;
            esp_rom_delay_us(1);
        }
        // Medir duración del pulso HIGH
        uint32_t high_time = 0;
        timeout = 0;
        while (gpio_get_level(DHT_PIN) == 1) {
            high_time++;
            if (++timeout > 300)
                return false;
            esp_rom_delay_us(1);
        }

        // Interpretar bit
        if (high_time > 40) {
            data[byte] |= (1 << bit);
        }
        if (bit == 0) {
            bit = 7;
            byte++;
        } else {
            bit--;
        }
    }

    // Checksum
    uint8_t sum = data[0] + data[1] + data[2] + data[3];
    if (sum != data[4]) {
        ESP_LOGE("DHT22", "Checksum inválido");
        return false;
    }
    // Convertir valores
    int16_t raw_hum = (data[0] << 8) | data[1];
    int16_t raw_temp = (data[2] << 8) | data[3];

    if (raw_temp & 0x8000) {
        raw_temp = -(raw_temp & 0x7FFF);
    }
    *humedad = raw_hum / 10.0f;
    *temperatura = raw_temp / 10.0f;

    return true;
}

// Manejo del WiFi
static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WiFi desconectado, reconectando...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "WiFi conectado correctamente");
    }
}

// Funcion para manejar eventos MQTT
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI("MQTT", "Conectado al broker");
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI("MQTT", "Desconectado del broker");
            break;

        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI("MQTT", "Mensaje publicado");
            break;

        default:
            break;
    }
}

esp_mqtt_client_handle_t mqtt_client;

// Función para inicializar MQTT
void mqtt_init() {
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address.uri = MQTT_URI,
            .verification.certificate = (const char *)emqx_ca_cert_pem_start
        },
        .credentials = {
            .username = MQTT_USERNAME,
            .authentication.password = MQTT_PASSWORD,
        }
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);

    ESP_LOGI("MQTT", "Inicializando MQTT...");
}


// Función para enviar los datos del sensor al servidor
void enviar_datos() {
    float temperatura = 0;
    float humedad = 0;
    // Intentar lectura varias veces
    for (int intento = 0; intento < 3; intento++) {
        if (dht22_leer(&temperatura, &humedad)) {
            ESP_LOGI(TAG, "Lectura exitosa - Temperatura: %.2f °C, Humedad: %.2f %%", temperatura, humedad);
            break;
        } else {
            ESP_LOGW(TAG, "Intento %d fallido, reintentando...", intento + 1);
            vTaskDelay(pdMS_TO_TICKS(1000)); // Esperar 1 segundo entre intentos
        } if (intento == 2) {
            ESP_LOGE(TAG, "Todos los intentos fallaron");
            return;
        }
    }

    // Crear JSON con los datos del sensor que se envian al servidor
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "temperatura", temperatura);
    cJSON_AddNumberToObject(root, "humedad", humedad);

    cJSON *telemetria = cJSON_CreateObject();
    cJSON_AddNumberToObject(telemetria, "rssi", -50);
    cJSON_AddNumberToObject(telemetria, "voltaje", 3.3);
    cJSON_AddItemToObject(root, "telemetria", telemetria);

    time_t now = time(NULL);
    struct tm info;
    localtime_r(&now, &info); 

    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S%z", &info);
    cJSON_AddStringToObject(root, "timestamp", timestamp);


    char *json_final = cJSON_Print(root);
    ESP_LOGI(TAG, "JSON a enviar:\n%s", json_final);

    esp_http_client_config_t config = {
        .url = URL_SERVIDOR,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
        .cert_pem = ca_cert_pem,
    };
        // Publicar JSON a MQTT
    esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC, json_final, 0, 1, 0);
    ESP_LOGI("MQTT", "Publicado a MQTT: %s", json_final);

    esp_http_client_handle_t cliente = esp_http_client_init(&config);
    esp_http_client_set_header(cliente, "Content-Type", "application/json");
    esp_http_client_set_post_field(cliente, json_final, strlen(json_final));

    esp_err_t err = esp_http_client_perform(cliente);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Código HTTP recibido: %d", esp_http_client_get_status_code(cliente));
    }
    else
    {
        ESP_LOGE(TAG, "Error HTTP: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(cliente);
    cJSON_Delete(root);
    free(json_final);
}

// Función para sincronizar con el horario local México
void iniciar_ntp() {
    ESP_LOGI("NTP", "Iniciando SNTP...");

    // Zona horaria correcta para México (Centro)
    setenv("TZ", "CST6CDT,M4.1.0/2,M10.5.0/2", 1);
    tzset();

    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();

    time_t now = 0;
    struct tm timeinfo = {0};

    int intentos = 0;
    while (timeinfo.tm_year < (2016 - 1900) && intentos < 15) {
        time(&now);
        localtime_r(&now, &timeinfo);
        ESP_LOGI("NTP", "Esperando sincronización (%d)...", intentos);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        intentos++;
    }

    if (timeinfo.tm_year >= (2016 - 1900)) {
        ESP_LOGI("NTP", "Hora sincronizada correctamente");
    } else {
        ESP_LOGE("NTP", "No se pudo sincronizar la hora");
    }
}



void app_main()
{
    // Inicializar NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Inicializar la red a la que nos conectamos
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    // Configurar WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Registrar event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    // Configurar credenciales WiFi
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Conectando al WiFi...");

    // Esperar a que se establezca la conexión WiFi
    vTaskDelay(pdMS_TO_TICKS(3000));
    mqtt_init();
    iniciar_ntp();

    // Bucle principal
    while (1) {
    enviar_datos();
    // Generar nuevo intervalo
    intervalo_ms = generar_intervalo();
    ESP_LOGI(TAG, "Esperando %d ms antes de la siguiente lectura", intervalo_ms);
        vTaskDelay(pdMS_TO_TICKS(intervalo_ms));
    }
}