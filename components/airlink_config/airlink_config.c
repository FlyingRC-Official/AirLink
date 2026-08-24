// SPDX-License-Identifier: Apache-2.0
#include "airlink_config.h"

#include <inttypes.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "airlink_provision.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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

_Static_assert(sizeof(airlink_bridge_role_t) == sizeof(uint32_t),
               "bridge role must preserve the schema-v1 reserved field layout");

#define CONFIG_MAGIC UINT32_C(0x414c4346)
#define IDENTITY_MAGIC UINT32_C(0x414c4944)
static const char *TAG = "config";
static const char *NVS_NAMESPACE = "airlink";
static airlink_config_t s_config;
static uint32_t s_generation;
static SemaphoreHandle_t s_config_lock;

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

static bool identity_password_valid(const char *password, size_t length)
{
    if (password == NULL || length < 12U || length > 63U) return false;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = (unsigned char)password[i];
        if (c < 0x21U || c > 0x7eU) return false;
    }
    return password[length] == '\0';
}

bool airlink_config_validate(const airlink_config_t *config)
{
    if (config == NULL || config->schema_version != AIRLINK_CONFIG_SCHEMA_VERSION) return false;
    if (config->route_mode > AIRLINK_ROUTE_TRANSPARENT || !baud_valid(config->uart_baud)) return false;
    if (config->wifi_mode > AIRLINK_WIFI_APSTA || config->wifi_band > AIRLINK_WIFI_BAND_5G) return false;
    if (config->usb_mode > AIRLINK_USB_MAVLINK || !can_bitrate_valid(config->can_bitrate)) return false;
    if (config->bridge_role > AIRLINK_BRIDGE_GROUND ||
        config->bridge_enabled != (config->bridge_role != AIRLINK_BRIDGE_OFF)) return false;
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
    if (config->bridge_role == AIRLINK_BRIDGE_AIR && config->wifi_mode != AIRLINK_WIFI_AP) return false;
    if (config->bridge_role == AIRLINK_BRIDGE_GROUND &&
        (config->wifi_mode != AIRLINK_WIFI_STA || config->usb_mode != AIRLINK_USB_MAVLINK)) return false;
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
    if (err != ESP_OK || length != sizeof(*identity) || identity->magic != IDENTITY_MAGIC ||
        identity->crc32 != identity_crc(identity)) return false;
    const size_t serial_length = strnlen(identity->serial_number, sizeof(identity->serial_number));
    const size_t password_length = strnlen(identity->initial_password, sizeof(identity->initial_password));
    return serial_length > 0 && serial_length < sizeof(identity->serial_number) &&
           identity_password_valid(identity->initial_password, password_length) &&
           serial_valid(identity->serial_number);
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
        .bridge_enabled = false,
        .bridge_role = AIRLINK_BRIDGE_OFF,
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

static bool provisioning_record_read(airlink_provision_record_t *record)
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, AIRLINK_PROVISION_PARTITION_LABEL);
    if (partition == NULL || partition->size < sizeof(*record) ||
        esp_partition_read(partition, 0, record, sizeof(*record)) != ESP_OK) return false;
    const uint8_t *bytes = (const uint8_t *)record;
    for (size_t i = 0; i < sizeof(*record); ++i) if (bytes[i] != 0xffU) return true;
    return false;
}

static void provisioning_record_erase(void)
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, AIRLINK_PROVISION_PARTITION_LABEL);
    if (partition == NULL) return;
    const esp_err_t err = esp_partition_erase_range(partition, 0, partition->size);
    if (err != ESP_OK) ESP_LOGW(TAG, "could not erase provisioning record: %s", esp_err_to_name(err));
}

static esp_err_t save_locked(const airlink_config_t *config)
{
    const uint32_t generation = s_generation + 1U;
    ESP_RETURN_ON_ERROR(write_record(config, generation), TAG, "write configuration");
    s_config = *config;
    s_generation = generation;
    return ESP_OK;
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
    s_config_lock = xSemaphoreCreateMutex();
    if (s_config_lock == NULL) return ESP_ERR_NO_MEM;

    nvs_handle_t nvs;
    /* A freshly erased NVS partition has no namespace yet. Opening it read-only
     * returns ESP_ERR_NVS_NOT_FOUND and used to make first boot reset forever.
     * Read-write mode creates the namespace; the missing A/B records below then
     * correctly fall through to the generated defaults. */
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "open NVS");
    config_record_t a = {0}, b = {0};
    bool a_valid = false, b_valid = false;
    ESP_GOTO_ON_ERROR(read_slot(nvs, "config_a", &a, &a_valid), out, TAG, "read slot A");
    ESP_GOTO_ON_ERROR(read_slot(nvs, "config_b", &b, &b_valid), out, TAG, "read slot B");
