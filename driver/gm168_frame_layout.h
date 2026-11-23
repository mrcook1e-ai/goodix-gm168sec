/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Goodix GM168SEC wire-frame layout and packed 12-bit decoding.
 *
 * Kept free of USB and device state so the exact production transform can be
 * exercised with synthetic buffers in the frame-layout unit test.
 */
#pragma once

#include <glib.h>
#include <string.h>

#define GM168_TLS_FRAME_SIZE   10564
#define GM168_TLS_PAYLOAD_SIZE 10560   /* 80 wire rows × 132 bytes */
#define GM168_WIRE_ROWS        80
#define GM168_WIRE_ROW_STRIDE  132     /* bytes per wire row */
#define GM168_WIRE_ROW_DATA    96      /* packed pixel bytes per wire row */
#define GM168_FRAME_W          80      /* final image width */
#define GM168_FRAME_H          64      /* final image height */
#define GM168_FRAME_PIXELS     (GM168_FRAME_W * GM168_FRAME_H)
#define GM168_IMAGE_SCALE      2
#define GM168_OUTPUT_W         (GM168_FRAME_W * GM168_IMAGE_SCALE)
#define GM168_OUTPUT_H         (GM168_FRAME_H * GM168_IMAGE_SCALE)
#define GM168_FRAME_BYTES      (GM168_FRAME_PIXELS * 2)

/* Stream samples advance down each 64-pixel column before the next column. */
static inline guint
gm168_stream_to_image_index (guint stream_index)
{
    return (stream_index % GM168_FRAME_H) * GM168_FRAME_W
         + (stream_index / GM168_FRAME_H);
}

/* Decode the 10560-byte payload (without the four-byte trailer) into 5120
 * native uint16 pixels. Every six packed bytes produce four 12-bit samples. */
static inline int
gm168_decode_frame (const guint8 *in, gsize in_len, guint16 *out)
{
    guint8 compact[GM168_WIRE_ROWS * GM168_WIRE_ROW_DATA];
    int k = 0;

    if (!in || !out || in_len < (gsize) GM168_TLS_PAYLOAD_SIZE)
        return -1;

    for (int r = 0; r < GM168_WIRE_ROWS; r++)
        memcpy (compact + r * GM168_WIRE_ROW_DATA,
                in + r * GM168_WIRE_ROW_STRIDE,
                GM168_WIRE_ROW_DATA);

    for (int i = 0; i < GM168_WIRE_ROWS * GM168_WIRE_ROW_DATA; i += 6) {
        guint8 b0 = compact[i + 0], b1 = compact[i + 1], b2 = compact[i + 2];
        guint8 b3 = compact[i + 3], b4 = compact[i + 4], b5 = compact[i + 5];
        guint16 pix[4] = {
            ((b0 & 0x0F) << 8) | b1,
            (b3 << 4)          | (b0 >> 4),
            ((b5 & 0x0F) << 8) | b2,
            (b4 << 4)          | (b5 >> 4),
        };

        for (int j = 0; j < 4; j++)
            out[gm168_stream_to_image_index ((guint) (k + j))] = pix[j];
        k += 4;
    }

    return 0;
}
