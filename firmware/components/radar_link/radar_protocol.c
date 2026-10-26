#include "radar_protocol.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define RADAR_ITEM_2D  16u
#define RADAR_ITEM_HLK 20u
#define RADAR_ITEM_3D  24u

static uint16_t item_stride(radar_wire_mode_t mode)
{
    switch (mode) {
    case RADAR_WIRE_2D:
        return RADAR_ITEM_2D;
    case RADAR_WIRE_HLK:
        return RADAR_ITEM_HLK;
    case RADAR_WIRE_3D:
        return RADAR_ITEM_3D;
    default:
        return 0;
    }
}

static uint8_t xor_cksum(const uint8_t *buf, size_t count)
{
    uint8_t value = 0;
    for (size_t i = 0; i < count; ++i) {
        value ^= buf[i];
    }
    return (uint8_t)~value;
}

static uint16_t read_u16_be(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static int32_t read_i32_le(const uint8_t *p)
{
    return (int32_t)read_u32_le(p);
}

static float read_f32_le(const uint8_t *p)
{
    uint32_t bits = read_u32_le(p);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void reset_for_sof(radar_tf_decoder_t *decoder)
{
    decoder->state = RADAR_TF_WAIT_SOF;
    decoder->field_used = 0;
    decoder->payload_used = 0;
}

static radar_wire_mode_t detect_mode(const uint8_t *payload, uint16_t length,
                                     radar_wire_mode_t latched)
{
    if (length < 4) {
        return RADAR_WIRE_UNKNOWN;
    }
    const uint16_t body = (uint16_t)(length - 4);
    const uint32_t count = read_u32_le(payload);
    if (body == 0) {
        return count == 0 ? latched : RADAR_WIRE_UNKNOWN;
    }
    if (count != 0 && count <= UINT16_MAX) {
        if (count * RADAR_ITEM_2D == body) {
            return RADAR_WIRE_2D;
        }
        if (count * RADAR_ITEM_HLK == body) {
            return RADAR_WIRE_HLK;
        }
        if (count * RADAR_ITEM_3D == body) {
            return RADAR_WIRE_3D;
        }
        return RADAR_WIRE_UNKNOWN;
    }
    return RADAR_WIRE_UNKNOWN;
}

/* Power-supply issues fixed: pick nearest clustered target from 0x0A04 only. */
#define RADAR_RANGE_MIN_M 0.05f
#define RADAR_RANGE_MAX_M 8.00f

static int32_t read_track_id(const uint8_t *item, radar_wire_mode_t mode)
{
    if (mode == RADAR_WIRE_3D) {
        return read_i32_le(item + 12);
    }
    if (mode == RADAR_WIRE_HLK) {
        return read_i32_le(item + 16);
    }
    return read_i32_le(item + 12); /* 2D */
}

static void fill_sample_from_item(radar_wire_sample_t *sample, const uint8_t *item,
                                  radar_wire_mode_t mode, float x, float y, float z,
                                  float distance, int32_t track_id)
{
    const bool has_z = mode == RADAR_WIRE_3D || mode == RADAR_WIRE_HLK;
    sample->present = true;
    sample->x_m = x;
    sample->y_m = y;
    sample->z_m = z;
    sample->distance_m = distance;
    sample->track_id = track_id;
    sample->azimuth_ok = true;
    sample->azimuth_deg = atan2f(x, y) * (180.0f / (float)M_PI);
    sample->elevation_deg =
        has_z ? asinf(fmaxf(-1.0f, fminf(1.0f, z / distance))) * (180.0f / (float)M_PI) : 0.0f;
    if (mode == RADAR_WIRE_3D) {
        sample->speed_mps_valid = true;
        sample->radial_speed_mps = read_f32_le(item + 20);
    } else if (mode == RADAR_WIRE_HLK) {
        sample->doppler_raw = read_i32_le(item + 12);
    } else {
        sample->doppler_raw = read_i32_le(item + 8);
    }
}

static bool parse_target(radar_tf_decoder_t *decoder, const uint8_t *payload, uint16_t length,
                         radar_wire_mode_t mode, radar_wire_sample_t *sample)
{
    (void)decoder;
    if (length < 4 || mode == RADAR_WIRE_UNKNOWN) {
        return false;
    }
    const uint32_t declared = read_u32_le(payload);
    const uint16_t stride = item_stride(mode);
    const uint16_t body = (uint16_t)(length - 4);
    if (stride == 0 || declared > INT32_MAX || declared * stride != body) {
        return false;
    }

    memset(sample, 0, sizeof(*sample));
    sample->mode = mode;
    sample->target_count = (int)declared;
    sample->track_id = -1;
    sample->blip_count = 0;
    if (declared == 0) {
        return true;
    }

    const bool has_z = mode == RADAR_WIRE_3D || mode == RADAR_WIRE_HLK;
    int chosen = -1;
    float best_dist = INFINITY;

    for (uint32_t i = 0; i < declared; ++i) {
        const uint8_t *item = payload + 4 + i * stride;
        const float x = read_f32_le(item);
        const float y = read_f32_le(item + 4);
        const float z = has_z ? read_f32_le(item + 8) : 0.0f;
        const float distance = sqrtf(x * x + y * y + z * z);
        if (!isfinite(distance) || distance < RADAR_RANGE_MIN_M || distance > RADAR_RANGE_MAX_M) {
            continue;
        }
        if (sample->blip_count < RADAR_MAX_BLIPS) {
            radar_wire_blip_t *b = &sample->blips[sample->blip_count++];
            b->x_m = x;
            b->y_m = y;
            b->z_m = z;
            b->distance_m = distance;
            b->azimuth_deg = atan2f(x, y) * (180.0f / (float)M_PI);
            b->track_id = read_track_id(item, mode);
        }
        if (distance < best_dist) {
            best_dist = distance;
            chosen = (int)i;
        }
    }

    if (chosen < 0) {
        return true;
    }

    const uint8_t *item = payload + 4 + (uint32_t)chosen * stride;
    const float x = read_f32_le(item);
    const float y = read_f32_le(item + 4);
    const float z = has_z ? read_f32_le(item + 8) : 0.0f;
    fill_sample_from_item(sample, item, mode, x, y, z, best_dist, read_track_id(item, mode));
    return true;
}

static void complete_frame(radar_tf_decoder_t *decoder, radar_wire_event_t *event)
{
    memset(event, 0, sizeof(*event));
    event->complete = true;
    event->type = decoder->type;
    if (decoder->type != RADAR_TF_TYPE_TARGET && decoder->type != RADAR_TF_TYPE_CLOUD) {
        return;
    }
    event->mode = detect_mode(decoder->payload, decoder->payload_len, decoder->latched_mode);
    if (event->mode != RADAR_WIRE_UNKNOWN) {
        decoder->latched_mode = event->mode;
    }
    if (decoder->type == RADAR_TF_TYPE_TARGET) {
        event->has_target_sample = parse_target(decoder, decoder->payload, decoder->payload_len,
                                                event->mode, &event->sample);
    }
}

void radar_tf_decoder_init(radar_tf_decoder_t *decoder)
{
    memset(decoder, 0, sizeof(*decoder));
    decoder->state = RADAR_TF_WAIT_SOF;
    decoder->latched_track_id = -1;
}

bool radar_tf_decoder_feed(radar_tf_decoder_t *decoder, uint8_t byte, radar_wire_event_t *event)
{
    if (!decoder || !event) {
        return false;
    }
    switch (decoder->state) {
    case RADAR_TF_WAIT_SOF:
        if (byte == RADAR_TF_SOF) {
            decoder->header[0] = byte;
            decoder->field_used = 0;
            decoder->state = RADAR_TF_READ_ID;
        }
        break;
    case RADAR_TF_READ_ID:
        decoder->header[1 + decoder->field_used++] = byte;
        if (decoder->field_used == 2) {
            decoder->field_used = 0;
            decoder->state = RADAR_TF_READ_LEN;
        }
        break;
    case RADAR_TF_READ_LEN:
        decoder->header[3 + decoder->field_used++] = byte;
        if (decoder->field_used == 2) {
            decoder->payload_len = read_u16_be(decoder->header + 3);
            decoder->field_used = 0;
            decoder->state = RADAR_TF_READ_TYPE;
        }
        break;
    case RADAR_TF_READ_TYPE:
        decoder->header[5 + decoder->field_used++] = byte;
        if (decoder->field_used == 2) {
            decoder->type = read_u16_be(decoder->header + 5);
            decoder->state = RADAR_TF_READ_HEAD_CKSUM;
        }
        break;
    case RADAR_TF_READ_HEAD_CKSUM:
        if (byte != xor_cksum(decoder->header, sizeof(decoder->header)) ||
            decoder->payload_len > RADAR_TF_PAYLOAD_MAX) {
            reset_for_sof(decoder);
        } else if (decoder->payload_len == 0) {
            complete_frame(decoder, event);
            reset_for_sof(decoder);
            return true;
        } else {
            decoder->payload_used = 0;
            decoder->state = RADAR_TF_READ_PAYLOAD;
        }
        break;
    case RADAR_TF_READ_PAYLOAD:
        decoder->payload[decoder->payload_used++] = byte;
        if (decoder->payload_used == decoder->payload_len) {
            decoder->state = RADAR_TF_READ_PAYLOAD_CKSUM;
        }
        break;
    case RADAR_TF_READ_PAYLOAD_CKSUM:
        if (byte == xor_cksum(decoder->payload, decoder->payload_len)) {
            complete_frame(decoder, event);
            reset_for_sof(decoder);
            return true;
        }
        reset_for_sof(decoder);
        break;
    }
    return false;
}
