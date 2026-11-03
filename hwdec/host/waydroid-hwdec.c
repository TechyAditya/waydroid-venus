/*
 * Decodes video for Waydroid's OMX.waydroid.* decoders on the host GPU.
 * Listens on a Unix socket; every connection is one decoder, served by its
 * own thread.  See ../protocol.h.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

#include "../protocol.h"

/* frames the decoder may hold beyond its references, for the reply queue */
#define EXTRA_HW_FRAMES 8
#define MAX_QUEUED_FRAMES 16
#define MAX_CONFIG_SIZE (1 << 20)

static uid_t allowed_uid = 1046; /* AID_MEDIA_CODEC */
static unsigned max_sessions = 16;
static atomic_uint sessions;

static pthread_mutex_t device_lock = PTHREAD_MUTEX_INITIALIZER;
static enum AVHWDeviceType device_type = AV_HWDEVICE_TYPE_NONE;
static AVBufferRef *device;
static enum AVPixelFormat device_format = AV_PIX_FMT_NONE;
static bool device_failed;

static void
log_msg(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
   fputc('\n', stderr);
}

static enum AVPixelFormat
hw_format(enum AVHWDeviceType type)
{
   switch (type) {
   case AV_HWDEVICE_TYPE_CUDA:
      return AV_PIX_FMT_CUDA;
   case AV_HWDEVICE_TYPE_VAAPI:
      return AV_PIX_FMT_VAAPI;
   case AV_HWDEVICE_TYPE_VULKAN:
      return AV_PIX_FMT_VULKAN;
   default:
      return AV_PIX_FMT_NONE;
   }
}

/* One device shared by all decoders; creating a CUDA context costs ~100 ms
 * and hundreds of MB of GPU memory.  Without --device, the first of CUDA
 * (NVIDIA), VA-API (Intel, AMD) and Vulkan that works. */
static AVBufferRef *
get_device(void)
{
   static const enum AVHWDeviceType types[] = {
      AV_HWDEVICE_TYPE_CUDA,
      AV_HWDEVICE_TYPE_VAAPI,
      AV_HWDEVICE_TYPE_VULKAN,
   };
   pthread_mutex_lock(&device_lock);
   for (unsigned i = 0; !device && !device_failed && i < sizeof(types) / sizeof(types[0]); i++) {
      enum AVHWDeviceType type = device_type != AV_HWDEVICE_TYPE_NONE ? device_type : types[i];
      int err = av_hwdevice_ctx_create(&device, type, NULL, NULL, 0);
      if (err == 0) {
         device_format = hw_format(type);
         log_msg("decoding with %s", av_hwdevice_get_type_name(type));
      } else {
         log_msg("no %s device: %s", av_hwdevice_get_type_name(type), av_err2str(err));
      }
      if (device_type != AV_HWDEVICE_TYPE_NONE)
         break;
   }
   if (!device && !device_failed) {
      log_msg("no GPU decoder, decoding on the CPU");
      device_failed = true;
   }
   AVBufferRef *ref = device ? av_buffer_ref(device) : NULL;
   pthread_mutex_unlock(&device_lock);
   return ref;
}

/* a gralloc buffer from Android that frames can go straight into */
struct target {
   struct hwdec_target layout;
   uint8_t *map;
   size_t size;
};

struct session {
   int fd;
   /* the fd attached to the current request, or -1 */
   int received_fd;
   struct target targets[HWDEC_MAX_TARGETS];
   AVCodecContext *ctx;
   AVPacket *pkt;
   AVFrame *queue[MAX_QUEUED_FRAMES];
   unsigned queued;
   bool draining;
   bool eof;
   AVFrame *download;
   struct SwsContext *sws;
   uint8_t *payload;
   size_t payload_cap;
   /* codec configuration waiting for the next frame */
   uint8_t config[MAX_CONFIG_SIZE];
   size_t config_size;
   int shm_fd;
   uint8_t *shm;
   size_t shm_size;
   bool shm_sent;
   unsigned bad_packets;
};

