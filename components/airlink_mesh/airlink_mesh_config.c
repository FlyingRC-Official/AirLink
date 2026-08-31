// SPDX-License-Identifier: Apache-2.0
#include "airlink_mesh_config.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "airlink_core.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"
#include "nvs.h"

#define MESH_CONFIG_MAGIC UINT32_C(0x414c4d43)
#define MESH_APPROVAL_MAGIC UINT32_C(0x414c4d41)

typedef struct {
    uint32_t magic;
    uint32_t generation;
    airlink_mesh_config_t config;
    uint32_t crc32;
} mesh_config_record_t;

typedef struct {
    uint32_t magic;
    uint32_t generation;
    airlink_mesh_approval_list_t list;
    uint32_t crc32;
} approval_record_t;

typedef struct {
    bool valid;
    uint32_t generation;
    airlink_mesh_config_t config;
} decoded_config_t;

static const char *TAG = "mesh_config";
static const char *MESH_NAMESPACE = "air_mesh";
static airlink_mesh_config_t s_config;
static uint32_t s_generation;
static airlink_mesh_approval_list_t s_approvals;
static uint32_t s_approval_generation;
static SemaphoreHandle_t s_lock;

typedef struct { const char code[3]; uint8_t first; uint8_t last; } country_rule_t;
static const country_rule_t COUNTRY_RULES[] = {
#include "airlink_mesh_country_table.inc"
};

bool airlink_mesh_channel_allowed(const char country[3], uint8_t channel)
{
    if (country == NULL || !isupper((unsigned char)country[0]) ||
        !isupper((unsigned char)country[1]) || country[2] != '\0') return false;
    for (size_t i = 0; i < sizeof(COUNTRY_RULES) / sizeof(COUNTRY_RULES[0]); ++i) {
        if (memcmp(country, COUNTRY_RULES[i].code, 3) == 0) {
            return channel >= COUNTRY_RULES[i].first && channel <= COUNTRY_RULES[i].last;
        }
    }
    return false;
}

static bool nonzero(const uint8_t *bytes, size_t length)
{
    uint8_t value = 0;
    for (size_t i = 0; i < length; ++i) value |= bytes[i];
    return value != 0;
}

bool airlink_mesh_config_validate(const airlink_mesh_config_t *config,
                                  const airlink_config_t *base_config)
{
    if (config == NULL || config->schema_version != AIRLINK_MESH_CONFIG_SCHEMA ||
        config->role > AIRLINK_MESH_ROLE_GROUND_ROOT) return false;
    if (!config->configured) return config->role == AIRLINK_MESH_ROLE_OFF;
    if (config->role == AIRLINK_MESH_ROLE_OFF || config->band != AIRLINK_MESH_BAND_2G ||
        config->max_nodes < 1U || config->max_nodes > AIRLINK_MESH_MAX_NODES ||
        config->max_hops < 1U || config->max_hops > 3U ||
        !airlink_mesh_channel_allowed(config->country, config->channel) ||
        !nonzero(config->network_id, sizeof(config->network_id)) ||
        !nonzero(config->fleet_key, sizeof(config->fleet_key))) return false;
    if (base_config != NULL && base_config->bridge_role != AIRLINK_BRIDGE_OFF) return false;
    return true;
}

static void defaults(airlink_mesh_config_t *config)
{
    *config = (airlink_mesh_config_t){
        .schema_version = AIRLINK_MESH_CONFIG_SCHEMA,
        .role = AIRLINK_MESH_ROLE_OFF,
        .band = AIRLINK_MESH_BAND_2G,
        .channel = 6,
        .country = "CN",
        .max_nodes = AIRLINK_MESH_MAX_NODES,
        .max_hops = 3,
    };
}

static uint32_t config_crc(const mesh_config_record_t *record)
{
    return airlink_crc32(record, offsetof(mesh_config_record_t, crc32));
}

static bool read_config_slot(nvs_handle_t nvs, const char *key, decoded_config_t *decoded)
{
    *decoded = (decoded_config_t){0};
    mesh_config_record_t record;
    size_t length = sizeof(record);
    if (nvs_get_blob(nvs, key, &record, &length) != ESP_OK) return false;
    if (length != sizeof(record) || record.magic != MESH_CONFIG_MAGIC ||
        record.crc32 != config_crc(&record) ||
        !airlink_mesh_config_validate(&record.config, NULL)) return false;
    decoded->valid = true;
    decoded->generation = record.generation;
    decoded->config = record.config;
    return true;
}

