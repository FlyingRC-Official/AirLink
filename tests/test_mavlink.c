// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "airlink_core.h"
#include "airlink_mavlink.h"
#include "airlink_provision.h"

static void crc_accumulate(uint8_t data, uint16_t *crc)
{
    uint8_t tmp = data ^ (uint8_t)(*crc & 0xffU); tmp ^= (uint8_t)(tmp << 4U);
    *crc = (uint16_t)((*crc >> 8U) ^ ((uint16_t)tmp << 8U) ^ ((uint16_t)tmp << 3U) ^ ((uint16_t)tmp >> 4U));
}

static size_t mav1(uint8_t *out, uint8_t msgid, const uint8_t *payload, size_t payload_len, uint8_t extra)
{
    out[0]=0xfe; out[1]=(uint8_t)payload_len; out[2]=1; out[3]=1; out[4]=1; out[5]=msgid;
    memcpy(out+6,payload,payload_len); uint16_t crc=0xffff;
    for(size_t i=1;i<6+payload_len;i++) {
        crc_accumulate(out[i],&crc);
    }
    crc_accumulate(extra,&crc);
    out[6+payload_len]=(uint8_t)crc;out[7+payload_len]=(uint8_t)(crc>>8);return payload_len+8;
}

static void test_heartbeat(void)
{
    uint8_t payload[9]={0};payload[4]=2;payload[5]=3;payload[6]=0x80;uint8_t bytes[32];size_t n=mav1(bytes,0,payload,9,50);
    airlink_mavlink_parser_t p={0};airlink_mavlink_frame_t f={0};bool done=false;
    for(size_t i=0;i<n;i++)done=airlink_mavlink_parse_byte(&p,bytes[i],&f)||done;
    assert(done&&f.crc_known&&f.crc_valid&&f.heartbeat_valid&&f.heartbeat_armed&&
           f.heartbeat_type==2&&f.heartbeat_autopilot==3&&f.message_id==0&&f.length==17);
    assert(airlink_mavlink_heartbeat_is_autopilot(&f));

    bytes[4]=42; /* Non-primary MAVLink component must never drive the safety latch. */
    uint16_t crc=0xffff;for(size_t i=1;i<15;i++)crc_accumulate(bytes[i],&crc);crc_accumulate(50,&crc);
    bytes[15]=(uint8_t)crc;bytes[16]=(uint8_t)(crc>>8);
    p=(airlink_mavlink_parser_t){0};f=(airlink_mavlink_frame_t){0};done=false;
    for(size_t i=0;i<n;i++)done=airlink_mavlink_parse_byte(&p,bytes[i],&f)||done;
    assert(done&&f.heartbeat_valid&&!airlink_mavlink_heartbeat_is_autopilot(&f));
}

static void test_crc_rejection(void)
{
    uint8_t payload[9]={0},bytes[32];size_t n=mav1(bytes,0,payload,9,50);bytes[10]^=1;
    airlink_mavlink_parser_t p={0};airlink_mavlink_frame_t f={0};
    for(size_t i=0;i<n;i++)if(airlink_mavlink_parse_byte(&p,bytes[i],&f)){assert(f.crc_known&&!f.crc_valid);}
    assert(p.errors==1);
}

static void test_priority_and_resync(void)
{
    uint8_t payload[33]={0},bytes[64];size_t n=mav1(bytes,76,payload,33,152);
    airlink_mavlink_parser_t p={0};airlink_mavlink_frame_t f={0};
    airlink_mavlink_parse_byte(&p,0x55,&f);bool done=false;
    for(size_t i=0;i<n;i++)done=airlink_mavlink_parse_byte(&p,bytes[i],&f)||done;
    assert(done&&f.crc_valid&&f.high_priority&&f.message_id==76);
}

static void test_unknown_dialect_is_structurally_routable(void)
{
    uint8_t bytes[]={0xfe,1,1,1,1,200,0x42,0,0};
    airlink_mavlink_parser_t p={0};airlink_mavlink_frame_t f={0};bool done=false;
    for(size_t i=0;i<sizeof(bytes);i++)done=airlink_mavlink_parse_byte(&p,bytes[i],&f)||done;
    assert(done&&!f.crc_known&&!f.crc_valid&&f.message_id==200&&f.length==sizeof(bytes));
}

static void test_provision_record(void)
{
    airlink_provision_record_t record = {
        .magic = AIRLINK_PROVISION_MAGIC,
        .version = AIRLINK_PROVISION_VERSION,
        .password_length = 18,
        .password = "AirLink-Test_2026!",
    };
    record.crc32 = airlink_provision_record_crc(&record);
    assert(airlink_provision_record_valid(&record));
    record.password[0] ^= 1;
    assert(!airlink_provision_record_valid(&record));
    record.password[0] ^= 1;
    record.crc32 = airlink_provision_record_crc(&record);
    record.password[2] = ' ';
    record.crc32 = airlink_provision_record_crc(&record);
    assert(!airlink_provision_record_valid(&record));
}

int main(void)
{
    assert(airlink_crc32("123456789",9)==0xcbf43926U);
    test_heartbeat();test_crc_rejection();test_priority_and_resync();
    test_unknown_dialect_is_structurally_routable();
    test_provision_record();
    puts("host tests passed");return 0;
}
