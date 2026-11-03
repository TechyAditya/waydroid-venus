/*
 * OMX video decoders that decode on the host GPU through waydroid-hwdec.
 * Built as Waydroid's vendor OMX plugin (libstagefrighthw.so), which the
 * 32-bit media.codec service loads.  The components reuse the decoder
 * plumbing of Google's soft OMX decoders and only replace decoding with
 * requests to the host; see ../protocol.h.
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

#include <log/log.h>
#include <media/hardware/OMXPluginBase.h>
#include <media/openmax/OMX_IndexExt.h>
#include <media/stagefright/foundation/ColorUtils.h>
#include <media/stagefright/omx/SoftVideoDecoderOMXComponent.h>

#include "../protocol.h"

namespace android {

namespace {

constexpr char kSocketPath[] = "/data/venus/" HWDEC_SOCKET_NAME;
constexpr OMX_U32 kNumBuffers = 8;
constexpr OMX_U32 kMinInputBufferSize = 2 << 20;
constexpr OMX_U32 kMinCompressionRatio = 2;
// a stuck host must not hang media.codec forever
constexpr int kTimeoutSeconds = 5;

const CodecProfileLevel kAvcProfileLevels[] = {
    {OMX_VIDEO_AVCProfileConstrainedBaseline, OMX_VIDEO_AVCLevel52},
    {OMX_VIDEO_AVCProfileBaseline, OMX_VIDEO_AVCLevel52},
    {OMX_VIDEO_AVCProfileMain, OMX_VIDEO_AVCLevel52},
    {OMX_VIDEO_AVCProfileConstrainedHigh, OMX_VIDEO_AVCLevel52},
    {OMX_VIDEO_AVCProfileHigh, OMX_VIDEO_AVCLevel52},
};

const CodecProfileLevel kHevcProfileLevels[] = {
    {OMX_VIDEO_HEVCProfileMain, OMX_VIDEO_HEVCMainTierLevel52},
    {OMX_VIDEO_HEVCProfileMainStill, OMX_VIDEO_HEVCMainTierLevel52},
};

const CodecProfileLevel kVp8ProfileLevels[] = {
    {OMX_VIDEO_VP8ProfileMain, OMX_VIDEO_VP8Level_Version0},
};

const CodecProfileLevel kVp9ProfileLevels[] = {
    {OMX_VIDEO_VP9Profile0, OMX_VIDEO_VP9Level52},
};

const CodecProfileLevel kAv1ProfileLevels[] = {
    {OMX_VIDEO_AV1ProfileMain8, OMX_VIDEO_AV1Level52},
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

    OMX_ERRORTYPE internalGetParameter(OMX_INDEXTYPE index, OMX_PTR params) override;
    void onQueueFilled(OMX_U32 portIndex) override;
    void onPortFlushCompleted(OMX_U32 portIndex) override;
    void onReset() override;

    int getColorAspectPreference() override { return kPreferBitstream; }

  private:
    int mFd = -1;
    bool mConnected = false;
    uint8_t *mShm = nullptr;
    size_t mShmSize = 0;
    bool mSignalledError = false;
    // the input reached end of stream; ask the host for the remaining frames
    bool mDraining = false;
    // frames the host has queued for us
    uint32_t mPending = 0;
    // a frame waiting in the shared memory for the output port to reconfigure
    bool mHaveHeldFrame = false;
    hwdec_reply mHeldFrame = {};

    bool connectHost(uint32_t codec);
    bool request(uint32_t type, const hwdec_packet *packet, const void *data, uint32_t size,
                 hwdec_reply *reply);
    bool receiveReply(hwdec_reply *reply);
    void unmapShm();
    void signalError(const char *what);
    void resetState();
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
    if (!request(HWDEC_REQUEST_OPEN, nullptr, &open, sizeof(open), &reply) ||
        reply.type != HWDEC_REPLY_OK) {
        ALOGE("the host cannot open decoder %u", codec);
        return false;
    }
    return true;
}

bool WaydroidDecoder::request(uint32_t type, const hwdec_packet *packet, const void *data,
                              uint32_t size, hwdec_reply *reply) {
    hwdec_request header = {type, (packet ? (uint32_t)sizeof(*packet) : 0) + size};
    iovec iov[3];
    int count = 0;
    iov[count++] = {&header, sizeof(header)};
    if (packet) iov[count++] = {const_cast<hwdec_packet *>(packet), sizeof(*packet)};
    if (size) iov[count++] = {const_cast<void *>(data), size};

    for (int i = 0; i < count;) {
        ssize_t n = writev(mFd, iov + i, count - i);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            ALOGE("send to the host: %s", n < 0 ? strerror(errno) : "closed");
            return false;
        }
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

bool WaydroidDecoder::outputFrame(const hwdec_reply &frame, BufferInfo *outInfo) {
    ColorAspects aspects;
    ColorUtils::convertIsoColorAspectsToCodecAspects(frame.primaries, frame.transfer, frame.matrix,
                                                     frame.full_range, aspects);
    if (colorAspectsDiffer(aspects, mBitstreamColorAspects)) {
        mBitstreamColorAspects = aspects;
        handleColorAspectsChange();
    }

    bool portWillReset = false;
    handlePortSettingsChange(&portWillReset, frame.width, frame.height);
    if (portWillReset) return false;

    OMX_BUFFERHEADERTYPE *outHeader = outInfo->mHeader;
    size_t size = (size_t)outputBufferWidth() * outputBufferHeight() * 3 / 2;
    if (outHeader->nAllocLen < size) {
        signalError("output buffer too small");
        return true;
    }
    copyYV12FrameToOutputBuffer(outHeader->pBuffer, mShm + frame.offset[0], mShm + frame.offset[1],
                                mShm + frame.offset[2], frame.stride[0], frame.stride[1],
                                frame.stride[2]);
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

OMX_ERRORTYPE WaydroidDecoder::internalGetParameter(OMX_INDEXTYPE index, OMX_PTR params) {
    // Frames come out in byte buffers, so ACodec has to copy them to the
    // surface itself.
    if ((int)index == OMX_IndexParamVideoAndroidRequiresSwRenderer) {
        auto *param = static_cast<OMX_PARAM_U32TYPE *>(params);
        if (!isValidOMXParam(param)) return OMX_ErrorBadParameter;
        if (param->nPortIndex != kOutputPortIndex) return OMX_ErrorBadPortIndex;
        param->nU32 = 1;
        return OMX_ErrorNone;
    }
    return SoftVideoDecoderOMXComponent::internalGetParameter(index, params);
}

static bool frameFits(const hwdec_reply &f, size_t shmSize) {
    if (!f.width || !f.height || f.width > HWDEC_MAX_DIMENSION || f.height > HWDEC_MAX_DIMENSION)
        return false;
    uint32_t rows[3] = {f.height, (f.height + 1) / 2, (f.height + 1) / 2};
    uint32_t widths[3] = {f.width, (f.width + 1) / 2, (f.width + 1) / 2};
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
        } else if (mPending) {
            if (!request(HWDEC_REQUEST_RECEIVE, nullptr, nullptr, 0, &reply))
                return signalError("host connection lost");
        } else if (mDraining) {
            if (!request(HWDEC_REQUEST_DRAIN, nullptr, nullptr, 0, &reply))
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
                if (!request(HWDEC_REQUEST_DECODE, &packet, inHeader->pBuffer + inHeader->nOffset,
                             inHeader->nFilledLen, &reply))
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
                if (!mShm || !frameFits(reply, mShmSize))
                    return signalError("invalid frame from the host");
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
    if (mConnected && (!request(HWDEC_REQUEST_FLUSH, nullptr, nullptr, 0, &reply) ||
                       reply.type != HWDEC_REPLY_OK))
        signalError("host flush failed");
}

void WaydroidDecoder::onPortFlushCompleted(OMX_U32 portIndex) {
    if (portIndex == kOutputPortIndex) resetState();
}

void WaydroidDecoder::onReset() {
    SoftVideoDecoderOMXComponent::onReset();
    mSignalledError = false;
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