/* Reads SIZE bytes, keeping an attached fd in s->received_fd. */
static bool
read_full(struct session *s, void *buf, size_t size)
{
   uint8_t *p = buf;
   while (size) {
      union {
         char buf[CMSG_SPACE(sizeof(int))];
         struct cmsghdr align;
      } control;
      struct iovec iov = { .iov_base = p, .iov_len = size };
      struct msghdr msg = {
         .msg_iov = &iov,
         .msg_iovlen = 1,
         .msg_control = control.buf,
         .msg_controllen = sizeof(control.buf),
      };
      ssize_t n = recvmsg(s->fd, &msg, MSG_CMSG_CLOEXEC);
      if (n < 0 && errno == EINTR)
         continue;
      if (n <= 0)
         return false;
      if (msg.msg_flags & MSG_CTRUNC) {
         log_msg("closing a connection that sent more than one fd");
         return false;
      }
      for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
         if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
            continue;
         int fd;
         memcpy(&fd, CMSG_DATA(c), sizeof(fd));
         if (c->cmsg_len != CMSG_LEN(sizeof(int)) || s->received_fd >= 0) {
            close(fd);
            return false;
         }
         s->received_fd = fd;
      }
      p += n;
      size -= n;
   }
   return true;
}

static void
unregister_targets(struct session *s)
{
   for (unsigned i = 0; i < HWDEC_MAX_TARGETS; i++) {
      if (s->targets[i].map)
         munmap(s->targets[i].map, s->targets[i].size);
      s->targets[i].map = NULL;
   }
}

static bool
plane_fits(uint64_t offset, uint64_t stride, uint64_t width, uint64_t rows, uint64_t size)
{
   return stride >= width && offset + stride * rows <= size;
}

/* Maps the attached memfd as target LAYOUT.  Only a memfd sealed against
 * shrinking is safe to write to: Android cannot truncate it under us. */
static bool
register_target(struct session *s, const struct hwdec_target *layout)
{
   int fd = s->received_fd;
   s->received_fd = -1;
   if (fd < 0)
      return false;

   bool ok = false;
   struct stat st;
   int seals = fcntl(fd, F_GET_SEALS);
   uint32_t w = layout->width, h = layout->height;
   uint32_t cw = (w + 1) / 2, ch = (h + 1) / 2;
   if (layout->id < 1 || layout->id > HWDEC_MAX_TARGETS || !w || !h ||
       w > HWDEC_MAX_DIMENSION || h > HWDEC_MAX_DIMENSION || seals < 0 ||
       !(seals & F_SEAL_SHRINK) || (seals & (F_SEAL_WRITE | F_SEAL_FUTURE_WRITE)) ||
       fstat(fd, &st) < 0 || st.st_size <= 0 || st.st_size > (1 << 30))
      goto out;
   size_t size = st.st_size;
   if (!plane_fits(layout->offset[0], layout->stride[0], w, h, size) ||
       !plane_fits(layout->offset[1], layout->stride[1], cw, ch, size) ||
       !plane_fits(layout->offset[2], layout->stride[2], cw, ch, size))
      goto out;

   uint8_t *map = mmap(NULL, size, PROT_WRITE, MAP_SHARED, fd, 0);
   if (map == MAP_FAILED)
      goto out;
   struct target *t = &s->targets[layout->id - 1];
   if (t->map)
      munmap(t->map, t->size);
   t->layout = *layout;
   t->map = map;
   t->size = size;
   ok = true;
out:
   close(fd);
   return ok;
}

static bool
send_reply(struct session *s, struct hwdec_reply *reply)
{
   struct iovec iov = { .iov_base = reply, .iov_len = sizeof(*reply) };
   union {
      char buf[CMSG_SPACE(sizeof(int))];
      struct cmsghdr align;
   } control;
   struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };

   bool in_shm = reply->type == HWDEC_REPLY_FRAME && !(reply->flags & HWDEC_FRAME_IN_TARGET);
   if (in_shm && !s->shm_sent) {
      reply->shm_size = s->shm_size;
      memset(&control, 0, sizeof(control));
      msg.msg_control = control.buf;
      msg.msg_controllen = sizeof(control.buf);
      struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
      cmsg->cmsg_level = SOL_SOCKET;
      cmsg->cmsg_type = SCM_RIGHTS;
      cmsg->cmsg_len = CMSG_LEN(sizeof(int));
      memcpy(CMSG_DATA(cmsg), &s->shm_fd, sizeof(int));
   }

   while (iov.iov_len) {
      ssize_t n = sendmsg(s->fd, &msg, MSG_NOSIGNAL);
      if (n < 0 && errno == EINTR)
         continue;
      if (n <= 0)
         return false;
      iov.iov_base = (uint8_t *)iov.iov_base + n;
      iov.iov_len -= n;
      msg.msg_control = NULL;
      msg.msg_controllen = 0;
   }
   if (in_shm)
      s->shm_sent = true;
   return true;
}

