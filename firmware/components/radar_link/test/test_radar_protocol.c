#include "radar_protocol.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#undef assert
#define assert(condition)                                                                    \
    do {                                                                                     \
        if (!(condition)) {                                                                  \
            fprintf(stderr, "assertion failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__); \
            exit(1);                                                                         \
        }                                                                                    \
    } while (0)

static uint8_t checksum(const uint8_t *data, size_t length)
{
    uint8_t value = 0;
    for (size_t i = 0; i < length; ++i) {
        value ^= data[i];
    }
    return (uint8_t)~value;
}

static void put_u32_le(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static void put_f32_le(uint8_t *out, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    put_u32_le(out, bits);
}

static size_t frame(uint8_t *out, uint16_t type, const uint8_t *payload, uint16_t length)
{
    out[0] = RADAR_TF_SOF;
    out[1] = 0x80; /* ID is opaque to the ESP32 parser. */
    out[2] = 0x01;
    out[3] = (uint8_t)(length >> 8);
    out[4] = (uint8_t)length;
    out[5] = (uint8_t)(type >> 8);
    out[6] = (uint8_t)type;
    out[7] = checksum(out, 7);
    if (length != 0) {
        memcpy(out + 8, payload, length);
        out[8 + length] = checksum(payload, length);
        return (size_t)length + 9;
    }
    return 8;
}

static bool feed(radar_tf_decoder_t *decoder, const uint8_t *bytes, size_t length,
                 radar_wire_event_t *event)
{
    bool complete = false;
    for (size_t i = 0; i < length; ++i) {
        if (radar_tf_decoder_feed(decoder, bytes[i], event)) {
            assert(!complete);
            complete = true;
        }
    }
    return complete;
}

static void make_2d(uint8_t *payload, uint32_t count, float x, float y, int32_t doppler)
{
    put_u32_le(payload, count);
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t *item = payload + 4 + i * 16;
        put_f32_le(item, x + (float)i);
        put_f32_le(item + 4, y);
        put_u32_le(item + 8, (uint32_t)doppler);
        put_u32_le(item + 12, i + 1);
    }
}

static void make_3d_item(uint8_t *item, float x, float y, float z, int32_t id, float speed)
{
    put_f32_le(item, x);
    put_f32_le(item + 4, y);
    put_f32_le(item + 8, z);
    put_u32_le(item + 12, (uint32_t)id);
    put_u32_le(item + 16, 0);
    put_f32_le(item + 20, speed);
}

static void test_fragmented_2d_and_empty(void)
{
    radar_tf_decoder_t decoder;
    radar_wire_event_t event;
    uint8_t payload[20], bytes[64];
    radar_tf_decoder_init(&decoder);
    make_2d(payload, 1, 0.6f, 0.8f, -17);
    const size_t length = frame(bytes, RADAR_TF_TYPE_TARGET, payload, sizeof(payload));
    assert(!feed(&decoder, bytes, 5, &event)); /* deliberate partial frame */
    assert(feed(&decoder, bytes + 5, length - 5, &event));
    assert(event.type == RADAR_TF_TYPE_TARGET && event.has_target_sample);
    assert(event.mode == RADAR_WIRE_2D && event.sample.present && event.sample.azimuth_ok);
    assert(!event.sample.speed_mps_valid && event.sample.doppler_raw == -17);
    assert(fabsf(event.sample.distance_m - 1.0f) < 0.001f);
    assert(fabsf(event.sample.azimuth_deg - 36.87f) < 0.02f);

    uint8_t empty[4] = {0};
    const size_t empty_length = frame(bytes, RADAR_TF_TYPE_TARGET, empty, sizeof(empty));
    assert(feed(&decoder, bytes, empty_length, &event));
    assert(event.has_target_sample && !event.sample.present && event.sample.target_count == 0);
}

static void test_concatenated_3d_and_cloud(void)
{
    radar_tf_decoder_t decoder;
    radar_wire_event_t event;
    uint8_t target[52], cloud[28], bytes[128];
    radar_tf_decoder_init(&decoder);
    put_u32_le(target, 2);
    make_3d_item(target + 4, 2.0f, 0.0f, 0.0f, 1, 0.3f);
    make_3d_item(target + 28, 0.0f, 0.6f, 0.8f, 2, -1.25f);
    const size_t a = frame(bytes, RADAR_TF_TYPE_TARGET, target, sizeof(target));
    put_u32_le(cloud, 1);
    make_3d_item(cloud + 4, 1.0f, 0.0f, 0.0f, -1, 0.0f);
    const size_t b = frame(bytes + a, RADAR_TF_TYPE_CLOUD, cloud, sizeof(cloud));

    assert(feed(&decoder, bytes, a, &event));
    assert(event.mode == RADAR_WIRE_3D && event.sample.target_count == 2);
    assert(event.sample.azimuth_ok && event.sample.track_id == 2);
    assert(fabsf(event.sample.distance_m - 1.0f) < 0.001f);
    assert(event.sample.speed_mps_valid && fabsf(event.sample.radial_speed_mps + 1.25f) < 0.001f);
    assert(feed(&decoder, bytes + a, b, &event));
    assert(event.type == RADAR_TF_TYPE_CLOUD && event.mode == RADAR_WIRE_3D);
    assert(!event.has_target_sample); /* Cloud telemetry must not create presence. */
}

static void make_hlk_item(uint8_t *item, float x, float y, float z, int32_t dop, int32_t id)
{
    put_f32_le(item, x);
    put_f32_le(item + 4, y);
    put_f32_le(item + 8, z);
    put_u32_le(item + 12, (uint32_t)dop);
    put_u32_le(item + 16, (uint32_t)id);
}

static void test_hlk_20byte_target(void)
{
    radar_tf_decoder_t decoder;
    radar_wire_event_t event;
    uint8_t payload[24], bytes[64];
    radar_tf_decoder_init(&decoder);
    put_u32_le(payload, 1);
    make_hlk_item(payload + 4, 0.6f, 0.8f, 0.0f, -3, 7);
    const size_t length = frame(bytes, RADAR_TF_TYPE_TARGET, payload, sizeof(payload));
    assert(feed(&decoder, bytes, length, &event));
    assert(event.mode == RADAR_WIRE_HLK && event.has_target_sample && event.sample.present);
    assert(event.sample.azimuth_ok && event.sample.track_id == 7);
    assert(!event.sample.speed_mps_valid && event.sample.doppler_raw == -3);
    assert(fabsf(event.sample.distance_m - 1.0f) < 0.001f);
    assert(fabsf(event.sample.azimuth_deg - 36.87f) < 0.02f);
}

static void test_nearest_cluster_wins(void)
{
    radar_tf_decoder_t decoder;
    radar_wire_event_t event;
    uint8_t payload[44], bytes[80];
    radar_tf_decoder_init(&decoder);
    put_u32_le(payload, 2);
    /* Nearest clustered target wins (id=1 closer than id=2). */
    make_hlk_item(payload + 4, 0.25f, 0.20f, 0.10f, 0, 1);
    make_hlk_item(payload + 24, 0.30f, 1.20f, 0.40f, 0, 2);
    const size_t length = frame(bytes, RADAR_TF_TYPE_TARGET, payload, sizeof(payload));
    assert(feed(&decoder, bytes, length, &event));
    assert(event.sample.present && event.sample.azimuth_ok);
    assert(event.sample.track_id == 1);
    assert(fabsf(event.sample.distance_m - sqrtf(0.25f * 0.25f + 0.20f * 0.20f + 0.10f * 0.10f)) <
           0.001f);
}

static void test_bad_checksums_unknown_and_resync(void)
{
    radar_tf_decoder_t decoder;
    radar_wire_event_t event;
    uint8_t payload[20], bytes[64];
    radar_tf_decoder_init(&decoder);
    make_2d(payload, 1, 0.0f, 1.0f, 3);
    const size_t length = frame(bytes, RADAR_TF_TYPE_TARGET, payload, sizeof(payload));
    bytes[7] ^= 0x01;
    assert(length > 8 && !feed(&decoder, bytes, 8, &event));

    const size_t unknown = frame(bytes, 0x4321u, NULL, 0);
    assert(feed(&decoder, bytes, unknown, &event));
    assert(event.complete && event.type == 0x4321u && !event.has_target_sample);

    const size_t good = frame(bytes, RADAR_TF_TYPE_TARGET, payload, sizeof(payload));
    bytes[good - 1] ^= 0x80;
    assert(!feed(&decoder, bytes, good, &event));
    const size_t recovered = frame(bytes, RADAR_TF_TYPE_TARGET, payload, sizeof(payload));
    assert(feed(&decoder, bytes, recovered, &event));
}

int main(void)
{
    test_fragmented_2d_and_empty();
    test_concatenated_3d_and_cloud();
    test_hlk_20byte_target();
    test_nearest_cluster_wins();
    test_bad_checksums_unknown_and_resync();
    puts("radar protocol tests: PASS");
    return 0;
}
