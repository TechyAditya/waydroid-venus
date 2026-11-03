/*
 * Wire protocol between the Android OMX decoder (client) and waydroid-hwdec
 * (host).  One connection is one decoder instance.  The client sends a
 * request header followed by `size` payload bytes; the host answers every
 * request with exactly one reply.  Decoded frames are written as I420 into
 * a shared memfd that the host attaches (SCM_RIGHTS) to the reply that first
 * uses it.  All integers are little endian.
 */
#ifndef WAYDROID_HWDEC_PROTOCOL_H
#define WAYDROID_HWDEC_PROTOCOL_H

#include <stdint.h>

#define HWDEC_VERSION 1
#define HWDEC_SOCKET_NAME "hwdec.sock"
#define HWDEC_MAX_PACKET_SIZE (64u << 20)
#define HWDEC_MAX_DIMENSION 8192u

enum hwdec_codec {
   HWDEC_CODEC_H264 = 1,
   HWDEC_CODEC_HEVC = 2,
   HWDEC_CODEC_VP8 = 3,
   HWDEC_CODEC_VP9 = 4,
   HWDEC_CODEC_AV1 = 5,
};

enum hwdec_request_type {
   /* payload: struct hwdec_open; reply HWDEC_REPLY_OK or HWDEC_REPLY_ERROR */
   HWDEC_REQUEST_OPEN = 1,
   /* payload: struct hwdec_packet, then the bitstream; reply a frame or
    * HWDEC_REPLY_NO_FRAME */
   HWDEC_REQUEST_DECODE = 2,
   /* no payload; returns a frame the last request left pending, or
    * HWDEC_REPLY_NO_FRAME */
   HWDEC_REQUEST_RECEIVE = 3,
   /* no payload; end of stream.  Returns the next remaining frame, or
    * HWDEC_REPLY_EOS once there is none, after which the decoder accepts a
    * new stream. */
   HWDEC_REQUEST_DRAIN = 4,
   /* no payload; drops every queued packet and frame; reply HWDEC_REPLY_OK */
   HWDEC_REQUEST_FLUSH = 5,
};

enum hwdec_reply_type {
   HWDEC_REPLY_OK = 1,
   HWDEC_REPLY_ERROR = 2,
   HWDEC_REPLY_FRAME = 3,
   HWDEC_REPLY_NO_FRAME = 4,
   HWDEC_REPLY_EOS = 5,
};

/* the packet is codec configuration (csd), not a frame */
#define HWDEC_PACKET_CONFIG (1u << 0)

struct hwdec_request {
   uint32_t type;
   uint32_t size;
};

struct hwdec_open {
   uint32_t version;
   uint32_t codec;
};

struct hwdec_packet {
   int64_t timestamp;
   uint32_t flags;
   uint32_t reserved;
};

struct hwdec_reply {
   uint32_t type;
   /* frames still queued on the host; fetch them with RECEIVE */
   uint32_t pending;
   int64_t timestamp;
   /* visible size of the frame */
   uint32_t width;
   uint32_t height;
   /* I420 planes in the shared memory */
   uint32_t offset[3];
   uint32_t stride[3];
   /* nonzero when a new shared memory fd is attached to this reply */
   uint64_t shm_size;
   /* ISO/IEC 23091-4 values; 2 means unspecified */
   uint8_t primaries;
   uint8_t transfer;
   uint8_t matrix;
   uint8_t full_range;
   uint32_t reserved;
};

#endif