static esp_err_t write_config_slot(const char *prefix, const airlink_mesh_config_t *config,
                                   uint32_t generation)
{
    mesh_config_record_t record = {.magic = MESH_CONFIG_MAGIC, .generation = generation,
                                   .config = *config};
    record.crc32 = config_crc(&record);
    char key[16];
    snprintf(key, sizeof(key), "%s_%c", prefix, (generation & 1U) ? 'b' : 'a');
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(MESH_NAMESPACE, NVS_READWRITE, &nvs), TAG, "open mesh NVS");
    esp_err_t err = nvs_set_blob(nvs, key, &record, sizeof(record));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static uint32_t approval_crc(const approval_record_t *record)
{
    return airlink_crc32(record, offsetof(approval_record_t, crc32));
}

static bool read_approval_slot(nvs_handle_t nvs, const char *key, approval_record_t *record)
{
    size_t length = sizeof(*record);
    return nvs_get_blob(nvs, key, record, &length) == ESP_OK && length == sizeof(*record) &&
           record->magic == MESH_APPROVAL_MAGIC && record->list.count <= AIRLINK_MESH_MAX_NODES &&
           record->crc32 == approval_crc(record);
}

static esp_err_t save_approvals_locked(void)
{
    if (s_approval_generation == UINT32_MAX) return ESP_ERR_INVALID_STATE;
    const uint32_t generation = s_approval_generation + 1U;
    approval_record_t record = {.magic = MESH_APPROVAL_MAGIC, .generation = generation,
                                .list = s_approvals};
    record.crc32 = approval_crc(&record);
    const char *key = (generation & 1U) ? "allow_b" : "allow_a";
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(MESH_NAMESPACE, NVS_READWRITE, &nvs), TAG, "open approvals");
    esp_err_t err = nvs_set_blob(nvs, key, &record, sizeof(record));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err == ESP_OK) s_approval_generation = generation;
    return err;
}

esp_err_t airlink_mesh_config_init(airlink_mesh_config_snapshot_t *snapshot)
{
    if (snapshot == NULL) return ESP_ERR_INVALID_ARG;
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(MESH_NAMESPACE, NVS_READWRITE, &nvs), TAG, "open mesh NVS");
    decoded_config_t a = {0}, b = {0}, pa = {0}, pb = {0};
    (void)read_config_slot(nvs, "mesh_a", &a); (void)read_config_slot(nvs, "mesh_b", &b);
    (void)read_config_slot(nvs, "pending_a", &pa); (void)read_config_slot(nvs, "pending_b", &pb);
    decoded_config_t *selected = a.valid && (!b.valid || a.generation >= b.generation) ? &a :
                                 b.valid ? &b : NULL;
    if (selected == NULL) { defaults(&s_config); s_generation = 0; }
    else { s_config = selected->config; s_generation = selected->generation; }
    approval_record_t aa = {0}, ab = {0};
    const bool aa_ok = read_approval_slot(nvs, "allow_a", &aa);
    const bool ab_ok = read_approval_slot(nvs, "allow_b", &ab);
    approval_record_t *allow = aa_ok && (!ab_ok || aa.generation >= ab.generation) ? &aa :
                               ab_ok ? &ab : NULL;
    if (allow != NULL) { s_approvals = allow->list; s_approval_generation = allow->generation; }
    else { s_approvals = (airlink_mesh_approval_list_t){0}; s_approval_generation = 0; }
    nvs_close(nvs);
    *snapshot = (airlink_mesh_config_snapshot_t){
        .value = s_config, .generation = s_generation, .loaded_defaults = selected == NULL,
        .pending_present = pa.valid || pb.valid,
    };
    return ESP_OK;
}

void airlink_mesh_config_get(airlink_mesh_config_t *config)
{
    if (config == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY); *config = s_config; xSemaphoreGive(s_lock);
}

uint32_t airlink_mesh_config_generation(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const uint32_t generation = s_generation;
    xSemaphoreGive(s_lock);
    return generation;
}

esp_err_t airlink_mesh_config_create(airlink_mesh_role_t role,
                                     airlink_mesh_config_t *created)
{
    if (created == NULL || role == AIRLINK_MESH_ROLE_OFF || role > AIRLINK_MESH_ROLE_GROUND_ROOT) {
        return ESP_ERR_INVALID_ARG;
    }
    defaults(created);
    created->configured = 1;
    created->role = (uint8_t)role;
    esp_fill_random(created->network_id, sizeof(created->network_id));
    esp_fill_random(created->fleet_key, sizeof(created->fleet_key));
    return ESP_OK;
}

