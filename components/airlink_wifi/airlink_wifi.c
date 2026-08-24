// SPDX-License-Identifier: Apache-2.0
#include "airlink_wifi.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include "airlink_core.h"
#include "airlink_mavlink.h"
#include "airlink_router.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define UDP_TIMEOUT_US INT64_C(30000000)
#define TCP_STALL_TIMEOUT_US INT64_C(10000000)
#define BRIDGE_CONNECT_TIMEOUT_US INT64_C(2000000)
#define NET_PACKET_QUEUE 64
#define NETWORK_TASK_PRIORITY 19
#define BRIDGE_TASK_PRIORITY 17
#define TCP_TX_BURST 8U

typedef struct {
    uint16_t length;
    uint8_t data[AIRLINK_MAX_FRAME_SIZE];
} net_packet_t;

typedef struct {
    bool used;
    uint8_t endpoint_id;
    struct sockaddr_in address;
    int64_t last_seen_us;
    uint8_t failures;
    airlink_mavlink_parser_t validator;
} udp_client_t;

typedef struct {
    bool used;
    uint8_t endpoint_id;
    int socket_fd;
    struct sockaddr_in address;
    QueueHandle_t tx_queue;
    net_packet_t pending;
    size_t pending_offset;
    int64_t pending_progress_us;
    bool has_pending;
} tcp_client_t;

static const char *TAG = "wifi";
static airlink_config_t s_config;
static airlink_wifi_status_t s_status;
static int s_udp_socket = -1;
static int s_tcp_listener = -1;
static udp_client_t s_udp[AIRLINK_MAX_UDP_CLIENTS];
static tcp_client_t s_tcp[AIRLINK_MAX_TCP_CLIENTS];
static esp_timer_handle_t s_reconnect_timer;
static uint32_t s_reconnect_attempts;
static int s_bridge_socket = -1;
static QueueHandle_t s_bridge_tx_queue;

static esp_err_t bridge_send(const uint8_t *data, size_t length,
                             bool high_priority, void *context)
{
    (void)high_priority;
    (void)context;
    if (!s_status.bridge_connected || s_bridge_tx_queue == NULL ||
        length > AIRLINK_MAX_FRAME_SIZE) return ESP_ERR_INVALID_STATE;
    net_packet_t packet = {.length = (uint16_t)length};
    memcpy(packet.data, data, length);
    return xQueueSend(s_bridge_tx_queue, &packet, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

static void bridge_disconnect(void)
{
    if (s_status.bridge_connected) {
        airlink_router_unregister(AIRLINK_ENDPOINT_ID_BRIDGE);
    }
    s_status.bridge_connected = false;
    if (s_bridge_socket >= 0) {
        shutdown(s_bridge_socket, SHUT_RDWR);
        close(s_bridge_socket);
        s_bridge_socket = -1;
    }
    if (s_bridge_tx_queue != NULL) xQueueReset(s_bridge_tx_queue);
}

static bool bridge_connect(void)
{
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) return false;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
    struct sockaddr_in peer = {
        .sin_family = AF_INET,
        .sin_port = htons(s_config.tcp_port),
        .sin_addr.s_addr = inet_addr("192.168.4.1"),
    };
    const int result = connect(fd, (struct sockaddr *)&peer, sizeof(peer));
    if (result != 0 && errno != EINPROGRESS) {
        close(fd);
        return false;
    }
    if (result != 0) {
        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(fd, &write_set);
        struct timeval timeout = {
            .tv_sec = (long)(BRIDGE_CONNECT_TIMEOUT_US / INT64_C(1000000)),
            .tv_usec = (long)(BRIDGE_CONNECT_TIMEOUT_US % INT64_C(1000000)),
        };
        if (select(fd + 1, NULL, &write_set, NULL, &timeout) <= 0) {
            close(fd);
            return false;
        }
        int socket_error = 0;
        socklen_t error_length = sizeof(socket_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_length) != 0 ||
            socket_error != 0) {
            close(fd);
            return false;
        }
    }
    s_bridge_socket = fd;
    const airlink_router_endpoint_t endpoint = {
        .id = AIRLINK_ENDPOINT_ID_BRIDGE,
        .type = AIRLINK_ENDPOINT_BRIDGE,
        .send = bridge_send,
        .name = "airlink-bridge",
    };
    if (airlink_router_register(&endpoint) != ESP_OK) {
        close(fd);
        s_bridge_socket = -1;
        return false;
    }
    s_status.bridge_connected = true;
    ESP_LOGI(TAG, "bridge connected to 192.168.4.1:%u", s_config.tcp_port);
    return true;
}

