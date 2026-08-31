// SPDX-License-Identifier: Apache-2.0
#include "airlink_provision.h"

#include <ctype.h>
#include <stddef.h>
#include "airlink_core.h"

_Static_assert(sizeof(airlink_provision_record_v1_t) == 77U, "v1 provision layout changed");
_Static_assert(sizeof(airlink_provision_record_t) == 104U, "v2 provision layout changed");

uint32_t airlink_provision_record_crc(const airlink_provision_record_t *record)
{
    return record == NULL ? 0 : airlink_crc32(record, offsetof(airlink_provision_record_t, crc32));
}

uint32_t airlink_provision_record_v1_crc(const airlink_provision_record_v1_t *record)
{
    return record == NULL ? 0 :
        airlink_crc32(record, offsetof(airlink_provision_record_v1_t, crc32));
}

bool airlink_provision_serial_valid(const char *serial, uint16_t length)
{
    if (serial == NULL || length == 0U || length >= AIRLINK_PROVISION_SERIAL_CAPACITY) return false;
    for (uint16_t i = 0; i < length; ++i) {
        const unsigned char c = (unsigned char)serial[i];
        if (!isalnum(c) && c != '-' && c != '_' && c != '.') return false;
    }
    return serial[length] == '\0';
}

bool airlink_provision_password_valid(const char *password, uint16_t length)
{
    if (password == NULL || length < 12U || length > 63U) return false;
    for (uint16_t i = 0; i < length; ++i) {
        const unsigned char c = (unsigned char)password[i];
        if (c < 0x21U || c > 0x7eU) return false;
    }
    return password[length] == '\0';
}

bool airlink_provision_record_valid(const airlink_provision_record_t *record)
{
    if (record == NULL || record->magic != AIRLINK_PROVISION_MAGIC ||
        record->version != AIRLINK_PROVISION_VERSION ||
        record->serial_length >= sizeof(record->serial) ||
        record->password_length >= sizeof(record->password) ||
        record->crc32 != airlink_provision_record_crc(record)) {
        return false;
    }
    return airlink_provision_serial_valid(record->serial, record->serial_length) &&
           airlink_provision_password_valid(record->password, record->password_length);
}

bool airlink_provision_record_v1_valid(const airlink_provision_record_v1_t *record)
{
    if (record == NULL || record->magic != AIRLINK_PROVISION_MAGIC ||
        record->version != AIRLINK_PROVISION_VERSION_V1 ||
        record->password_length >= sizeof(record->password) ||
        record->crc32 != airlink_provision_record_v1_crc(record)) return false;
    return airlink_provision_password_valid(record->password, record->password_length);
}

airlink_provision_action_t airlink_provision_decide(bool identity_present,
                                                     bool identity_matches_v2,
                                                     bool valid_v1,
                                                     bool valid_v2)
{
    if (identity_present) {
        if (valid_v2 && identity_matches_v2) return AIRLINK_PROVISION_ACTION_RETRY_V2_CONFIG;
        if (valid_v1 || valid_v2) return AIRLINK_PROVISION_ACTION_CONSUME;
        return AIRLINK_PROVISION_ACTION_NONE;
    }
    if (valid_v2) return AIRLINK_PROVISION_ACTION_CREATE_V2_IDENTITY;
    if (valid_v1) return AIRLINK_PROVISION_ACTION_APPLY_V1;
    return AIRLINK_PROVISION_ACTION_NONE;
}
