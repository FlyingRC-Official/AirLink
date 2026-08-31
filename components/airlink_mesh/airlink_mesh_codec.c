// SPDX-License-Identifier: Apache-2.0
#include "airlink_mesh_codec.h"

#include <string.h>
#include "mbedtls/base64.h"
#include "psa/crypto.h"

_Static_assert(sizeof(airlink_mesh_wire_header_t) == AIRLINK_MESH_HEADER_SIZE,
               "mesh wire header layout changed");

static bool hkdf(const uint8_t *salt, size_t salt_length,
                 const uint8_t *key, size_t key_length,
                 const void *info, size_t info_length,
                 uint8_t *output, size_t output_length)
{
    psa_key_derivation_operation_t operation = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t status = psa_key_derivation_setup(
        &operation, PSA_ALG_HKDF(PSA_ALG_SHA_256));
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_bytes(&operation,
                                                PSA_KEY_DERIVATION_INPUT_SALT,
                                                salt, salt_length);
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_bytes(&operation,
                                                PSA_KEY_DERIVATION_INPUT_SECRET,
                                                key, key_length);
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_bytes(&operation,
                                                PSA_KEY_DERIVATION_INPUT_INFO,
                                                info, info_length);
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_output_bytes(&operation, output, output_length);
    }
    const psa_status_t abort_status = psa_key_derivation_abort(&operation);
    return status == PSA_SUCCESS && abort_status == PSA_SUCCESS;
}

static bool import_gcm_key(const uint8_t key[32], psa_key_usage_t usage,
                           mbedtls_svc_key_id_t *key_id)
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 256);
    psa_set_key_usage_flags(&attributes, usage);
    psa_set_key_algorithm(&attributes, PSA_ALG_GCM);
    const psa_status_t status = psa_import_key(&attributes, key, 32, key_id);
    psa_reset_key_attributes(&attributes);
    return status == PSA_SUCCESS;
}

bool airlink_mesh_derive_softap_password(const uint8_t fleet_key[32],
                                         const uint8_t network_id[6],
                                         char output[44])
{
    static const char info[] = "airlink-mesh-v1/softap";
    uint8_t derived[32];
    if (fleet_key == NULL || network_id == NULL || output == NULL ||
        !hkdf(network_id, 6, fleet_key, 32, info, sizeof(info) - 1,
              derived, sizeof(derived))) return false;
    uint8_t encoded[45];
    size_t length = 0;
    if (mbedtls_base64_encode(encoded, sizeof(encoded), &length,
                              derived, sizeof(derived)) != 0 || length != 44U) return false;
    /* A WPA2 password may be at most 63 characters.  Base64 for 32 bytes is
     * 44 characters including '=', so replacing padding keeps a printable,
     * deterministic 43-character secret. */
    if (encoded[43] == '=') { memcpy(output, encoded, 43); output[43] = '\0'; }
    else return false;
    return true;
}

static bool session_key(const uint8_t fleet_key[32], const uint8_t network_id[6],
                        const uint8_t source[6], const uint8_t session_id[16],
                        bool from_root, uint8_t output[32])
{
    static const char sender_prefix[] = "airlink-mesh-v1/sender/";
    uint8_t sender_info[sizeof(sender_prefix) - 1U + 6U];
    memcpy(sender_info, sender_prefix, sizeof(sender_prefix) - 1U);
    memcpy(sender_info + sizeof(sender_prefix) - 1U, source, 6U);
    uint8_t sender_key[32];
    if (!hkdf(network_id, 6, fleet_key, 32, sender_info, sizeof(sender_info),
              sender_key, sizeof(sender_key))) return false;
    const char *direction = from_root ? "airlink-mesh-v1/down" : "airlink-mesh-v1/up";
    return hkdf(session_id, 16, sender_key, sizeof(sender_key), direction,
                strlen(direction), output, 32);
}

