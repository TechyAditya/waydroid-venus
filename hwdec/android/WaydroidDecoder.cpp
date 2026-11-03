/*
 * OMX video decoders that decode on the host GPU through waydroid-hwdec.
 * Built as Waydroid's vendor OMX plugin (libstagefrighthw.so), which the
 * 32-bit media.codec service loads.  The components reuse the decoder
 * plumbing of Google's soft OMX decoders and only replace decoding with
 * requests to the host; see ../protocol.h.
 *
 * When ACodec renders to a surface, the output buffers are the surface's
 * YV12 gralloc buffers, and the host writes frames straight into them.
 * Otherwise frames come back in shared memory and are copied out.
 */
#define LOG_TAG "WaydroidDecoder"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#include <hardware/gralloc.h>
#include <log/log.h>
#include <media/hardware/HardwareAPI.h>
#include <media/hardware/OMXPluginBase.h>
#include <media/openmax/OMX_IndexExt.h>
#include <media/stagefright/foundation/ColorUtils.h>
#include <media/stagefright/omx/SoftVideoDecoderOMXComponent.h>
#include <system/graphics.h>

#include "../protocol.h"

namespace android {

namespace {

constexpr char kSocketPath[] = "/data/venus/" HWDEC_SOCKET_NAME;
constexpr OMX_U32 kNumBuffers = 8;
constexpr OMX_U32 kMinInputBufferSize = 2 << 20;
constexpr OMX_U32 kMinCompressionRatio = 2;
// a stuck host must not hang media.codec forever
constexpr int kTimeoutSeconds = 5;
constexpr OMX_COLOR_FORMATTYPE kNativeFormat = static_cast<OMX_COLOR_FORMATTYPE>(HAL_PIXEL_FORMAT_YV12);
constexpr OMX_COLOR_FORMATTYPE kNativeFormat10 =
        static_cast<OMX_COLOR_FORMATTYPE>(HAL_PIXEL_FORMAT_YCBCR_P010);
constexpr uint32_t kNativeUsage = GRALLOC_USAGE_SW_WRITE_OFTEN;

const CodecProfileLevel kAvcProfileLevels[] = {
    {OMX_VIDEO_AVCProfileConstrainedBaseline, OMX_VIDEO_AVCLevel52},
    {OMX_VIDEO_AVCProfileBaseline, OMX_VIDEO_AVCLevel52},
    {OMX_VIDEO_AVCProfileMain, OMX_VIDEO_AVCLevel52},
    {OMX_VIDEO_AVCProfileConstrainedHigh, OMX_VIDEO_AVCLevel52},
    {OMX_VIDEO_AVCProfileHigh, OMX_VIDEO_AVCLevel52},
};

// 10-bit streams come out as P010 or 16-bit planar, and HDR10 metadata is
// passed on, so the 10-bit and HDR10 profiles are supported.  HDR10+
// dynamic metadata is not.
const CodecProfileLevel kHevcProfileLevels[] = {
    {OMX_VIDEO_HEVCProfileMain, OMX_VIDEO_HEVCMainTierLevel52},
    {OMX_VIDEO_HEVCProfileMainStill, OMX_VIDEO_HEVCMainTierLevel52},
    {OMX_VIDEO_HEVCProfileMain10, OMX_VIDEO_HEVCMainTierLevel52},
    {OMX_VIDEO_HEVCProfileMain10HDR10, OMX_VIDEO_HEVCMainTierLevel52},
};

const CodecProfileLevel kVp8ProfileLevels[] = {
    {OMX_VIDEO_VP8ProfileMain, OMX_VIDEO_VP8Level_Version0},
};

const CodecProfileLevel kVp9ProfileLevels[] = {
    {OMX_VIDEO_VP9Profile0, OMX_VIDEO_VP9Level52},
    {OMX_VIDEO_VP9Profile2, OMX_VIDEO_VP9Level52},
    {OMX_VIDEO_VP9Profile2HDR, OMX_VIDEO_VP9Level52},
};

const CodecProfileLevel kAv1ProfileLevels[] = {
    {OMX_VIDEO_AV1ProfileMain8, OMX_VIDEO_AV1Level52},
    {OMX_VIDEO_AV1ProfileMain10, OMX_VIDEO_AV1Level52},
    {OMX_VIDEO_AV1ProfileMain10HDR10, OMX_VIDEO_AV1Level52},
};

struct CodecInfo {
    const char *name;
    const char *role;
    const char *mime;
    OMX_VIDEO_CODINGTYPE coding;
    uint32_t hostCodec;
    const CodecProfileLevel *profileLevels;
    size_t numProfileLevels;
};

#define PROFILE_LEVELS(a) a, sizeof(a) / sizeof(a[0])

const CodecInfo kCodecs[] = {
    {"OMX.waydroid.avc.decoder", "video_decoder.avc", "video/avc", OMX_VIDEO_CodingAVC,
     HWDEC_CODEC_H264, PROFILE_LEVELS(kAvcProfileLevels)},
    {"OMX.waydroid.hevc.decoder", "video_decoder.hevc", "video/hevc", OMX_VIDEO_CodingHEVC,
     HWDEC_CODEC_HEVC, PROFILE_LEVELS(kHevcProfileLevels)},
    {"OMX.waydroid.vp8.decoder", "video_decoder.vp8", "video/x-vnd.on2.vp8", OMX_VIDEO_CodingVP8,
     HWDEC_CODEC_VP8, PROFILE_LEVELS(kVp8ProfileLevels)},
    {"OMX.waydroid.vp9.decoder", "video_decoder.vp9", "video/x-vnd.on2.vp9", OMX_VIDEO_CodingVP9,
     HWDEC_CODEC_VP9, PROFILE_LEVELS(kVp9ProfileLevels)},
    {"OMX.waydroid.av1.decoder", "video_decoder.av1", "video/av01", OMX_VIDEO_CodingAV1,
     HWDEC_CODEC_AV1, PROFILE_LEVELS(kAv1ProfileLevels)},
};

class WaydroidDecoder : public SoftVideoDecoderOMXComponent {
  public:
    WaydroidDecoder(const CodecInfo &info, const OMX_CALLBACKTYPE *callbacks, OMX_PTR appData,
                    OMX_COMPONENTTYPE **component)
        : SoftVideoDecoderOMXComponent(info.name, info.role, info.coding, info.profileLevels,
                                       info.numProfileLevels, 320, 240, callbacks, appData,
                                       component) {
        initPorts(kNumBuffers, kMinInputBufferSize, kNumBuffers, info.mime, kMinCompressionRatio);
        const hw_module_t *module;
        if (hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &module) == 0) {
            mGralloc = reinterpret_cast<const gralloc_module_t *>(module);
            if (!mGralloc->lock || !mGralloc->lock_ycbcr || !mGralloc->unlock) mGralloc = nullptr;
        }
        mConnected = connectHost(info.hostCodec);
    }