static bool
reply_simple(struct session *s, uint32_t type)
{
   struct hwdec_reply reply = { .type = type, .pending = s->queued };
   return send_reply(s, &reply);
}

/* Replaces the shared memory with a sealed memfd of at least SIZE bytes.
 * The seals keep Android from shrinking it under our writes. */
static bool
ensure_shm(struct session *s, size_t size)
{
   if (s->shm && s->shm_size >= size)
      return true;
   size = (size + (1 << 20) - 1) & ~(size_t)((1 << 20) - 1);

   int fd = memfd_create("waydroid-hwdec", MFD_CLOEXEC | MFD_ALLOW_SEALING);
   if (fd < 0)
      return false;
   if (ftruncate(fd, size) < 0 ||
       fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) < 0) {
      close(fd);
      return false;
   }
   uint8_t *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   if (map == MAP_FAILED) {
      close(fd);
      return false;
   }
   if (s->shm) {
      munmap(s->shm, s->shm_size);
      close(s->shm_fd);
   }
   s->shm_fd = fd;
   s->shm = map;
   s->shm_size = size;
   s->shm_sent = false;
   return true;
}

static enum AVPixelFormat
get_format(AVCodecContext *ctx, const enum AVPixelFormat *formats)
{
   const enum AVPixelFormat *f;
   if (ctx->hw_device_ctx) {
      for (f = formats; *f != AV_PIX_FMT_NONE; f++) {
         if (*f == device_format)
            return *f;
      }
      log_msg("%s profile %d not supported by the GPU, decoding on the CPU",
              ctx->codec->name, ctx->profile);
   }
   for (f = formats; *f != AV_PIX_FMT_NONE; f++) {
      if (!(av_pix_fmt_desc_get(*f)->flags & AV_PIX_FMT_FLAG_HWACCEL))
         return *f;
   }
   return AV_PIX_FMT_NONE;
}

static bool
open_decoder(struct session *s, const struct hwdec_open *open)
{
   static const char *const names[] = {
      [HWDEC_CODEC_H264] = "h264", [HWDEC_CODEC_HEVC] = "hevc",
      [HWDEC_CODEC_VP8] = "vp8",   [HWDEC_CODEC_VP9] = "vp9",
      [HWDEC_CODEC_AV1] = "av1",
   };
   if (s->ctx || open->version != HWDEC_VERSION || !open->codec ||
       open->codec >= sizeof(names) / sizeof(names[0]))
      return false;

   /* by name, so AV1 uses FFmpeg's decoder, which has hwaccels, rather
    * than libdav1d */
   const AVCodec *codec = avcodec_find_decoder_by_name(names[open->codec]);
   if (!codec)
      return false;
   s->ctx = avcodec_alloc_context3(codec);
   s->pkt = av_packet_alloc();
   s->download = av_frame_alloc();
   if (!s->ctx || !s->pkt || !s->download)
      return false;

   s->ctx->hw_device_ctx = get_device();
   s->ctx->get_format = get_format;
   s->ctx->extra_hw_frames = EXTRA_HW_FRAMES;
   s->ctx->pkt_timebase = (AVRational){ 1, 1000000 };
   /* frame threads only help the CPU fallback and add latency */
   s->ctx->thread_count = s->ctx->hw_device_ctx ? 1 : 0;
   return avcodec_open2(s->ctx, codec, NULL) == 0;
}

static void
clear_queue(struct session *s)
{
   for (unsigned i = 0; i < s->queued; i++)
      av_frame_free(&s->queue[i]);
   s->queued = 0;
}

/* Moves decoded frames into the queue.  Returns a negative AVERROR on a
 * decoder failure. */
static int
receive_frames(struct session *s)
{
   while (s->queued < MAX_QUEUED_FRAMES) {
      AVFrame *frame = av_frame_alloc();
      if (!frame)
         return AVERROR(ENOMEM);
      int err = avcodec_receive_frame(s->ctx, frame);
      if (err < 0) {
         av_frame_free(&frame);
         if (err == AVERROR_EOF)
            s->eof = true;
         if (err == AVERROR(EAGAIN) || err == AVERROR_EOF)
            return 0;
         /* a corrupt frame; the decoder recovers at the next one */
         if (err == AVERROR_INVALIDDATA)
            continue;
         return err;
      }
      s->queue[s->queued++] = frame;
   }
   return 0;
}

