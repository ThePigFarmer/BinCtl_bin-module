#include <stdio.h>


#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include <esp_http_server.h>
#include "nvs_flash.h"

#include <lwip/sockets.h>
#include <lwip/sys.h>
#include <lwip/api.h>

#include "portmacro.h"

#include "driver/gpio.h"
#include "cJSON.h"
#include <hx711.h>
#include <button.h> // every project needs one

// create the debug tag
static const char *TAG = "bin-module";


#define EXAMPLE_WIFI_SSID CONFIG_EXAMPLE_WIFI_SSID
#define EXAMPLE_WIFI_PASS CONFIG_EXAMPLE_WIFI_PASSWORD
#define EXAMPLE_MAXIMUM_RETRY CONFIG_EXAMPLE_MAXIMUM_RETRY
#define EXAMPLE_STATIC_IP_ADDR        CONFIG_EXAMPLE_STATIC_IP_ADDR
#define EXAMPLE_STATIC_NETMASK_ADDR   CONFIG_EXAMPLE_STATIC_NETMASK_ADDR
#define EXAMPLE_STATIC_GW_ADDR        CONFIG_EXAMPLE_STATIC_GW_ADDR
#ifdef CONFIG_EXAMPLE_STATIC_DNS_AUTO
#define EXAMPLE_MAIN_DNS_SERVER       EXAMPLE_STATIC_GW_ADDR
#define EXAMPLE_BACKUP_DNS_SERVER     "0.0.0.0"
#else
#define EXAMPLE_MAIN_DNS_SERVER       CONFIG_EXAMPLE_STATIC_DNS_SERVER_MAIN
#define EXAMPLE_BACKUP_DNS_SERVER     CONFIG_EXAMPLE_STATIC_DNS_SERVER_BACKUP
#endif
#ifdef CONFIG_EXAMPLE_STATIC_DNS_RESOLVE_TEST
#define EXAMPLE_RESOLVE_DOMAIN        CONFIG_EXAMPLE_STATIC_RESOLVE_DOMAIN
#endif

#define JSON_STRING_LENGTH 30 // json response length is easy and fun as long as we have it big enough

#define LED_PIN 2

#define HX711_AVG_TIMES 10
#define HX711_DOUT_GPIO 19
#define HX711_PD_SCK_GPIO 18

hx711_t hx711_dev = {.dout = HX711_DOUT_GPIO,
                         .pd_sck = HX711_PD_SCK_GPIO,
                         .gain = HX711_GAIN_A_64};

void blink_led(gpio_num_t pin, uint16_t on_delay)
{
   gpio_set_level(pin, 1);
   vTaskDelay(on_delay /portTICK_PERIOD_MS);
   gpio_set_level(pin, 0);
}


/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static int s_retry_num = 0;


static esp_err_t network_set_dns_server(esp_netif_t *netif, uint32_t addr, esp_netif_dns_type_t type)
{
    if (addr && (addr != IPADDR_NONE)) {
        esp_netif_dns_info_t dns;
        dns.ip.u_addr.ip4.addr = addr;
        dns.ip.type = IPADDR_TYPE_V4;
        ESP_ERROR_CHECK(esp_netif_set_dns_info(netif, type, &dns));
    }
    return ESP_OK;
}

static void network_set_static_ip(esp_netif_t *netif)
{
    if (esp_netif_dhcpc_stop(netif) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop dhcp client");
        return;
    }
    esp_netif_ip_info_t ip;
    memset(&ip, 0 , sizeof(esp_netif_ip_info_t));
    ip.ip.addr = ipaddr_addr(EXAMPLE_STATIC_IP_ADDR);
    ip.netmask.addr = ipaddr_addr(EXAMPLE_STATIC_NETMASK_ADDR);
    ip.gw.addr = ipaddr_addr(EXAMPLE_STATIC_GW_ADDR);
    if (esp_netif_set_ip_info(netif, &ip) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set ip info");
        return;
    }
    ESP_LOGD(TAG, "Success to set static ip: %s, netmask: %s, gw: %s", EXAMPLE_STATIC_IP_ADDR, EXAMPLE_STATIC_NETMASK_ADDR, EXAMPLE_STATIC_GW_ADDR);
    ESP_ERROR_CHECK(network_set_dns_server(netif, ipaddr_addr(EXAMPLE_MAIN_DNS_SERVER), ESP_NETIF_DNS_MAIN));
    ESP_ERROR_CHECK(network_set_dns_server(netif, ipaddr_addr(EXAMPLE_BACKUP_DNS_SERVER), ESP_NETIF_DNS_BACKUP));
}

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        network_set_static_ip(arg);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < EXAMPLE_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "static ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void network_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        sta_netif,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        sta_netif,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_WIFI_SSID,
            .password = EXAMPLE_WIFI_PASS,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
	     .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 EXAMPLE_WIFI_SSID, EXAMPLE_WIFI_PASS);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 EXAMPLE_WIFI_SSID, EXAMPLE_WIFI_PASS);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