out:
    nvs_close(nvs);
    ESP_RETURN_ON_ERROR(ret, TAG, "load configuration");

    airlink_provision_record_t provision = {0};
    const bool provision_present = provisioning_record_read(&provision);
    factory_identity_t identity;
    const bool identity_present = read_identity(&identity);
    bool defaults = false;
    if (a_valid || b_valid) {
        const config_record_t *selected = !b_valid || (a_valid && a.generation >= b.generation) ? &a : &b;
        s_config = selected->config;
        s_generation = selected->generation;
    } else {
        load_defaults(&s_config);
        s_generation = 1;
        defaults = true;
    }
    bool credentials_changed = false;
    if (identity_present) {
        credentials_changed = strcmp(s_config.serial_number, identity.serial_number) != 0 ||
                              strcmp(s_config.ap_password, identity.initial_password) != 0 ||
                              strcmp(s_config.admin_password, identity.initial_password) != 0;
        strlcpy(s_config.serial_number, identity.serial_number, sizeof(s_config.serial_number));
        strlcpy(s_config.ap_password, identity.initial_password, sizeof(s_config.ap_password));
        strlcpy(s_config.admin_password, identity.initial_password, sizeof(s_config.admin_password));
    }
    if (provision_present) {
        if (airlink_provision_record_valid(&provision) && !identity_present) {
            credentials_changed = strcmp(s_config.ap_password, provision.password) != 0 ||
                                  strcmp(s_config.admin_password, provision.password) != 0;
            strlcpy(s_config.ap_password, provision.password, sizeof(s_config.ap_password));
            strlcpy(s_config.admin_password, provision.password, sizeof(s_config.admin_password));
            ESP_LOGI(TAG, "applied one-time USB provisioning password");
        } else if (identity_present) {
            ESP_LOGW(TAG, "factory identity present; preserving factory credentials");
        } else {
            ESP_LOGW(TAG, "invalid USB provisioning record ignored");
        }
        provisioning_record_erase();
    }
    if (defaults) {
        ESP_RETURN_ON_ERROR(write_record(&s_config, s_generation), TAG, "store defaults");
    } else if (credentials_changed) {
        ESP_RETURN_ON_ERROR(save_locked(&s_config), TAG, "store provisioned credentials");
    }
    if (snapshot != NULL) {
        *snapshot = (airlink_config_snapshot_t){s_config, s_generation, defaults};
    }
    ESP_LOGI(TAG, "loaded generation=%" PRIu32 " defaults=%d", s_generation, defaults);
    return ESP_OK;
}

void airlink_config_get(airlink_config_t *config)
{
    if (config == NULL) return;
    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    *config = s_config;
    xSemaphoreGive(s_config_lock);
}

uint32_t airlink_config_generation(void)
{
    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    const uint32_t generation = s_generation;
    xSemaphoreGive(s_config_lock);
    return generation;
}

esp_err_t airlink_config_save(const airlink_config_t *config)
{
    if (!airlink_config_validate(config)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    const esp_err_t err = save_locked(config);
    xSemaphoreGive(s_config_lock);
    return err;
}

esp_err_t airlink_config_factory_reset(void)
{
    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    airlink_config_t defaults;
    load_defaults(&defaults);
    const esp_err_t err = save_locked(&defaults);
    xSemaphoreGive(s_config_lock);
    return err;
}

esp_err_t airlink_config_set_identity(const char *serial, const char *password)
{
    const size_t password_length = password == NULL ? 0 : strnlen(password, AIRLINK_PASSWORD_MAX + 1U);
    if (!serial_valid(serial) || !identity_password_valid(password, password_length)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_config_lock, portMAX_DELAY);
    factory_identity_t existing;
    if (read_identity(&existing) &&
        (strcmp(existing.serial_number, serial) != 0 || strcmp(existing.initial_password, password) != 0)) {
        xSemaphoreGive(s_config_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t identity_err = write_identity(serial, password);
    if (identity_err != ESP_OK) {
        xSemaphoreGive(s_config_lock);
        return identity_err;
    }
    airlink_config_t updated = s_config;
    strlcpy(updated.serial_number, serial, sizeof(updated.serial_number));
    strlcpy(updated.ap_password, password, sizeof(updated.ap_password));
    strlcpy(updated.admin_password, password, sizeof(updated.admin_password));
    const esp_err_t err = save_locked(&updated);
    xSemaphoreGive(s_config_lock);
    return err;
}
