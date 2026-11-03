/*
 * Wire protocol between the Android OMX decoder (client) and waydroid-hwdec
 * (host).  One connection is one decoder instance.  The client sends a
 * request header followed by `size` payload bytes; the host answers every
 * request with exactly one reply.  All integers are little endian.
 *
 * Decoded frames are I420.  The host writes a frame into a target buffer
 * the client registered, which is how frames go straight into Android's
 * gralloc buffers, or else into a shared memfd that it attaches
 * (SCM_RIGHTS) to the reply that first uses it.
 */
#ifndef WAYDROID_HWDEC_PROTOCOL_H
#define WAYDROID_HWDEC_PROTOCOL_H

#include <stdint.h>

#define HWDEC_VERSION 2
#define HWDEC_SOCKET_NAME "hwdec.sock"
#define HWDEC_MAX_PACKET_SIZE (64u << 20)
#define HWDEC_MAX_DIMENSION 8192u
#define HWDEC_MAX_TARGETS 64u

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
   /* payload: struct hwdec_output; returns a frame the last request left
    * pending, or HWDEC_REPLY_NO_FRAME */
   HWDEC_REQUEST_RECEIVE = 3,
   /* payload: struct hwdec_output; end of stream.  Returns the next
    * remaining frame, or HWDEC_REPLY_EOS once there is none, after which the
    * decoder accepts a new stream. */
   HWDEC_REQUEST_DRAIN = 4,
   /* no payload; drops every queued packet and frame; reply HWDEC_REPLY_OK */
   HWDEC_REQUEST_FLUSH = 5,
   /* payload: struct hwdec_target, with the buffer's memfd attached; reply
    * HWDEC_REPLY_OK, or HWDEC_REPLY_ERROR if the host cannot write to it */
   HWDEC_REQUEST_REGISTER = 6,
   /* no payload; forgets every target; reply HWDEC_REPLY_OK */
   HWDEC_REQUEST_UNREGISTER_ALL = 7,
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

/* the frame is in the requested target rather than the shared memory */
#define HWDEC_FRAME_IN_TARGET (1u << 0)

struct hwdec_request {
   uint32_t type;
   uint32_t size;
};

struct hwdec_open {
   uint32_t version;
   uint32_t codec;
};

/* Where a frame the request returns goes.  The host uses the target only
 * for a frame of exactly this size, so a frame that needs differently sized
 * buffers stays in the shared memory until the client has them. */
struct hwdec_output {
   /* a registered target, or 0 */
   uint32_t target;
   uint32_t width;
   uint32_t height;
};

struct hwdec_packet {
   int64_t timestamp;
   uint32_t flags;
   struct hwdec_output output;
};

/* A buffer of the attached memfd with Y, Cb and Cr planes at these offsets.
 * The memfd must be sealed against shrinking. */
struct hwdec_target {
   uint32_t id; /* 1 to HWDEC_MAX_TARGETS */
   uint32_t width;
   uint32_t height;
   uint32_t offset[3];
   uint32_t stride[3];
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
   uint32_t flags;
};

#endif