static void recount_clients(void)
{
    uint8_t udp = 0, tcp = 0;
    for (size_t i = 0; i < AIRLINK_MAX_UDP_CLIENTS; ++i) if (s_udp[i].used) udp++;
    for (size_t i = 0; i < AIRLINK_MAX_TCP_CLIENTS; ++i) if (s_tcp[i].used) tcp++;
    s_status.udp_clients = udp;
    s_status.tcp_clients = tcp;
}

static esp_err_t udp_send(const uint8_t *data, size_t length, bool high_priority, void *context)
{
    (void)high_priority;
    udp_client_t *client = context;
    if (client == NULL || !client->used || s_udp_socket < 0) return ESP_ERR_INVALID_STATE;
    const ssize_t sent = sendto(s_udp_socket, data, length, MSG_DONTWAIT,
                                (const struct sockaddr *)&client->address, sizeof(client->address));
    if (sent == (ssize_t)length) { client->failures = 0; return ESP_OK; }
    if (++client->failures >= 3) return ESP_ERR_INVALID_STATE;
    return ESP_FAIL;
}

static esp_err_t udp_broadcast_send(const uint8_t *data, size_t length,
                                    bool high_priority, void *context)
{
    (void)high_priority;
    (void)context;
    if (s_status.udp_clients != 0 || s_udp_socket < 0 || !s_status.ap_started) return ESP_OK;
    struct sockaddr_in broadcast = {
        .sin_family = AF_INET,
        .sin_port = htons(s_config.udp_port),
        .sin_addr.s_addr = inet_addr("192.168.4.255"),
    };
    return sendto(s_udp_socket, data, length, MSG_DONTWAIT,
                  (struct sockaddr *)&broadcast, sizeof(broadcast)) == (ssize_t)length ? ESP_OK : ESP_FAIL;
}