/* Converts the next queued frame into OUT's target, or else the shared
 * memory, and replies with it. */
static bool
reply_frame(struct session *s, const struct hwdec_output *out)
{
   AVFrame *frame = s->queue[0];
   memmove(s->queue, s->queue + 1, --s->queued * sizeof(s->queue[0]));

   AVFrame *src = frame;
   if (frame->hw_frames_ctx) {
      av_frame_unref(s->download);
      if (av_hwframe_transfer_data(s->download, frame, 0) < 0) {
         av_frame_free(&frame);
         return reply_simple(s, HWDEC_REPLY_ERROR);
      }
      src = s->download;
   }

   unsigned w = frame->width, h = frame->height;
   if (!w || !h || w > HWDEC_MAX_DIMENSION || h > HWDEC_MAX_DIMENSION) {
      av_frame_free(&frame);
      return reply_simple(s, HWDEC_REPLY_ERROR);
   }
   unsigned cw = (w + 1) / 2, ch = (h + 1) / 2;
   struct hwdec_reply reply = {
      .type = HWDEC_REPLY_FRAME,
      .pending = s->queued,
      .timestamp = frame->pts != AV_NOPTS_VALUE ? frame->pts : frame->best_effort_timestamp,
      .width = w,
      .height = h,
      .stride = { FFALIGN(w, 64), FFALIGN(cw, 64), FFALIGN(cw, 64) },
      .primaries = frame->color_primaries,
      .transfer = frame->color_trc,
      .matrix = frame->colorspace,
      .full_range = frame->color_range == AVCOL_RANGE_JPEG,
   };
   s->sws = sws_getCachedContext(s->sws, w, h, src->format, w, h, AV_PIX_FMT_YUV420P,
                                 SWS_BILINEAR, NULL, NULL, NULL);
   if (!s->sws) {
      av_frame_free(&frame);
      return reply_simple(s, HWDEC_REPLY_ERROR);
   }

   uint8_t *dst[4];
   int dst_stride[4];
   const struct target *t = NULL;
   if (out->target >= 1 && out->target <= HWDEC_MAX_TARGETS)
      t = &s->targets[out->target - 1];
   if (t && t->map && w == out->width && h == out->height && w <= t->layout.width &&
       h <= t->layout.height) {
      for (int i = 0; i < 3; i++) {
         dst[i] = t->map + t->layout.offset[i];
         dst_stride[i] = t->layout.stride[i];
      }
      reply.flags |= HWDEC_FRAME_IN_TARGET;
   } else {
      reply.offset[1] = reply.stride[0] * h;
      reply.offset[2] = reply.offset[1] + reply.stride[1] * ch;
      size_t size = reply.offset[2] + (size_t)reply.stride[2] * ch;
      if (!ensure_shm(s, size)) {
         av_frame_free(&frame);
         return reply_simple(s, HWDEC_REPLY_ERROR);
      }
      for (int i = 0; i < 3; i++) {
         dst[i] = s->shm + reply.offset[i];
         dst_stride[i] = reply.stride[i];
      }
   }
   sws_scale(s->sws, (const uint8_t *const *)src->data, src->linesize, 0, h, dst, dst_stride);
   av_frame_free(&frame);
   return send_reply(s, &reply);
}

