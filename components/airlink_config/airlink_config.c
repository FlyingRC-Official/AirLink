// SPDX-License-Identifier: Apache-2.0
#include "airlink_config.h"

#include <inttypes.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"

typedef struct {
    uint32_t magic;
    uint32_t generation;
    airlink_config_t config;
    uint32_t crc32;
} config_record_t;

typedef struct {
    uint32_t magic;
    char serial_number[AIRLINK_SERIAL_MAX + 1];
    char initial_password[AIRLINK_PASSWORD_MAX + 1];
    uint32_t crc32;
} factory_identity_t;

#define CONFIG_MAGIC UINT32_C(0x414c4346)
#define IDENTITY_MAGIC UINT32_C(0x414c4944)
static const char *TAG = "config";
static const char *NVS_NAMESPACE = "airlink";
static airlink_config_t s_config;
static uint32_t s_generation;

static bool baud_valid(uint32_t baud)
{
    static const uint32_t allowed[] = {57600, 115200, 230400, 460800, 921600};
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i) {
        if (baud == allowed[i]) return true;
    }
    return false;
}

static bool can_bitrate_valid(uint32_t bitrate)
{
    return bitrate == 125000 || bitrate == 250000 ||
           bitrate == 500000 || bitrate == 1000000;
}

static bool serial_valid(const char *serial)
{
    if (serial == NULL) return false;
    const size_t length = strlen(serial);
    if (length == 0 || length > AIRLINK_SERIAL_MAX) return false;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = (unsigned char)serial[i];
        if (!isalnum(c) && c != '-' && c != '_' && c != '.') return false;
    }
    return true;
}

bool airlink_config_validate(const airlink_config_t *config)
{
    if (config == NULL || config->schema_version != AIRLINK_CONFIG_SCHEMA_VERSION) return false;
    if (config->route_mode > AIRLINK_ROUTE_TRANSPARENT || !baud_valid(config->uart_baud)) return false;
    if (config->wifi_mode > AIRLINK_WIFI_APSTA || config->wifi_band > AIRLINK_WIFI_BAND_5G) return false;
    if (config->usb_mode > AIRLINK_USB_MAVLINK || !can_bitrate_valid(config->can_bitrate)) return false;
    if (config->udp_port == 0 || config->tcp_port == 0 || config->led_brightness > 100) return false;
    const size_t ap_ssid_len = strnlen(config->ap_ssid, sizeof(config->ap_ssid));
    const size_t sta_ssid_len = strnlen(config->sta_ssid, sizeof(config->sta_ssid));
    if (ap_ssid_len == 0 || ap_ssid_len >= sizeof(config->ap_ssid) ||
        sta_ssid_len >= sizeof(config->sta_ssid)) return false;
    const size_t ap_password_len = strnlen(config->ap_password, sizeof(config->ap_password));
    const size_t sta_password_len = strnlen(config->sta_password, sizeof(config->sta_password));
    const size_t admin_password_len = strnlen(config->admin_password, sizeof(config->admin_password));
    if (ap_password_len < 12 || ap_password_len > 63 ||
        sta_password_len >= sizeof(config->sta_password) ||
        (sta_password_len != 0 && (sta_password_len < 8 || sta_password_len > 63)) ||
        admin_password_len < 12 || admin_password_len >= sizeof(config->admin_password)) return false;
    if ((config->wifi_mode == AIRLINK_WIFI_STA || config->wifi_mode == AIRLINK_WIFI_APSTA) &&
        sta_ssid_len == 0) return false;
    return true;
}

static uint32_t identity_crc(const factory_identity_t *identity)
{
    return airlink_crc32(identity, offsetof(factory_identity_t, crc32));
}

static bool read_identity(factory_identity_t *identity)
{
    nvs_handle_t nvs;
    if (nvs_open_from_partition("identity", "factory", NVS_READONLY, &nvs) != ESP_OK) return false;
    size_t length = sizeof(*identity);
    const esp_err_t err = nvs_get_blob(nvs, "identity", identity, &length);
    nvs_close(nvs);
    return err == ESP_OK && length == sizeof(*identity) && identity->magic == IDENTITY_MAGIC &&
           identity->crc32 == identity_crc(identity) && identity->serial_number[0] != '\0' &&
           strnlen(identity->initial_password, sizeof(identity->initial_password)) >= 12;
}

static esp_err_t write_identity(const char *serial, const char *password)
{
    factory_identity_t identity = {.magic = IDENTITY_MAGIC};
    strlcpy(identity.serial_number, serial, sizeof(identity.serial_number));
    strlcpy(identity.initial_password, password, sizeof(identity.initial_password));
    identity.crc32 = identity_crc(&identity);
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open_from_partition("identity", "factory", NVS_READWRITE, &nvs), TAG,
                        "open identity partition");
    esp_err_t err = nvs_set_blob(nvs, "identity", &identity, sizeof(identity));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static void random_password(char output[17])
{
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    for (size_t i = 0; i < 16; ++i) {
        output[i] = alphabet[esp_random() % (sizeof(alphabet) - 1U)];
    }
    output[16] = '\0';
}