esp_err_t airlink_mesh_config_save(const airlink_mesh_config_t *config)
{
    airlink_config_t base; airlink_config_get(&base);
    if (!airlink_mesh_config_validate(config, &base)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_generation == UINT32_MAX) { xSemaphoreGive(s_lock); return ESP_ERR_INVALID_STATE; }
    const uint32_t generation = s_generation + 1U;
    esp_err_t err = write_config_slot("mesh", config, generation);
    if (err == ESP_OK) { s_config = *config; s_generation = generation; }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t airlink_mesh_config_stage(const airlink_mesh_config_t *config, uint32_t generation)
{
    airlink_config_t base; airlink_config_get(&base);
    if (!airlink_mesh_config_validate(config, &base)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = generation <= s_generation ? ESP_ERR_INVALID_ARG :
                    write_config_slot("pending", config, generation);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t airlink_mesh_config_commit_staged(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MESH_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) { xSemaphoreGive(s_lock); return err; }
    decoded_config_t a = {0}, b = {0};
    (void)read_config_slot(nvs, "pending_a", &a); (void)read_config_slot(nvs, "pending_b", &b);
    nvs_close(nvs);
    decoded_config_t *selected = a.valid && (!b.valid || a.generation >= b.generation) ? &a :
                                 b.valid ? &b : NULL;
    if (selected == NULL || selected->generation <= s_generation) {
        xSemaphoreGive(s_lock); return ESP_ERR_NOT_FOUND;
    }
    err = write_config_slot("mesh", &selected->config, selected->generation);
    if (err == ESP_OK) { s_config = selected->config; s_generation = selected->generation; }
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) return err;
    return airlink_mesh_config_abort_staged();
}

esp_err_t airlink_mesh_config_abort_staged(void)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(MESH_NAMESPACE, NVS_READWRITE, &nvs), TAG, "open pending");
    esp_err_t a = nvs_erase_key(nvs, "pending_a");
    esp_err_t b = nvs_erase_key(nvs, "pending_b");
    esp_err_t err = (a == ESP_OK || a == ESP_ERR_NVS_NOT_FOUND) &&
                    (b == ESP_OK || b == ESP_ERR_NVS_NOT_FOUND) ? nvs_commit(nvs) : ESP_FAIL;
    nvs_close(nvs);
    return err;
}

esp_err_t airlink_mesh_config_reset(void)
{
    airlink_mesh_config_t reset; defaults(&reset);
    return airlink_mesh_config_save(&reset);
}