static bool
handle_decode(struct session *s, uint32_t size)
{
   struct hwdec_packet hdr;
   if (size < sizeof(hdr) || !s->ctx || s->draining)
      return false;
   memcpy(&hdr, s->payload, sizeof(hdr));
   uint8_t *data = s->payload + sizeof(hdr);
   size -= sizeof(hdr);

   if (hdr.flags & HWDEC_PACKET_CONFIG) {
      /* H.264 and HEVC configuration is Annex B parameter sets, which the
       * decoders reject as a packet of their own, so they go in front of
       * the next frame.  VP8 and VP9 have none in-band, and AV1 repeats its
       * sequence header in the bitstream, so the container's configuration
       * record (marker bit set) is not needed. */
      enum AVCodecID id = s->ctx->codec_id;
      bool skip = id == AV_CODEC_ID_VP8 || id == AV_CODEC_ID_VP9 ||
                  (id == AV_CODEC_ID_AV1 && size && (data[0] & 0x80));
      if (!skip) {
         if (s->config_size + size > MAX_CONFIG_SIZE)
            return reply_simple(s, HWDEC_REPLY_ERROR);
         memcpy(s->config + s->config_size, data, size);
         s->config_size += size;
      }
      size = 0;
   }

   if (size) {
      av_packet_unref(s->pkt);
      if (av_new_packet(s->pkt, s->config_size + size) < 0)
         return reply_simple(s, HWDEC_REPLY_ERROR);
      memcpy(s->pkt->data, s->config, s->config_size);
      memcpy(s->pkt->data + s->config_size, data, size);
      s->config_size = 0;
      s->pkt->pts = hdr.timestamp;

      int err;
      for (;;) {
         err = avcodec_send_packet(s->ctx, s->pkt);
         if (err != AVERROR(EAGAIN))
            break;
         unsigned before = s->queued;
         if (receive_frames(s) < 0)
            return reply_simple(s, HWDEC_REPLY_ERROR);
         if (s->queued == before)
            break;
      }
      if (err == AVERROR_INVALIDDATA || err == AVERROR(EAGAIN)) {
         if (s->bad_packets++ < 10)
            log_msg("%s: dropped a packet: %s", s->ctx->codec->name, av_err2str(err));
      } else if (err < 0) {
         log_msg("%s: %s", s->ctx->codec->name, av_err2str(err));
         return reply_simple(s, HWDEC_REPLY_ERROR);
      }
   }

   if (receive_frames(s) < 0)
      return reply_simple(s, HWDEC_REPLY_ERROR);
   return s->queued ? reply_frame(s, &hdr.output) : reply_simple(s, HWDEC_REPLY_NO_FRAME);
}

static bool
handle_receive(struct session *s, const struct hwdec_output *out)
{
   if (!s->queued && !s->draining && receive_frames(s) < 0)
      return reply_simple(s, HWDEC_REPLY_ERROR);
   return s->queued ? reply_frame(s, out) : reply_simple(s, HWDEC_REPLY_NO_FRAME);
}

static bool
handle_drain(struct session *s, const struct hwdec_output *out)
{
   if (!s->draining) {
      s->draining = true;
      s->eof = false;
      avcodec_send_packet(s->ctx, NULL);
   }
   /* Draining decoders should not ask for more input, but give one that
    * does a moment before calling the stream broken. */
   for (int tries = 0; !s->queued && !s->eof && tries < 100; tries++) {
      if (receive_frames(s) < 0)
         return reply_simple(s, HWDEC_REPLY_ERROR);
      if (!s->queued && !s->eof)
         usleep(1000);
   }
   if (s->queued)
      return reply_frame(s, out);
   if (!s->eof)
      return reply_simple(s, HWDEC_REPLY_ERROR);

   avcodec_flush_buffers(s->ctx);
   s->draining = false;
   s->eof = false;
   return reply_simple(s, HWDEC_REPLY_EOS);
}

static bool
handle_request(struct session *s, const struct hwdec_request *req)
{
   switch (req->type) {
   case HWDEC_REQUEST_OPEN: {
      struct hwdec_open open;
      if (req->size != sizeof(open))
         return false;
      memcpy(&open, s->payload, sizeof(open));
      if (!open_decoder(s, &open)) {
         reply_simple(s, HWDEC_REPLY_ERROR);
         return false;
      }
      return reply_simple(s, HWDEC_REPLY_OK);
   }
   case HWDEC_REQUEST_DECODE:
      return handle_decode(s, req->size);
   case HWDEC_REQUEST_RECEIVE:
   case HWDEC_REQUEST_DRAIN: {
      struct hwdec_output out;
      if (req->size != sizeof(out) || !s->ctx)
         return false;
      memcpy(&out, s->payload, sizeof(out));
      return req->type == HWDEC_REQUEST_RECEIVE ? handle_receive(s, &out) : handle_drain(s, &out);
   }
   case HWDEC_REQUEST_REGISTER: {
      struct hwdec_target layout;
      if (req->size != sizeof(layout))
         return false;
      memcpy(&layout, s->payload, sizeof(layout));
      return reply_simple(s, register_target(s, &layout) ? HWDEC_REPLY_OK : HWDEC_REPLY_ERROR);
   }
   case HWDEC_REQUEST_UNREGISTER_ALL:
      if (req->size)
         return false;
      unregister_targets(s);
      return reply_simple(s, HWDEC_REPLY_OK);
   case HWDEC_REQUEST_FLUSH:
      if (req->size || !s->ctx)
         return false;
      clear_queue(s);
      avcodec_flush_buffers(s->ctx);
      s->draining = false;
      s->eof = false;
      return reply_simple(s, HWDEC_REPLY_OK);
   default:
      return false;
   }
}