static void load_defaults(airlink_config_t *config)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    *config = (airlink_config_t){
        .schema_version = AIRLINK_CONFIG_SCHEMA_VERSION,
        .route_mode = AIRLINK_ROUTE_MAVLINK,
        .uart_baud = 115200,
        .wifi_mode = AIRLINK_WIFI_AP,
        .wifi_band = AIRLINK_WIFI_BAND_AUTO,
        .udp_port = 14550,
        .tcp_port = 5760,
        .usb_mode = AIRLINK_USB_LOG_CLI,
        .can_bitrate = 1000000,
        .led_brightness = 25,
    };
    snprintf(config->ap_ssid, sizeof(config->ap_ssid), "FlyingRC-AirLink-%02X%02X", mac[4], mac[5]);
    random_password(config->ap_password);
    memcpy(config->admin_password, config->ap_password, sizeof(config->admin_password));
    factory_identity_t identity;
    if (read_identity(&identity)) {
        strlcpy(config->serial_number, identity.serial_number, sizeof(config->serial_number));
        strlcpy(config->ap_password, identity.initial_password, sizeof(config->ap_password));
        strlcpy(config->admin_password, identity.initial_password, sizeof(config->admin_password));
    }
}

static uint32_t record_crc(const config_record_t *record)
{
    return airlink_crc32(record, offsetof(config_record_t, crc32));
}

static bool record_valid(const config_record_t *record, size_t length)
{
    return length == sizeof(*record) && record->magic == CONFIG_MAGIC &&
           record->crc32 == record_crc(record) && airlink_config_validate(&record->config);
}

static esp_err_t read_slot(nvs_handle_t nvs, const char *key, config_record_t *record, bool *valid)
{
    size_t length = sizeof(*record);
    const esp_err_t err = nvs_get_blob(nvs, key, record, &length);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *valid = false;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "slot %s unreadable: %s", key, esp_err_to_name(err));
        *valid = false;
        return ESP_OK;
    }
    *valid = record_valid(record, length);
    return ESP_OK;
}

static esp_err_t write_record(const airlink_config_t *config, uint32_t generation)
{
    config_record_t record = {
        .magic = CONFIG_MAGIC,
        .generation = generation,
        .config = *config,
    };
    record.crc32 = record_crc(&record);
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "open NVS");
    const char *key = (generation & 1U) ? "config_b" : "config_a";
    esp_err_t err = nvs_set_blob(nvs, key, &record, sizeof(record));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t airlink_config_init(airlink_config_snapshot_t *snapshot)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase damaged NVS");
        ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "NVS init");
    ret = nvs_flash_init_partition("identity");
    ESP_RETURN_ON_ERROR(ret, TAG, "identity NVS init");

    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs), TAG, "open NVS");
    config_record_t a = {0}, b = {0};
    bool a_valid = false, b_valid = false;
    ESP_GOTO_ON_ERROR(read_slot(nvs, "config_a", &a, &a_valid), out, TAG, "read slot A");
    ESP_GOTO_ON_ERROR(read_slot(nvs, "config_b", &b, &b_valid), out, TAG, "read slot B");
out:
    nvs_close(nvs);
    ESP_RETURN_ON_ERROR(ret, TAG, "load configuration");

    bool defaults = false;
    if (a_valid || b_valid) {
        const config_record_t *selected = !b_valid || (a_valid && a.generation >= b.generation) ? &a : &b;
        s_config = selected->config;
        s_generation = selected->generation;
    } else {
        load_defaults(&s_config);
        s_generation = 1;
        defaults = true;
        ESP_RETURN_ON_ERROR(write_record(&s_config, s_generation), TAG, "store defaults");
    }
    if (snapshot != NULL) {
        *snapshot = (airlink_config_snapshot_t){s_config, s_generation, defaults};
    }
    ESP_LOGI(TAG, "loaded generation=%" PRIu32 " defaults=%d", s_generation, defaults);
    return ESP_OK;
}

const airlink_config_t *airlink_config_get(void) { return &s_config; }
uint32_t airlink_config_generation(void) { return s_generation; }

esp_err_t airlink_config_save(const airlink_config_t *config)
{
    if (!airlink_config_validate(config)) return ESP_ERR_INVALID_ARG;
    const uint32_t generation = s_generation + 1U;
    ESP_RETURN_ON_ERROR(write_record(config, generation), TAG, "write configuration");
    s_config = *config;
    s_generation = generation;
    return ESP_OK;
}

esp_err_t airlink_config_factory_reset(void)
{
    airlink_config_t defaults;
    load_defaults(&defaults);
    return airlink_config_save(&defaults);
}

esp_err_t airlink_config_set_identity(const char *serial, const char *password)
{
    if (!serial_valid(serial) || password == NULL ||
        strlen(password) < 12 || strlen(password) > 63) {
        return ESP_ERR_INVALID_ARG;
    }
    factory_identity_t existing;
    if (read_identity(&existing) &&
        (strcmp(existing.serial_number, serial) != 0 || strcmp(existing.initial_password, password) != 0)) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(write_identity(serial, password), TAG, "store factory identity");
    airlink_config_t updated = s_config;
    strlcpy(updated.serial_number, serial, sizeof(updated.serial_number));
    strlcpy(updated.ap_password, password, sizeof(updated.ap_password));
    strlcpy(updated.admin_password, password, sizeof(updated.admin_password));
    return airlink_config_save(&updated);
}