static void nonce_for(bool from_root, uint64_t sequence, uint8_t nonce[12])
{
    const uint32_t stream = from_root ? UINT32_C(0x524f4f54) : UINT32_C(0x4e4f4445);
    nonce[0] = (uint8_t)(stream >> 24); nonce[1] = (uint8_t)(stream >> 16);
    nonce[2] = (uint8_t)(stream >> 8); nonce[3] = (uint8_t)stream;
    for (size_t i = 0; i < 8; ++i) nonce[4 + i] = (uint8_t)(sequence >> (56U - i * 8U));
}

static bool replay_accept(airlink_mesh_replay_window_t *window,
                          const airlink_mesh_wire_header_t *header)
{
    const bool new_sender = !window->initialized ||
        memcmp(window->source, header->source, 6) != 0;
    if (new_sender) *window = (airlink_mesh_replay_window_t){0};
    const bool new_session = !window->initialized ||
        memcmp(window->session_id, header->session_id, 16) != 0;
    if (new_session) {
        for (uint8_t i = 0; i < window->retired_session_count; ++i) {
            if (memcmp(window->retired_session_ids[i], header->session_id, 16) == 0) return false;
        }
        if (window->initialized) {
            memcpy(window->retired_session_ids[window->retired_session_next],
                   window->session_id, AIRLINK_MESH_SESSION_ID_SIZE);
            if (window->retired_session_count < 3U) window->retired_session_count++;
            window->retired_session_next = (uint8_t)((window->retired_session_next + 1U) % 3U);
        }
        window->initialized = true;
        memcpy(window->source, header->source, 6);
        memcpy(window->session_id, header->session_id, 16);
        window->highest_sequence = header->sequence;
        window->seen_bitmap = 1;
        return true;
    }
    if (header->sequence > window->highest_sequence) {
        const uint64_t distance = header->sequence - window->highest_sequence;
        window->seen_bitmap = distance >= 64U ? 1U :
                              (window->seen_bitmap << distance) | 1U;
        window->highest_sequence = header->sequence;
        return true;
    }
    const uint64_t distance = window->highest_sequence - header->sequence;
    if (distance >= 64U || (window->seen_bitmap & (UINT64_C(1) << distance)) != 0) return false;
    window->seen_bitmap |= UINT64_C(1) << distance;
    return true;
}

bool airlink_mesh_encode(airlink_mesh_crypto_context_t *context,
                         airlink_mesh_message_type_t type, uint16_t flags,
                         const uint8_t destination[6], const void *payload,
                         size_t payload_length, uint8_t *output,
                         size_t output_capacity, size_t *output_length)
{
    if (context == NULL || destination == NULL || output == NULL || output_length == NULL ||
        (payload_length != 0 && payload == NULL) || payload_length > AIRLINK_MESH_MAX_PAYLOAD ||
        output_capacity < AIRLINK_MESH_HEADER_SIZE + payload_length + AIRLINK_MESH_TAG_SIZE ||
        context->next_sequence == UINT64_MAX) return false;
    airlink_mesh_wire_header_t header = {
        .magic = AIRLINK_MESH_PROTOCOL_MAGIC,
        .version = AIRLINK_MESH_PROTOCOL_VERSION,
        .type = (uint8_t)type,
        .flags = (uint16_t)(flags | (context->from_root ? AIRLINK_MESH_FLAG_FROM_ROOT : 0U)),
        .sequence = context->next_sequence++,
        .payload_length = (uint16_t)payload_length,
    };
    memcpy(header.source, context->local_mac, 6);
    memcpy(header.destination, destination, 6);
    memcpy(header.session_id, context->session_id, 16);
    memcpy(output, &header, sizeof(header));
    const uint8_t *plain = payload_length == 0 ? output + sizeof(header) : payload;

    uint8_t key[32], nonce[12];
    if (!session_key(context->fleet_key, context->network_id, header.source,
                     header.session_id, context->from_root, key)) return false;
    nonce_for(context->from_root, header.sequence, nonce);
    mbedtls_svc_key_id_t key_id = MBEDTLS_SVC_KEY_ID_INIT;
    size_t encrypted_length = 0;
    const bool imported = import_gcm_key(key, PSA_KEY_USAGE_ENCRYPT, &key_id);
    psa_status_t result = imported ?
        psa_aead_encrypt(key_id, PSA_ALG_GCM, nonce, sizeof(nonce),
                         output, sizeof(header), plain, payload_length,
                         output + sizeof(header), payload_length + AIRLINK_MESH_TAG_SIZE,
                         &encrypted_length) : PSA_ERROR_INVALID_ARGUMENT;
    if (imported) (void)psa_destroy_key(key_id);
    memset(key, 0, sizeof(key));
    if (result != PSA_SUCCESS ||
        encrypted_length != payload_length + AIRLINK_MESH_TAG_SIZE) return false;
    *output_length = sizeof(header) + payload_length + AIRLINK_MESH_TAG_SIZE;
    return true;
}