static void *
session_main(void *arg)
{
   struct session *s = arg;
   struct hwdec_request req;

   while (read_full(s, &req, sizeof(req))) {
      if (req.size > HWDEC_MAX_PACKET_SIZE + sizeof(struct hwdec_packet))
         break;
      if (req.size > s->payload_cap) {
         uint8_t *p = realloc(s->payload, req.size);
         if (!p)
            break;
         s->payload = p;
         s->payload_cap = req.size;
      }
      if (!read_full(s, s->payload, req.size))
         break;
      if (!handle_request(s, &req)) {
         log_msg("closing a connection after a bad request %u of %u bytes", req.type, req.size);
         break;
      }
      /* only REGISTER takes an fd */
      if (s->received_fd >= 0) {
         close(s->received_fd);
         s->received_fd = -1;
      }
   }

   if (s->received_fd >= 0)
      close(s->received_fd);
   unregister_targets(s);
   clear_queue(s);
   avcodec_free_context(&s->ctx);
   av_packet_free(&s->pkt);
   av_frame_free(&s->download);
   sws_freeContext(s->sws);
   if (s->shm) {
      munmap(s->shm, s->shm_size);
      close(s->shm_fd);
   }
   free(s->payload);
   close(s->fd);
   free(s);
   atomic_fetch_sub(&sessions, 1);
   return NULL;
}

static void
accept_client(int listen_fd)
{
   int fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
   if (fd < 0)
      return;

   struct ucred cred;
   socklen_t len = sizeof(cred);
   if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0) {
      log_msg("refused a connection without credentials: %s", strerror(errno));
      close(fd);
      return;
   }
   if (cred.uid != allowed_uid) {
      log_msg("refused a connection from uid %d", (int)cred.uid);
      close(fd);
      return;
   }
   if (atomic_fetch_add(&sessions, 1) >= max_sessions) {
      atomic_fetch_sub(&sessions, 1);
      log_msg("refused a connection: %u decoders open", max_sessions);
      close(fd);
      return;
   }

   struct session *s = calloc(1, sizeof(*s));
   pthread_t thread;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
   if (s) {
      s->fd = fd;
      s->received_fd = -1;
      s->shm_fd = -1;
   }
   if (!s || pthread_create(&thread, &attr, session_main, s)) {
      free(s);
      close(fd);
      atomic_fetch_sub(&sessions, 1);
   }
   pthread_attr_destroy(&attr);
}

int
main(int argc, char **argv)
{
   static const struct option options[] = {
      { "socket-path", required_argument, NULL, 's' },
      { "allowed-uid", required_argument, NULL, 'u' },
      { "max-decoders", required_argument, NULL, 'n' },
      { "device", required_argument, NULL, 'd' },
      { 0 },
   };
   const char *path = NULL;
   int opt;
   while ((opt = getopt_long(argc, argv, "", options, NULL)) != -1) {
      switch (opt) {
      case 's':
         path = optarg;
         break;
      case 'u':
         allowed_uid = strtoul(optarg, NULL, 10);
         break;
      case 'n':
         max_sessions = strtoul(optarg, NULL, 10);
         break;
      case 'd':
         device_type = av_hwdevice_find_type_by_name(optarg);
         if (hw_format(device_type) == AV_PIX_FMT_NONE) {
            fprintf(stderr, "unsupported device %s; use cuda, vaapi or vulkan\n", optarg);
            return 2;
         }
         break;
      default:
         return 2;
      }
   }
   struct sockaddr_un addr = { .sun_family = AF_UNIX };
   if (!path || strlen(path) >= sizeof(addr.sun_path)) {
      fprintf(stderr,
              "usage: %s --socket-path PATH [--allowed-uid UID] [--max-decoders N] "
              "[--device cuda|vaapi|vulkan]\n",
              argv[0]);
      return 2;
   }
   strcpy(addr.sun_path, path);

   signal(SIGPIPE, SIG_IGN);
   int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
   unlink(path);
   if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
       chmod(path, 0666) < 0 || listen(fd, 16) < 0) {
      perror(path);
      return 1;
   }
   log_msg("listening on %s for uid %d", path, (int)allowed_uid);
   for (;;)
      accept_client(fd);
}