#ifdef CONFIG_EXAMPLE_STATIC_DNS_RESOLVE_TEST
    struct addrinfo *address_info;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int res = getaddrinfo(EXAMPLE_RESOLVE_DOMAIN, NULL, &hints, &address_info);
    if (res != 0 || address_info == NULL) {
        ESP_LOGE(TAG, "couldn't get hostname for :%s: "
                      "getaddrinfo() returns %d, addrinfo=%p", EXAMPLE_RESOLVE_DOMAIN, res, address_info);
    } else {
        if (address_info->ai_family == AF_INET) {
            struct sockaddr_in *p = (struct sockaddr_in *)address_info->ai_addr;
            ESP_LOGI(TAG, "Resolved IPv4 address: %s", ipaddr_ntoa((const ip_addr_t*)&p->sin_addr.s_addr));
        }
#if CONFIG_LWIP_IPV6
        else if (address_info->ai_family == AF_INET6) {
            struct sockaddr_in6 *p = (struct sockaddr_in6 *)address_info->ai_addr;
            ESP_LOGI(TAG, "Resolved IPv6 address: %s", ip6addr_ntoa((const ip6_addr_t*)&p->sin6_addr));
        }
#endif
    }
#endif
    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);
}

uint32_t read_bin_weight(void) {
    // TODO redo this all

    esp_err_t r = hx711_wait(&hx711_dev, 500);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "HX711 Device not found: %d (%s)\n", r,
                 esp_err_to_name(r));
        return 0;
    }

    // get weight here
    int32_t data;
    r = hx711_read_average(&hx711_dev, HX711_AVG_TIMES, &data);

    // printf("weight: %u", weight);
    return data;
}

void parse_bin_weight_to_json(uint32_t weight, char *buffer, size_t size) {
    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        ESP_LOGE(TAG, "cJSON: Failed to create JSON object\n");
        return;
    }

    cJSON_AddNumberToObject(json, "weight", weight);

    char *json_string = cJSON_PrintUnformatted(json);

    // I doubt if this is necessary
    if (json_string == NULL) {
        ESP_LOGE(TAG, "cJSON: Failed to print JSON\n");
        cJSON_Delete(json);
        return;
    }

    // printf("JSON output:\n%s\n", json_string);

    strncpy(buffer, json_string, size);

    cJSON_Delete(json);
    free(json_string);
}

// handler for "/data"
esp_err_t data_handler(httpd_req_t *req)
{
    // then get it as json, and stick it in the response_buffer
    char response_buffer[JSON_STRING_LENGTH];
    parse_bin_weight_to_json(read_bin_weight(), response_buffer, sizeof(response_buffer));

    httpd_resp_set_type(req, "application/json");

    blink_led(LED_PIN, 100);

    return httpd_resp_send(req, response_buffer, HTTPD_RESP_USE_STRLEN);
}

httpd_uri_t uri_data = {
    .uri = "/data",
    .method = HTTP_GET,
    .handler = data_handler,
    .user_ctx = NULL
};

// handler for "/"
esp_err_t status_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "Webserver is working, send request to /data to get weight.\n", HTTPD_RESP_USE_STRLEN);
}

httpd_uri_t uri_status = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = status_handler,
    .user_ctx = NULL
};


httpd_handle_t setup_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK)
    {
        httpd_register_uri_handler(server, &uri_data);
        httpd_register_uri_handler(server, &uri_status);
    }

    return server;
}


void app_main(void)
{

    // init NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);


    // init wifi and network
    ESP_LOGI(TAG, "init network and wifi (sta)");
    network_init_sta();

    // init indicator led
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);

    // init hx711
    ESP_LOGI(TAG, "init hx711");
    ESP_ERROR_CHECK(hx711_init(&hx711_dev));

    // init webserver
    ESP_LOGI(TAG, "init webserver");
    setup_server();
}