    OMX_ERRORTYPE initCheck() const override {
        return mConnected ? OMX_ErrorNone : OMX_ErrorInsufficientResources;
    }

  protected:
    ~WaydroidDecoder() override {
        unmapShm();
        if (mFd >= 0) close(mFd);
    }

    OMX_ERRORTYPE getExtensionIndex(const char *name, OMX_INDEXTYPE *index) override;
    OMX_ERRORTYPE internalGetParameter(OMX_INDEXTYPE index, OMX_PTR params) override;
    OMX_ERRORTYPE internalSetParameter(OMX_INDEXTYPE index, const OMX_PTR params) override;
    void onQueueFilled(OMX_U32 portIndex) override;
    void onPortFlushCompleted(OMX_U32 portIndex) override;
    void onPortEnableCompleted(OMX_U32 portIndex, bool enabled) override;
    void onReset() override;

    int getColorAspectPreference() override { return kPreferBitstream; }
    bool supportDescribeHdrStaticInfo() override { return true; }

  private:
    enum {
        kEnableNativeBuffersIndex = kDescribeHdr10PlusInfoIndex + 1,
        kGetNativeBufferUsageIndex,
        kUseNativeBuffer2Index,
    };

    // an output gralloc buffer the host knows about
    struct Target {
        buffer_handle_t handle;
        // the host writes frames into it; otherwise they are copied in
        bool direct;
    };

    int mFd = -1;
    bool mConnected = false;
    uint8_t *mShm = nullptr;
    size_t mShmSize = 0;
    const gralloc_module_t *mGralloc = nullptr;
    // ACodec renders to a surface, and the output buffers are its buffers
    bool mNativeBuffers = false;
    // target i has host id i + 1
    Vector<Target> mTargets;
    bool mSignalledError = false;
    // the input reached end of stream; ask the host for the remaining frames
    bool mDraining = false;
    // frames the host has queued for us
    uint32_t mPending = 0;
    // a frame waiting in the shared memory for the output port to reconfigure
    bool mHaveHeldFrame = false;
    hwdec_reply mHeldFrame = {};