esp_err_t airlink_mesh_config_export_json(const airlink_mesh_config_t *config,
                                          bool include_secret, char *output,
                                          size_t capacity)
{
    if (config == NULL || output == NULL || capacity == 0 || !config->configured) {
        return ESP_ERR_INVALID_ARG;
    }
    char id[13];
    snprintf(id, sizeof(id), "%02x%02x%02x%02x%02x%02x", config->network_id[0],
             config->network_id[1], config->network_id[2], config->network_id[3],
             config->network_id[4], config->network_id[5]);
    char key[45] = "";
    if (include_secret) {
        size_t length = 0;
        if (mbedtls_base64_encode((uint8_t *)key, sizeof(key), &length,
                                  config->fleet_key, sizeof(config->fleet_key)) != 0) return ESP_FAIL;
        for (size_t i = 0; i < length; ++i) {
            if (key[i] == '+') key[i] = '-';
            else if (key[i] == '/') key[i] = '_';
        }
        while (length != 0 && key[length - 1U] == '=') length--;
        key[length] = '\0';
    }
    const int written = snprintf(output, capacity,
        "{\"schema\":\"airlink-mesh-provision/v1\",\"network_id\":\"%s\","
        "\"fleet_key\":%s%s%s,\"band\":\"2g\",\"channel\":%u,"
        "\"country\":\"%s\",\"max_nodes\":%u,\"max_hops\":%u}",
        id, include_secret ? "\"" : "null", include_secret ? key : "",
        include_secret ? "\"" : "", config->channel, config->country,
        config->max_nodes, config->max_hops);
    return written > 0 && (size_t)written < capacity ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool parse_hex_id(const char *text, uint8_t id[6])
{
    if (text == NULL || strlen(text) != 12U) return false;
    for (size_t i = 0; i < 6; ++i) {
        unsigned value;
        if (sscanf(text + i * 2U, "%2x", &value) != 1) return false;
        id[i] = (uint8_t)value;
    }
    return true;
}

esp_err_t airlink_mesh_config_import_json(const char *json,
                                          airlink_mesh_role_t role,
                                          airlink_mesh_config_t *config)
{
    if (json == NULL || config == NULL || role == AIRLINK_MESH_ROLE_OFF) return ESP_ERR_INVALID_ARG;
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return ESP_ERR_INVALID_ARG;
    defaults(config); config->configured = 1; config->role = (uint8_t)role;
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "network_id");
    const cJSON *key = cJSON_GetObjectItemCaseSensitive(root, "fleet_key");
    const cJSON *band = cJSON_GetObjectItemCaseSensitive(root, "band");
    const cJSON *channel = cJSON_GetObjectItemCaseSensitive(root, "channel");
    const cJSON *country = cJSON_GetObjectItemCaseSensitive(root, "country");
    const cJSON *max_nodes = cJSON_GetObjectItemCaseSensitive(root, "max_nodes");
    const cJSON *max_hops = cJSON_GetObjectItemCaseSensitive(root, "max_hops");
    bool ok = cJSON_IsString(schema) && strcmp(schema->valuestring, "airlink-mesh-provision/v1") == 0 &&
              cJSON_IsString(id) && parse_hex_id(id->valuestring, config->network_id) &&
              cJSON_IsString(key) && cJSON_IsString(band) && strcmp(band->valuestring, "2g") == 0 &&
              cJSON_IsNumber(channel) && cJSON_IsString(country) && strlen(country->valuestring) == 2U &&
              cJSON_IsNumber(max_nodes) && cJSON_IsNumber(max_hops);
    size_t decoded = 0;
    char base64_key[45];
    if (ok) {
        const size_t key_length = strlen(key->valuestring);
        ok = key_length == 43U;
        if (ok) {
            memcpy(base64_key, key->valuestring, key_length);
            for (size_t i = 0; i < key_length; ++i) {
                if (base64_key[i] == '-') base64_key[i] = '+';
                else if (base64_key[i] == '_') base64_key[i] = '/';
                else if (!isalnum((unsigned char)base64_key[i])) { ok = false; break; }
            }
            base64_key[43] = '='; base64_key[44] = '\0';
        }
    }
    if (ok) ok = mbedtls_base64_decode(config->fleet_key, sizeof(config->fleet_key), &decoded,
                                       (const uint8_t *)base64_key, 44U) == 0 && decoded == 32U;
    if (ok) {
        config->channel = (uint8_t)channel->valueint;
        config->country[0] = (char)toupper((unsigned char)country->valuestring[0]);
        config->country[1] = (char)toupper((unsigned char)country->valuestring[1]);
        config->max_nodes = (uint8_t)max_nodes->valueint;
        config->max_hops = (uint8_t)max_hops->valueint;
    }
    cJSON_Delete(root);
    airlink_config_t base; airlink_config_get(&base);
    return ok && airlink_mesh_config_validate(config, &base) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t airlink_mesh_approval_get(airlink_mesh_approval_list_t *list)
{
    if (list == NULL) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY); *list = s_approvals; xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool airlink_mesh_approval_contains(const airlink_mesh_approval_list_t *list,
                                    const char *serial, const uint8_t sta_mac[6])
{
    if (list == NULL || serial == NULL || sta_mac == NULL) return false;
    for (size_t i = 0; i < list->count; ++i) {
        if (strcmp(list->entries[i].serial, serial) == 0 &&
            memcmp(list->entries[i].sta_mac, sta_mac, 6) == 0) return true;
    }
    return false;
}

esp_err_t airlink_mesh_approval_add(const char *serial, const uint8_t sta_mac[6])
{
    if (serial == NULL || sta_mac == NULL || strlen(serial) == 0 ||
        strlen(serial) > AIRLINK_MESH_SERIAL_SIZE) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (airlink_mesh_approval_contains(&s_approvals, serial, sta_mac)) {
        xSemaphoreGive(s_lock); return ESP_OK;
    }
    if (s_approvals.count >= AIRLINK_MESH_MAX_NODES) {
        xSemaphoreGive(s_lock); return ESP_ERR_NO_MEM;
    }
    const airlink_mesh_approval_list_t previous = s_approvals;
    airlink_mesh_approval_t *entry = &s_approvals.entries[s_approvals.count++];
    strlcpy(entry->serial, serial, sizeof(entry->serial)); memcpy(entry->sta_mac, sta_mac, 6);
    esp_err_t err = save_approvals_locked();
    if (err != ESP_OK) s_approvals = previous;
    xSemaphoreGive(s_lock); return err;
}

esp_err_t airlink_mesh_approval_remove(const char *serial, const uint8_t sta_mac[6])
{
    if (serial == NULL || sta_mac == NULL) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t found = AIRLINK_MESH_MAX_NODES;
    for (size_t i = 0; i < s_approvals.count; ++i) {
        if (strcmp(s_approvals.entries[i].serial, serial) == 0 &&
            memcmp(s_approvals.entries[i].sta_mac, sta_mac, 6) == 0) { found = i; break; }
    }
    if (found == AIRLINK_MESH_MAX_NODES) { xSemaphoreGive(s_lock); return ESP_ERR_NOT_FOUND; }
    const airlink_mesh_approval_list_t previous = s_approvals;
    for (size_t i = found + 1U; i < s_approvals.count; ++i) s_approvals.entries[i - 1U] = s_approvals.entries[i];
    memset(&s_approvals.entries[--s_approvals.count], 0, sizeof(s_approvals.entries[0]));
    esp_err_t err = save_approvals_locked();
    if (err != ESP_OK) s_approvals = previous;
    xSemaphoreGive(s_lock); return err;
}