airlink_mesh_decode_result_t airlink_mesh_decode_ex(
    const uint8_t fleet_key[32], const uint8_t network_id[6],
    const uint8_t *packet, size_t packet_length,
    airlink_mesh_replay_window_t *replay,
    airlink_mesh_decoded_packet_t *decoded)
{
    if (fleet_key == NULL || network_id == NULL || packet == NULL || replay == NULL ||
        decoded == NULL || packet_length < AIRLINK_MESH_HEADER_SIZE + AIRLINK_MESH_TAG_SIZE ||
        packet_length > AIRLINK_MESH_MAX_PACKET) return AIRLINK_MESH_DECODE_MALFORMED;
    airlink_mesh_wire_header_t header;
    memcpy(&header, packet, sizeof(header));
    if (header.magic != AIRLINK_MESH_PROTOCOL_MAGIC ||
        header.version != AIRLINK_MESH_PROTOCOL_VERSION || header.reserved != 0 ||
        header.payload_length > AIRLINK_MESH_MAX_PAYLOAD ||
        packet_length != sizeof(header) + header.payload_length + AIRLINK_MESH_TAG_SIZE) {
        return AIRLINK_MESH_DECODE_MALFORMED;
    }
    const bool from_root = (header.flags & AIRLINK_MESH_FLAG_FROM_ROOT) != 0;
    uint8_t key[32], nonce[12];
    if (!session_key(fleet_key, network_id, header.source, header.session_id,
                     from_root, key)) return AIRLINK_MESH_DECODE_AUTH_FAILED;
    nonce_for(from_root, header.sequence, nonce);
    mbedtls_svc_key_id_t key_id = MBEDTLS_SVC_KEY_ID_INIT;
    size_t plaintext_length = 0;
    const bool imported = import_gcm_key(key, PSA_KEY_USAGE_DECRYPT, &key_id);
    psa_status_t result = imported ?
        psa_aead_decrypt(key_id, PSA_ALG_GCM, nonce, sizeof(nonce),
                         packet, sizeof(header), packet + sizeof(header),
                         header.payload_length + AIRLINK_MESH_TAG_SIZE,
                         decoded->payload, sizeof(decoded->payload),
                         &plaintext_length) : PSA_ERROR_INVALID_ARGUMENT;
    if (imported) (void)psa_destroy_key(key_id);
    memset(key, 0, sizeof(key));
    if (result != PSA_SUCCESS || plaintext_length != header.payload_length) {
        return AIRLINK_MESH_DECODE_AUTH_FAILED;
    }
    if (!replay_accept(replay, &header)) return AIRLINK_MESH_DECODE_REPLAY;
    decoded->header = header;
    decoded->payload_length = header.payload_length;
    return AIRLINK_MESH_DECODE_OK;
}

bool airlink_mesh_decode(const uint8_t fleet_key[32], const uint8_t network_id[6],
                         const uint8_t *packet, size_t packet_length,
                         airlink_mesh_replay_window_t *replay,
                         airlink_mesh_decoded_packet_t *decoded)
{
    return airlink_mesh_decode_ex(fleet_key, network_id, packet, packet_length,
                                  replay, decoded) == AIRLINK_MESH_DECODE_OK;
}