static esp_err_t tcp_send(const uint8_t *data, size_t length, bool high_priority, void *context)
{
    (void)high_priority;
    tcp_client_t *client = context;
    if (client == NULL || !client->used || length > AIRLINK_MAX_FRAME_SIZE) return ESP_ERR_INVALID_STATE;
    net_packet_t packet = {.length = (uint16_t)length};
    memcpy(packet.data, data, length);
    return xQueueSend(client->tx_queue, &packet, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool datagram_has_acceptable_mavlink(const uint8_t *data, size_t length)
{
    airlink_mavlink_parser_t parser = {0};
    airlink_mavlink_frame_t frame;
    for (size_t i = 0; i < length; ++i) {
        if (airlink_mavlink_parse_byte(&parser, data[i], &frame) &&
            (!frame.crc_known || frame.crc_valid)) return true;
    }
    return false;
}

static udp_client_t *find_udp(const struct sockaddr_in *address)
{
    for (size_t i = 0; i < AIRLINK_MAX_UDP_CLIENTS; ++i) {
        if (s_udp[i].used && s_udp[i].address.sin_addr.s_addr == address->sin_addr.s_addr &&
            s_udp[i].address.sin_port == address->sin_port) return &s_udp[i];
    }
    return NULL;
}

static udp_client_t *register_udp(const struct sockaddr_in *address)
{
    udp_client_t *client = find_udp(address);
    if (client != NULL) return client;
    for (size_t i = 0; i < AIRLINK_MAX_UDP_CLIENTS; ++i) {
        if (s_udp[i].used) continue;
        client = &s_udp[i];
        *client = (udp_client_t){
            .used = true,
            .endpoint_id = AIRLINK_ENDPOINT_ID_UDP_BASE + (uint8_t)i,
            .address = *address,
            .last_seen_us = esp_timer_get_time(),
        };
        const airlink_router_endpoint_t endpoint = {
            .id = client->endpoint_id, .type = AIRLINK_ENDPOINT_UDP,
            .send = udp_send, .context = client, .name = "udp-client",
        };
        if (airlink_router_register(&endpoint) != ESP_OK) { client->used = false; return NULL; }
        recount_clients();
        return client;
    }
    return NULL;
}

static void expire_udp(void)
{
    const int64_t now = esp_timer_get_time();
    for (size_t i = 0; i < AIRLINK_MAX_UDP_CLIENTS; ++i) {
        if (!s_udp[i].used) continue;
        if (now - s_udp[i].last_seen_us > UDP_TIMEOUT_US || s_udp[i].failures >= 3) {
            airlink_router_unregister(s_udp[i].endpoint_id);
            s_udp[i].used = false;
        }
    }
    recount_clients();
}

static void close_tcp(tcp_client_t *client)
{
    if (!client->used) return;
    airlink_router_unregister(client->endpoint_id);
    shutdown(client->socket_fd, SHUT_RDWR);
    close(client->socket_fd);
    if (client->tx_queue != NULL) vQueueDelete(client->tx_queue);
    *client = (tcp_client_t){0};
    recount_clients();
}

static void accept_tcp(void)
{
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    const int fd = accept(s_tcp_listener, (struct sockaddr *)&address, &length);
    if (fd < 0) return;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int keepalive = 1, idle = 15, interval = 5, count = 3;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
    /* A USB reset on the ground unit can drop Wi-Fi without a TCP FIN.  DHCP
     * assigns the reconnecting station the same AP-local address, so replace
     * its stale socket immediately instead of consuming both client slots
     * until keepalive expires. */
    for (size_t i = 0; i < AIRLINK_MAX_TCP_CLIENTS; ++i) {
        if (s_tcp[i].used &&
            s_tcp[i].address.sin_addr.s_addr == address.sin_addr.s_addr) {
            ESP_LOGI(TAG, "replacing stale TCP client from reconnecting station");
            close_tcp(&s_tcp[i]);
        }
    }
    for (size_t i = 0; i < AIRLINK_MAX_TCP_CLIENTS; ++i) {
        if (s_tcp[i].used) continue;
        tcp_client_t *client = &s_tcp[i];
        *client = (tcp_client_t){
            .used = true, .endpoint_id = AIRLINK_ENDPOINT_ID_TCP_BASE + (uint8_t)i,
            .socket_fd = fd, .address = address,
            .tx_queue = xQueueCreate(NET_PACKET_QUEUE, sizeof(net_packet_t)),
        };
        if (client->tx_queue == NULL) { close(fd); client->used = false; return; }
        const airlink_router_endpoint_t endpoint = {
            .id = client->endpoint_id, .type = AIRLINK_ENDPOINT_TCP,
            .send = tcp_send, .context = client, .name = "tcp-client",
        };
        if (airlink_router_register(&endpoint) != ESP_OK) { close_tcp(client); return; }
        recount_clients();
        return;
    }
    close(fd);
}

static esp_err_t telemetry_bind_address(in_addr_t *address)
{
    if (address == NULL) return ESP_ERR_INVALID_ARG;
    /* AP+STA deliberately exposes telemetry only on the private AP.  STA-only
     * has a single network interface, so INADDR_ANY remains valid across DHCP
     * address changes and reconnects. */
    *address = s_config.wifi_mode == AIRLINK_WIFI_STA ? htonl(INADDR_ANY) :
                                                       inet_addr("192.168.4.1");
    return ESP_OK;
}

static esp_err_t open_sockets(void)
{
    in_addr_t bind_address;
    ESP_RETURN_ON_ERROR(telemetry_bind_address(&bind_address), TAG, "telemetry bind address");
    s_udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_udp_socket < 0) return ESP_FAIL;
    int yes = 1;
    setsockopt(s_udp_socket, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    fcntl(s_udp_socket, F_SETFL, fcntl(s_udp_socket, F_GETFL, 0) | O_NONBLOCK);
    struct sockaddr_in udp_address = {
        .sin_family = AF_INET, .sin_port = htons(s_config.udp_port),
        .sin_addr.s_addr = bind_address,
    };
    if (bind(s_udp_socket, (struct sockaddr *)&udp_address, sizeof(udp_address)) != 0) goto fail;

    s_tcp_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s_tcp_listener < 0) goto fail;
    setsockopt(s_tcp_listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    fcntl(s_tcp_listener, F_SETFL, fcntl(s_tcp_listener, F_GETFL, 0) | O_NONBLOCK);
    struct sockaddr_in tcp_address = {
        .sin_family = AF_INET, .sin_port = htons(s_config.tcp_port),
        .sin_addr.s_addr = bind_address,
    };
    if (bind(s_tcp_listener, (struct sockaddr *)&tcp_address, sizeof(tcp_address)) != 0 ||
        listen(s_tcp_listener, AIRLINK_MAX_TCP_CLIENTS) != 0) goto fail;

    const airlink_router_endpoint_t broadcast = {
        .id = AIRLINK_ENDPOINT_ID_UDP_BASE - 1U, .type = AIRLINK_ENDPOINT_UDP,
        .send = udp_broadcast_send, .name = "udp-ap-broadcast",
    };
    const esp_err_t err = airlink_router_register(&broadcast);
    if (err == ESP_OK) return ESP_OK;
fail:
    if (s_tcp_listener >= 0) { close(s_tcp_listener); s_tcp_listener = -1; }
    if (s_udp_socket >= 0) { close(s_udp_socket); s_udp_socket = -1; }
    return ESP_FAIL;
}

static void service_tcp_tx(tcp_client_t *client)
{
    for (unsigned burst = 0; burst < TCP_TX_BURST && client->used; ++burst) {
        if (!client->has_pending &&
            xQueueReceive(client->tx_queue, &client->pending, 0) == pdTRUE) {
            client->pending_offset = 0;
            client->pending_progress_us = esp_timer_get_time();
            client->has_pending = true;
        }
        if (!client->has_pending) return;

        const size_t remaining = client->pending.length - client->pending_offset;
        const ssize_t sent = send(client->socket_fd,
                                  client->pending.data + client->pending_offset,
                                  remaining, MSG_DONTWAIT);
        if (sent > 0) {
            client->pending_offset += (size_t)sent;
            client->pending_progress_us = esp_timer_get_time();
            if (client->pending_offset == client->pending.length) {
                client->has_pending = false;
                continue;
            }
            continue;
        }
        if (sent == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            close_tcp(client);
            return;
        }
        if (esp_timer_get_time() - client->pending_progress_us > TCP_STALL_TIMEOUT_US) {
            ESP_LOGW(TAG, "closing stalled TCP client");
            close_tcp(client);
        }
        return;
    }
}

static void network_task(void *argument)
{
    (void)argument;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    uint8_t rx[1024];
    while (true) {
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        struct sockaddr_in source;
        socklen_t source_length = sizeof(source);
        const ssize_t received = recvfrom(s_udp_socket, rx, sizeof(rx), MSG_DONTWAIT,
                                          (struct sockaddr *)&source, &source_length);
        if (received > 0) {
            udp_client_t *client = find_udp(&source);
            const bool accepted = client != NULL || s_config.route_mode == AIRLINK_ROUTE_TRANSPARENT ||
                                  datagram_has_acceptable_mavlink(rx, (size_t)received);
            if (client == NULL && accepted) client = register_udp(&source);
            if (client != NULL && accepted) {
                client->last_seen_us = esp_timer_get_time();
                airlink_router_ingest(client->endpoint_id, rx, (size_t)received);
            }
        }
        accept_tcp();
        for (size_t i = 0; i < AIRLINK_MAX_TCP_CLIENTS; ++i) {
            tcp_client_t *client = &s_tcp[i];
            if (!client->used) continue;
            const ssize_t tcp_received = recv(client->socket_fd, rx, sizeof(rx), MSG_DONTWAIT);
            if (tcp_received > 0) {
                airlink_router_ingest(client->endpoint_id, rx, (size_t)tcp_received);
            } else if (tcp_received == 0) {
                close_tcp(client);
                continue;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                close_tcp(client);
                continue;
            }
            service_tcp_tx(client);
        }
        expire_udp();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

static void bridge_task(void *argument)
{
    (void)argument;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    uint8_t rx[1024];
    net_packet_t pending = {0};
    size_t pending_offset = 0;
    int64_t pending_progress_us = 0;
    bool has_pending = false;
    while (true) {
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        if (!s_status.sta_connected) {
            bridge_disconnect();
            has_pending = false;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (s_bridge_socket < 0) {
            if (!bridge_connect()) {
                s_status.bridge_reconnects++;
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            has_pending = false;
        }

        const ssize_t received = recv(s_bridge_socket, rx, sizeof(rx), MSG_DONTWAIT);
        if (received > 0) {
            airlink_router_ingest(AIRLINK_ENDPOINT_ID_BRIDGE, rx, (size_t)received);
        } else if (received == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            ESP_LOGW(TAG, "bridge disconnected");
            bridge_disconnect();
            s_status.bridge_reconnects++;
            has_pending = false;
            continue;
        }

        if (!has_pending && xQueueReceive(s_bridge_tx_queue, &pending, 0) == pdTRUE) {
            pending_offset = 0;
            pending_progress_us = esp_timer_get_time();
            has_pending = true;
        }
        if (has_pending) {
            const size_t remaining = pending.length - pending_offset;
            const ssize_t sent = send(s_bridge_socket, pending.data + pending_offset,
                                      remaining, MSG_DONTWAIT);
            if (sent > 0) {
                pending_offset += (size_t)sent;
                pending_progress_us = esp_timer_get_time();
                if (pending_offset == pending.length) has_pending = false;
            } else if (sent == 0 || (errno != EAGAIN && errno != EWOULDBLOCK) ||
                       esp_timer_get_time() - pending_progress_us > TCP_STALL_TIMEOUT_US) {
                ESP_LOGW(TAG, "bridge transmit stalled");
                bridge_disconnect();
                s_status.bridge_reconnects++;
                has_pending = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

static void reconnect_timer(void *argument)
{
    (void)argument;
    esp_wifi_connect();
}

static void wifi_event(void *argument, esp_event_base_t base, int32_t id, void *data)
{
    (void)argument; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) s_status.ap_started = true;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_status.sta_connected = false;
        s_status.reconnects++;
        const uint32_t shift = s_reconnect_attempts > 5 ? 5 : s_reconnect_attempts++;
        const uint64_t delay_us = (uint64_t)(250U << shift) * 1000U;
        if (esp_timer_is_active(s_reconnect_timer)) esp_timer_stop(s_reconnect_timer);
        esp_timer_start_once(s_reconnect_timer, delay_us);
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_status.sta_connected = true;
        s_status.reconnects = 0;
        s_reconnect_attempts = 0;
        if (esp_timer_is_active(s_reconnect_timer)) esp_timer_stop(s_reconnect_timer);
    }
}

static void configure_ap_netif(esp_netif_t *netif)
{
    esp_netif_ip_info_t info;
    IP4_ADDR(&info.ip, 192, 168, 4, 1);
    IP4_ADDR(&info.gw, 192, 168, 4, 1);
    IP4_ADDR(&info.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(netif);
    esp_netif_set_ip_info(netif, &info);
    esp_netif_dhcps_start(netif);
}

esp_err_t airlink_wifi_start(const airlink_config_t *config)
{
    if (config == NULL) return ESP_ERR_INVALID_ARG;
    s_config = *config;
    const esp_timer_create_args_t timer_args = {
        .callback = reconnect_timer,
        .name = "wifi_retry",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_reconnect_timer), TAG, "retry timer");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    esp_netif_t *ap_netif = NULL;
    if (config->wifi_mode != AIRLINK_WIFI_STA) {
        ap_netif = esp_netif_create_default_wifi_ap();
        if (ap_netif == NULL) return ESP_ERR_NO_MEM;
        configure_ap_netif(ap_netif);
    }
    if (config->wifi_mode != AIRLINK_WIFI_AP && esp_netif_create_default_wifi_sta() == NULL) {
        return ESP_ERR_NO_MEM;
    }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "Wi-Fi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL), TAG, "Wi-Fi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL), TAG, "IP events");

    wifi_mode_t mode = config->wifi_mode == AIRLINK_WIFI_AP ? WIFI_MODE_AP :
                       config->wifi_mode == AIRLINK_WIFI_STA ? WIFI_MODE_STA : WIFI_MODE_APSTA;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(mode), TAG, "Wi-Fi mode");
    wifi_band_mode_t band = config->wifi_band == AIRLINK_WIFI_BAND_2G ? WIFI_BAND_MODE_2G_ONLY :
                            config->wifi_band == AIRLINK_WIFI_BAND_5G ? WIFI_BAND_MODE_5G_ONLY : WIFI_BAND_MODE_AUTO;
    if (config->wifi_mode != AIRLINK_WIFI_STA) {
        wifi_config_t ap = {0};
        const size_t ssid_length = strlen(config->ap_ssid);
        memcpy(ap.ap.ssid, config->ap_ssid, ssid_length);
        strlcpy((char *)ap.ap.password, config->ap_password, sizeof(ap.ap.password));
        ap.ap.ssid_len = ssid_length;
        ap.ap.channel = config->wifi_band == AIRLINK_WIFI_BAND_5G ? 36 : 6;
        ap.ap.max_connection = 8;
        ap.ap.authmode = WIFI_AUTH_WPA2_WPA3_PSK;
        ap.ap.pmf_cfg.required = false;
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "AP config");
    }
    if (config->wifi_mode != AIRLINK_WIFI_AP) {
        wifi_config_t sta = {0};
        memcpy(sta.sta.ssid, config->sta_ssid, strlen(config->sta_ssid));
        strlcpy((char *)sta.sta.password, config->sta_password, sizeof(sta.sta.password));
        sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta), TAG, "STA config");
    }
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start");
    /* ESP-IDF requires the driver to be started before selecting the active
     * band; calling this earlier returns ESP_ERR_WIFI_NOT_STARTED. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_band_mode(band), TAG, "Wi-Fi band");
    if (config->wifi_mode != AIRLINK_WIFI_AP) esp_wifi_connect();
    if (config->bridge_enabled && config->bridge_role == AIRLINK_BRIDGE_GROUND) {
        s_bridge_tx_queue = xQueueCreate(NET_PACKET_QUEUE, sizeof(net_packet_t));
        if (s_bridge_tx_queue == NULL) return ESP_ERR_NO_MEM;
        return xTaskCreate(bridge_task, "airlink_bridge", 6144, NULL,
                           BRIDGE_TASK_PRIORITY, NULL) == pdPASS ?
               ESP_OK : ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(open_sockets(), TAG, "telemetry sockets");
    return xTaskCreate(network_task, "telemetry_net", 6144, NULL,
                       NETWORK_TASK_PRIORITY, NULL) == pdPASS ?
           ESP_OK : ESP_ERR_NO_MEM;
}

void airlink_wifi_get_status(airlink_wifi_status_t *status)
{
    if (status == NULL) return;
    wifi_ap_record_t record;
    if (s_status.sta_connected && esp_wifi_sta_get_ap_info(&record) == ESP_OK) {
        s_status.rssi = record.rssi;
        s_status.channel = record.primary;
    }
    *status = s_status;
    if (s_config.bridge_enabled && s_config.bridge_role == AIRLINK_BRIDGE_AIR) {
        status->bridge_connected = status->tcp_clients > 0;
    }
}

size_t airlink_wifi_clients_json(char *output, size_t capacity)
{
    if (output == NULL || capacity == 0) return 0;
    size_t used = (size_t)snprintf(output, capacity, "{\"udp\":[");
    for (size_t i = 0; i < AIRLINK_MAX_UDP_CLIENTS && used < capacity; ++i) {
        if (!s_udp[i].used) continue;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &s_udp[i].address.sin_addr, ip, sizeof(ip));
        used += (size_t)snprintf(output + used, capacity - used, "%s{\"ip\":\"%s\",\"port\":%u}",
                                 used > strlen("{\"udp\":[") ? "," : "", ip, ntohs(s_udp[i].address.sin_port));
    }
    if (used < capacity) used += (size_t)snprintf(output + used, capacity - used, "],\"tcp_count\":%u}", s_status.tcp_clients);
    return used < capacity ? used : capacity - 1U;
}
