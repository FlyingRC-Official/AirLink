// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define AIRLINK_PROVISION_PARTITION_LABEL "provision"
#define AIRLINK_PROVISION_FLASH_OFFSET UINT32_C(0x2c000)
#define AIRLINK_PROVISION_PARTITION_SIZE UINT32_C(0x1000)
#define AIRLINK_PROVISION_MAGIC UINT32_C(0x414c5057)
#define AIRLINK_PROVISION_VERSION UINT16_C(1)
#define AIRLINK_PROVISION_PASSWORD_CAPACITY 65U

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t password_length;
    char password[AIRLINK_PROVISION_PASSWORD_CAPACITY];
    uint32_t crc32;
} airlink_provision_record_t;

uint32_t airlink_provision_record_crc(const airlink_provision_record_t *record);
bool airlink_provision_password_valid(const char *password, uint16_t length);
bool airlink_provision_record_valid(const airlink_provision_record_t *record);
