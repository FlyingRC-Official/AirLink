// SPDX-License-Identifier: Apache-2.0
#include "airlink_core.h"

/* This exact marker is embedded in every application image.  The OTA receiver
 * verifies the bytes from flash instead of trusting only request headers. */
static const char s_hardware_marker[] __attribute__((used)) = AIRLINK_IMAGE_HARDWARE_MARKER;

const char *airlink_image_hardware_marker(void)
{
    return s_hardware_marker;
}