    bool connectHost(uint32_t codec);
    bool request(uint32_t type, const void *head, uint32_t headSize, const void *data,
                 uint32_t dataSize, int fd, hwdec_reply *reply);
    bool receiveReply(hwdec_reply *reply);
    void unmapShm();
    void signalError(const char *what);
    void resetState();
    void clearTargets();
    // Where the host should put the next frame, for OUTHEADER.
    hwdec_output outputFor(OMX_BUFFERHEADERTYPE *outHeader);
    uint32_t registerTarget(buffer_handle_t handle);
    bool copyToGraphicBuffer(const hwdec_reply &frame, buffer_handle_t handle);
    // The output color format FRAME needs.
    OMX_COLOR_FORMATTYPE formatFor(const hwdec_reply &frame) const;
    // Takes on FRAME's HDR10 metadata, telling ACodec when it changes.
    void updateHdrStaticInfo(const hwdec_reply &frame);
    // Returns false if the output port must reconfigure before the frame can
    // be written.
    bool outputFrame(const hwdec_reply &frame, BufferInfo *outInfo);
};

bool WaydroidDecoder::connectHost(uint32_t codec) {
    mFd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (mFd < 0) {
        ALOGE("socket: %s", strerror(errno));
        return false;
    }
    timeval timeout = {kTimeoutSeconds, 0};
    setsockopt(mFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(mFd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    strlcpy(addr.sun_path, kSocketPath, sizeof(addr.sun_path));
    if (connect(mFd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        ALOGE("connect %s: %s", kSocketPath, strerror(errno));
        return false;
    }

    hwdec_open open = {HWDEC_VERSION, codec};
    hwdec_reply reply;
    if (!request(HWDEC_REQUEST_OPEN, &open, sizeof(open), nullptr, 0, -1, &reply) ||
        reply.type != HWDEC_REPLY_OK) {
        ALOGE("the host cannot open decoder %u", codec);
        return false;
    }
    return true;
}

// Sends HEAD and DATA as the payload of a TYPE request, with FD attached
// unless it is -1, and waits for the reply.
bool WaydroidDecoder::request(uint32_t type, const void *head, uint32_t headSize,
                              const void *data, uint32_t dataSize, int fd, hwdec_reply *reply) {
    hwdec_request header = {type, headSize + dataSize};
    iovec iov[3];
    int count = 0;
    iov[count++] = {&header, sizeof(header)};
    if (headSize) iov[count++] = {const_cast<void *>(head), headSize};
    if (dataSize) iov[count++] = {const_cast<void *>(data), dataSize};

    union {
        char buf[CMSG_SPACE(sizeof(int))];
        cmsghdr align;
    } control;
    msghdr msg = {};
    if (fd >= 0) {
        memset(&control, 0, sizeof(control));
        msg.msg_control = control.buf;
        msg.msg_controllen = sizeof(control.buf);
        cmsghdr *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof(int));
    }

    for (int i = 0; i < count;) {
        msg.msg_iov = iov + i;
        msg.msg_iovlen = count - i;
        ssize_t n = sendmsg(mFd, &msg, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            ALOGE("send to the host: %s", n < 0 ? strerror(errno) : "closed");
            return false;
        }
        msg.msg_control = nullptr;
        msg.msg_controllen = 0;
        while (i < count && (size_t)n >= iov[i].iov_len) n -= iov[i++].iov_len;
        if (i < count) {
            iov[i].iov_base = static_cast<uint8_t *>(iov[i].iov_base) + n;
            iov[i].iov_len -= n;
        }
    }
    return receiveReply(reply);
}

bool WaydroidDecoder::receiveReply(hwdec_reply *reply) {
    uint8_t *p = reinterpret_cast<uint8_t *>(reply);
    size_t left = sizeof(*reply);
    int newFd = -1;
    while (left) {
        union {
            char buf[CMSG_SPACE(sizeof(int))];
            cmsghdr align;
        } control;
        iovec iov = {p, left};
        msghdr msg = {};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control.buf;
        msg.msg_controllen = sizeof(control.buf);
        ssize_t n = recvmsg(mFd, &msg, MSG_CMSG_CLOEXEC);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            ALOGE("receive from the host: %s", n < 0 ? strerror(errno) : "closed");
            if (newFd >= 0) close(newFd);
            return false;
        }
        for (cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS &&
                c->cmsg_len == CMSG_LEN(sizeof(int))) {
                if (newFd >= 0) close(newFd);
                memcpy(&newFd, CMSG_DATA(c), sizeof(int));
            }
        }
        p += n;
        left -= n;
    }

    if (newFd >= 0) {
        unmapShm();
        void *map = reply->shm_size && reply->shm_size <= SIZE_MAX
                            ? mmap(nullptr, reply->shm_size, PROT_READ, MAP_SHARED, newFd, 0)
                            : MAP_FAILED;
        close(newFd);
        if (map == MAP_FAILED) {
            ALOGE("map the frame memory: %s", strerror(errno));
            return false;
        }
        mShm = static_cast<uint8_t *>(map);
        mShmSize = reply->shm_size;
    }
    return true;
}

void WaydroidDecoder::unmapShm() {
    if (mShm) munmap(mShm, mShmSize);
    mShm = nullptr;
    mShmSize = 0;
}

void WaydroidDecoder::signalError(const char *what) {
    ALOGE("%s", what);
    notify(OMX_EventError, OMX_ErrorUndefined, 0, nullptr);
    mSignalledError = true;
}

void WaydroidDecoder::clearTargets() {
    if (mTargets.isEmpty()) return;
    mTargets.clear();
    hwdec_reply reply;
    if (!request(HWDEC_REQUEST_UNREGISTER_ALL, nullptr, 0, nullptr, 0, -1, &reply) ||
        reply.type != HWDEC_REPLY_OK)
        signalError("host connection lost");
}

// Tells the host about gralloc buffer HANDLE.  Returns its id.
uint32_t WaydroidDecoder::registerTarget(buffer_handle_t handle) {
    Target target = {handle, false};
    uint32_t id = mTargets.size() + 1;
    if (id > HWDEC_MAX_TARGETS) return 0;

    // the plane offsets in the buffer's fd, from where the gralloc maps it
    hwdec_target layout = {};
    layout.id = id;
    layout.width = outputBufferWidth();
    layout.height = outputBufferHeight();
    void *base = nullptr;
    android_ycbcr ycbcr = {};
    bool mapped = mGralloc->lock(mGralloc, handle, kNativeUsage, 0, 0, layout.width, layout.height,
                                 &base) == 0;
    if (mapped) mGralloc->unlock(mGralloc, handle);
    if (mapped && mGralloc->lock_ycbcr(mGralloc, handle, kNativeUsage, 0, 0, layout.width,
                                       layout.height, &ycbcr) == 0) {
        mGralloc->unlock(mGralloc, handle);
        uint8_t *planes[3] = {static_cast<uint8_t *>(ycbcr.y), static_cast<uint8_t *>(ycbcr.cb),
                              static_cast<uint8_t *>(ycbcr.cr)};
        size_t strides[3] = {ycbcr.ystride, ycbcr.cstride, ycbcr.cstride};
        // YV12 has three planes; P010 interleaves 16-bit Cb and Cr
        if (ycbcr.chroma_step == 1)
            layout.format = HWDEC_FORMAT_I420;
        else if (ycbcr.chroma_step == 4 && planes[2] == planes[1] + 2)
            layout.format = HWDEC_FORMAT_P010;
        for (int i = 0; i < 3; i++) {
            if (planes[i] < static_cast<uint8_t *>(base)) layout.format = 0;
            layout.offset[i] = planes[i] - static_cast<uint8_t *>(base);
            layout.stride[i] = strides[i];
        }
        hwdec_reply reply;
        if (layout.format && handle->numFds >= 1) {
            if (!request(HWDEC_REQUEST_REGISTER, &layout, sizeof(layout), nullptr, 0,
                         handle->data[0], &reply))
                return 0;
            target.direct = reply.type == HWDEC_REPLY_OK;
        }
    }
    if (!target.direct) ALOGW("frames for buffer %u are copied", id);
    mTargets.push(target);
    return id;
}

hwdec_output WaydroidDecoder::outputFor(OMX_BUFFERHEADERTYPE *outHeader) {
    hwdec_output out = {0, mWidth, mHeight};
    if (!mNativeBuffers) return out;
    auto handle = reinterpret_cast<buffer_handle_t>(outHeader->pBuffer);
    for (size_t i = 0; i < mTargets.size(); i++) {
        if (mTargets[i].handle == handle) {
            if (mTargets[i].direct) out.target = i + 1;
            return out;
        }
    }
    uint32_t id = registerTarget(handle);
    if (id && mTargets[id - 1].direct) out.target = id;
    return out;
}

bool WaydroidDecoder::copyToGraphicBuffer(const hwdec_reply &frame, buffer_handle_t handle) {
    android_ycbcr ycbcr = {};
    if (mGralloc->lock_ycbcr(mGralloc, handle, kNativeUsage, 0, 0, frame.width, frame.height,
                             &ycbcr) != 0)
        return false;
    uint8_t *dst[3] = {static_cast<uint8_t *>(ycbcr.y), static_cast<uint8_t *>(ycbcr.cb),
                       static_cast<uint8_t *>(ycbcr.cr)};
    size_t dstStride[3] = {ycbcr.ystride, ycbcr.cstride, ycbcr.cstride};
    // The shared memory holds 8-bit or 16-bit (10 bits in the low bits)
    // planes; the buffer is 8-bit planar YV12, or P010 with 16-bit samples
    // holding 10 bits in the high bits and Cb Cr interleaved.
    bool srcDeep = frame.format == HWDEC_FORMAT_I420_10;
    bool dstDeep = mOutputFormat == kNativeFormat10;
    size_t step = dstDeep ? 2 : 1;
    if (ycbcr.chroma_step != (dstDeep ? 4u : 1u)) {
        mGralloc->unlock(mGralloc, handle);
        return false;
    }
    for (int i = 0; i < 3; i++) {
        uint32_t w = i ? (frame.width + 1) / 2 : frame.width;
        uint32_t h = i ? (frame.height + 1) / 2 : frame.height;
        size_t pixelStep = i ? ycbcr.chroma_step : step;
        const uint8_t *src = mShm + frame.offset[i];
        for (uint32_t y = 0; y < h; y++) {
            const uint8_t *s = src + y * frame.stride[i];
            uint8_t *d = dst[i] + y * dstStride[i];
            if (!srcDeep && !dstDeep && pixelStep == 1) {
                memcpy(d, s, w);
                continue;
            }
            for (uint32_t x = 0; x < w; x++) {
                uint32_t v = srcDeep ? reinterpret_cast<const uint16_t *>(s)[x] << 6 : s[x] << 8;
                if (dstDeep)
                    *reinterpret_cast<uint16_t *>(d + x * pixelStep) = v;
                else
                    d[x * pixelStep] = v >> 8;
            }
        }
    }
    mGralloc->unlock(mGralloc, handle);
    return true;
}

OMX_COLOR_FORMATTYPE WaydroidDecoder::formatFor(const hwdec_reply &frame) const {
    bool deep = frame.format == HWDEC_FORMAT_I420_10 || frame.format == HWDEC_FORMAT_P010;
    if (mNativeBuffers) return deep ? kNativeFormat10 : kNativeFormat;
    return deep ? OMX_COLOR_FormatYUV420Planar16 : OMX_COLOR_FormatYUV420Planar;
}

void WaydroidDecoder::updateHdrStaticInfo(const hwdec_reply &frame) {
    const hwdec_hdr &h = frame.hdr;
    if (!h.max_luminance && !h.max_cll && !h.primaries[0][0]) return;
    HDRStaticInfo info = {};
    info.mID = HDRStaticInfo::kType1;
    info.sType1.mR = {h.primaries[0][0], h.primaries[0][1]};
    info.sType1.mG = {h.primaries[1][0], h.primaries[1][1]};
    info.sType1.mB = {h.primaries[2][0], h.primaries[2][1]};
    info.sType1.mW = {h.white[0], h.white[1]};
    info.sType1.mMaxDisplayLuminance = h.max_luminance;
    info.sType1.mMinDisplayLuminance = h.min_luminance;
    info.sType1.mMaxContentLightLevel = h.max_cll;
    info.sType1.mMaxFrameAverageLightLevel = h.max_fall;
    if (!memcmp(&info, &mHdrStaticInfo, sizeof(info))) return;
    mHdrStaticInfo = info;
    // ACodec rereads the output format, HDR metadata included, on any port
    // settings change
    notify(OMX_EventPortSettingsChanged, kOutputPortIndex, kDescribeHdrStaticInfoIndex, nullptr);
}

bool WaydroidDecoder::outputFrame(const hwdec_reply &frame, BufferInfo *outInfo) {
    ColorAspects aspects;
    ColorUtils::convertIsoColorAspectsToCodecAspects(frame.primaries, frame.transfer, frame.matrix,
                                                     frame.full_range, aspects);
    if (colorAspectsDiffer(aspects, mBitstreamColorAspects)) {
        mBitstreamColorAspects = aspects;
        handleColorAspectsChange();
    }

    updateHdrStaticInfo(frame);

    // a change of bit depth reconfigures the port like a change of size
    bool portWillReset = false;
    handlePortSettingsChange(&portWillReset, frame.width, frame.height, formatFor(frame));
    if (portWillReset) return false;

    OMX_BUFFERHEADERTYPE *outHeader = outInfo->mHeader;
    size_t bpp = mOutputFormat == OMX_COLOR_FormatYUV420Planar16 ? 2 : 1;
    size_t size = (size_t)outputBufferWidth() * outputBufferHeight() * 3 / 2 * bpp;
    if (mNativeBuffers) {
        auto handle = reinterpret_cast<buffer_handle_t>(outHeader->pBuffer);
        if (!(frame.flags & HWDEC_FRAME_IN_TARGET) && !copyToGraphicBuffer(frame, handle)) {
            signalError("cannot write to an output buffer");
            return true;
        }
    } else {
        if (outHeader->nAllocLen < size) {
            signalError("output buffer too small");
            return true;
        }
        copyYV12FrameToOutputBuffer(outHeader->pBuffer, mShm + frame.offset[0],
                                    mShm + frame.offset[1], mShm + frame.offset[2],
                                    frame.stride[0], frame.stride[1], frame.stride[2]);
    }
    outHeader->nOffset = 0;
    outHeader->nFilledLen = size;
    outHeader->nFlags = 0;
    outHeader->nTimeStamp = frame.timestamp;

    List<BufferInfo *> &outQueue = getPortQueue(kOutputPortIndex);
    outQueue.erase(outQueue.begin());
    outInfo->mOwnedByUs = false;
    notifyFillBufferDone(outHeader);
    return true;
}

OMX_ERRORTYPE WaydroidDecoder::getExtensionIndex(const char *name, OMX_INDEXTYPE *index) {
    if (mGralloc) {
        if (!strcmp(name, "OMX.google.android.index.enableAndroidNativeBuffers")) {
            *index = static_cast<OMX_INDEXTYPE>(kEnableNativeBuffersIndex);
            return OMX_ErrorNone;
        }
        if (!strcmp(name, "OMX.google.android.index.getAndroidNativeBufferUsage")) {
            *index = static_cast<OMX_INDEXTYPE>(kGetNativeBufferUsageIndex);
            return OMX_ErrorNone;
        }
        // The buffer handle arrives as the buffer pointer of OMX_UseBuffer,
        // which SimpleSoftOMXComponent already handles.
        if (!strcmp(name, "OMX.google.android.index.useAndroidNativeBuffer2")) {
            *index = static_cast<OMX_INDEXTYPE>(kUseNativeBuffer2Index);
            return OMX_ErrorNone;
        }
    }
    return SoftVideoDecoderOMXComponent::getExtensionIndex(name, index);
}

OMX_ERRORTYPE WaydroidDecoder::internalGetParameter(OMX_INDEXTYPE index, OMX_PTR params) {
    switch ((int)index) {
        case OMX_IndexParamVideoAndroidRequiresSwRenderer: {
            // Without a usable gralloc, frames come out in byte buffers and
            // ACodec copies them to the surface.
            if (mGralloc) break;
            auto *param = static_cast<OMX_PARAM_U32TYPE *>(params);
            if (!isValidOMXParam(param)) return OMX_ErrorBadParameter;
            if (param->nPortIndex != kOutputPortIndex) return OMX_ErrorBadPortIndex;
            param->nU32 = 1;
            return OMX_ErrorNone;
        }
        case kGetNativeBufferUsageIndex: {
            auto *param = static_cast<GetAndroidNativeBufferUsageParams *>(params);
            if (!isValidOMXParam(param)) return OMX_ErrorBadParameter;
            if (param->nPortIndex != kOutputPortIndex) return OMX_ErrorBadPortIndex;
            param->nUsage = kNativeUsage;
            return OMX_ErrorNone;
        }
        case OMX_IndexParamVideoPortFormat: {
            OMX_ERRORTYPE err = SoftVideoDecoderOMXComponent::internalGetParameter(index, params);
            auto *param = static_cast<OMX_VIDEO_PARAM_PORTFORMATTYPE *>(params);
            if (err == OMX_ErrorNone && param->nPortIndex == kOutputPortIndex && mNativeBuffers)
                param->eColorFormat = mOutputFormat;
            return err;
        }
    }
    return SoftVideoDecoderOMXComponent::internalGetParameter(index, params);
}

OMX_ERRORTYPE WaydroidDecoder::internalSetParameter(OMX_INDEXTYPE index, const OMX_PTR params) {
    switch ((int)index) {
        case kEnableNativeBuffersIndex: {
            auto *param = static_cast<const EnableAndroidNativeBuffersParams *>(params);
            if (!isValidOMXParam(param)) return OMX_ErrorBadParameter;
            if (param->nPortIndex != kOutputPortIndex) return OMX_ErrorBadPortIndex;
            // ACodec passes the output color format to the surface as its HAL
            // pixel format.
            mNativeBuffers = param->enable;
            mOutputFormat = mNativeBuffers ? kNativeFormat : OMX_COLOR_FormatYUV420Planar;
            updatePortDefinitions();
            clearTargets();
            return OMX_ErrorNone;
        }
        case OMX_IndexParamVideoPortFormat: {
            auto *param = static_cast<const OMX_VIDEO_PARAM_PORTFORMATTYPE *>(params);
            if (!isValidOMXParam(param)) return OMX_ErrorBadParameter;
            if (param->nPortIndex == kOutputPortIndex && mNativeBuffers) {
                bool ok = param->eCompressionFormat == OMX_VIDEO_CodingUnused &&
                          param->eColorFormat == mOutputFormat;
                return ok ? OMX_ErrorNone : OMX_ErrorUnsupportedSetting;
            }
            break;
        }
    }
    return SoftVideoDecoderOMXComponent::internalSetParameter(index, params);
}

bool frameFits(const hwdec_reply &f, size_t shmSize) {
    if (!f.width || !f.height || f.width > HWDEC_MAX_DIMENSION || f.height > HWDEC_MAX_DIMENSION)
        return false;
    if (f.flags & HWDEC_FRAME_IN_TARGET) return true;
    if (f.format != HWDEC_FORMAT_I420 && f.format != HWDEC_FORMAT_I420_10) return false;
    uint32_t bpp = f.format == HWDEC_FORMAT_I420_10 ? 2 : 1;
    uint32_t rows[3] = {f.height, (f.height + 1) / 2, (f.height + 1) / 2};
    uint32_t widths[3] = {f.width * bpp, (f.width + 1) / 2 * bpp, (f.width + 1) / 2 * bpp};
    for (int i = 0; i < 3; i++) {
        if (f.stride[i] < widths[i]) return false;
        uint64_t end = f.offset[i] + (uint64_t)f.stride[i] * rows[i];
        if (end > shmSize) return false;
    }
    return true;
}

void WaydroidDecoder::onQueueFilled(OMX_U32 /* portIndex */) {
    if (mSignalledError || mOutputPortSettingsChange != NONE) return;

    List<BufferInfo *> &inQueue = getPortQueue(kInputPortIndex);
    List<BufferInfo *> &outQueue = getPortQueue(kOutputPortIndex);

    while (!outQueue.empty()) {
        hwdec_reply reply;
        if (mHaveHeldFrame) {
            reply = mHeldFrame;
            mHaveHeldFrame = false;
        } else if (mPending || mDraining) {
            hwdec_output out = outputFor((*outQueue.begin())->mHeader);
            if (mSignalledError) return;
            uint32_t type = mPending ? HWDEC_REQUEST_RECEIVE : HWDEC_REQUEST_DRAIN;
            if (!request(type, &out, sizeof(out), nullptr, 0, -1, &reply))
                return signalError("host connection lost");
        } else if (!inQueue.empty()) {
            BufferInfo *inInfo = *inQueue.begin();
            OMX_BUFFERHEADERTYPE *inHeader = inInfo->mHeader;
            bool eos = inHeader->nFlags & OMX_BUFFERFLAG_EOS;
            if (inHeader->nFilledLen) {
                hwdec_packet packet = {};
                packet.timestamp = inHeader->nTimeStamp;
                if (inHeader->nFlags & OMX_BUFFERFLAG_CODECCONFIG)
                    packet.flags |= HWDEC_PACKET_CONFIG;
                packet.output = outputFor((*outQueue.begin())->mHeader);
                if (mSignalledError) return;
                if (!request(HWDEC_REQUEST_DECODE, &packet, sizeof(packet),
                             inHeader->pBuffer + inHeader->nOffset, inHeader->nFilledLen, -1,
                             &reply))
                    return signalError("host connection lost");
            } else {
                reply = {};
                reply.type = HWDEC_REPLY_NO_FRAME;
            }
            inQueue.erase(inQueue.begin());
            inInfo->mOwnedByUs = false;
            notifyEmptyBufferDone(inHeader);
            if (eos) mDraining = true;
        } else {
            break;
        }

        switch (reply.type) {
            case HWDEC_REPLY_FRAME:
                mPending = reply.pending;
                if (!frameFits(reply, mShmSize) ||
                    (!(reply.flags & HWDEC_FRAME_IN_TARGET) && !mShm))
                    return signalError("invalid frame from the host");
                // The host only writes a frame of the current size into a
                // buffer, so a frame that needs new buffers is in the shared
                // memory and can wait there.
                if (!outputFrame(reply, *outQueue.begin())) {
                    mHeldFrame = reply;
                    mHaveHeldFrame = true;
                    return;
                }
                if (mSignalledError) return;
                break;
            case HWDEC_REPLY_NO_FRAME:
                mPending = reply.pending;
                break;
            case HWDEC_REPLY_EOS: {
                BufferInfo *outInfo = *outQueue.begin();
                OMX_BUFFERHEADERTYPE *outHeader = outInfo->mHeader;
                outHeader->nOffset = 0;
                outHeader->nFilledLen = 0;
                outHeader->nFlags = OMX_BUFFERFLAG_EOS;
                outHeader->nTimeStamp = 0;
                outQueue.erase(outQueue.begin());
                outInfo->mOwnedByUs = false;
                notifyFillBufferDone(outHeader);
                mDraining = false;
                mPending = 0;
                break;
            }
            default:
                return signalError("the host failed to decode");
        }
    }
}

void WaydroidDecoder::resetState() {
    mHaveHeldFrame = false;
    mPending = 0;
    mDraining = false;
    hwdec_reply reply;
    if (mConnected && (!request(HWDEC_REQUEST_FLUSH, nullptr, 0, nullptr, 0, -1, &reply) ||
                       reply.type != HWDEC_REPLY_OK))
        signalError("host flush failed");
}

void WaydroidDecoder::onPortFlushCompleted(OMX_U32 portIndex) {
    if (portIndex == kOutputPortIndex) resetState();
}

void WaydroidDecoder::onPortEnableCompleted(OMX_U32 portIndex, bool enabled) {
    SoftVideoDecoderOMXComponent::onPortEnableCompleted(portIndex, enabled);
    // ACodec frees the buffers of a disabled port
    if (portIndex == kOutputPortIndex && !enabled) clearTargets();
}

void WaydroidDecoder::onReset() {
    SoftVideoDecoderOMXComponent::onReset();
    mSignalledError = false;
    clearTargets();
    resetState();
}

const CodecInfo *findCodec(const char *name) {
    for (const CodecInfo &codec : kCodecs) {
        if (!strcmp(name, codec.name)) return &codec;
    }
    return nullptr;
}

class WaydroidOMXPlugin : public OMXPluginBase {
  public:
    OMX_ERRORTYPE makeComponentInstance(const char *name, const OMX_CALLBACKTYPE *callbacks,
                                        OMX_PTR appData, OMX_COMPONENTTYPE **component) override {
        const CodecInfo *codec = findCodec(name);
        if (!codec) return OMX_ErrorInvalidComponentName;
        sp<SoftOMXComponent> decoder = new WaydroidDecoder(*codec, callbacks, appData, component);
        OMX_ERRORTYPE err = decoder->initCheck();
        if (err != OMX_ErrorNone) return err;
        // owned by the component handle until destroyComponentInstance
        decoder->incStrong(this);
        return OMX_ErrorNone;
    }

    OMX_ERRORTYPE destroyComponentInstance(OMX_COMPONENTTYPE *component) override {
        auto *decoder = static_cast<SoftOMXComponent *>(component->pComponentPrivate);
        decoder->prepareForDestruction();
        decoder->decStrong(this);
        return OMX_ErrorNone;
    }

    OMX_ERRORTYPE enumerateComponents(OMX_STRING name, size_t size, OMX_U32 index) override {
        if (index >= sizeof(kCodecs) / sizeof(kCodecs[0])) return OMX_ErrorNoMore;
        strlcpy(name, kCodecs[index].name, size);
        return OMX_ErrorNone;
    }

    OMX_ERRORTYPE getRolesOfComponent(const char *name, Vector<String8> *roles) override {
        roles->clear();
        const CodecInfo *codec = findCodec(name);
        if (!codec) return OMX_ErrorInvalidComponentName;
        roles->push(String8(codec->role));
        return OMX_ErrorNone;
    }
};

}  // namespace

// OMXStore looks up android::createOMXPlugin when there is no C symbol, and
// HardwareAPI.h declares a C++ ::createOMXPlugin, so define it here.
OMXPluginBase *createOMXPlugin() {
    return new WaydroidOMXPlugin;
}

}  // namespace android
