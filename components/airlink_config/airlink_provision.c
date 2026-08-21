// SPDX-License-Identifier: Apache-2.0
#include "airlink_provision.h"

#include <stddef.h>
#include "airlink_core.h"

_Static_assert(sizeof(airlink_provision_record_t) == 77U, "provision record layout changed");

uint32_t airlink_provision_record_crc(const airlink_provision_record_t *record)
{
    return record == NULL ? 0 : airlink_crc32(record, offsetof(airlink_provision_record_t, crc32));
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
        record->password_length >= sizeof(record->password) ||
        record->crc32 != airlink_provision_record_crc(record)) {
        return false;
    }
    return airlink_provision_password_valid(record->password, record->password_length);
}
