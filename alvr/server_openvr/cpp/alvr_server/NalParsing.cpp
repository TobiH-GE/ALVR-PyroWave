
#include "Logger.h"
#include "Settings.h"
#include "Utils.h"
#include "bindings.h"
#include <mutex>
#include <vector>
#include <string.h>

static const char NAL_PREFIX_3B[] = { 0x00, 0x00, 0x01 };
static const char NAL_PREFIX_4B[] = { 0x00, 0x00, 0x00, 0x01 };

static const unsigned char H264_NAL_TYPE_SPS = 7;
static const unsigned char HEVC_NAL_TYPE_VPS = 32;

static const unsigned char H264_NAL_TYPE_AUD = 9;
static const unsigned char HEVC_NAL_TYPE_AUD = 35;

int8_t getNalPrefixSize(unsigned char* buf) {
    if (memcmp(buf, NAL_PREFIX_3B, sizeof(NAL_PREFIX_3B)) == 0) {
        return sizeof(NAL_PREFIX_3B);
    } else if (memcmp(buf, NAL_PREFIX_4B, sizeof(NAL_PREFIX_4B)) == 0) {
        return sizeof(NAL_PREFIX_4B);
    } else {
        return -1;
    }
}

/*
Sends the (VPS + )SPS + PPS video configuration headers from H.264 or H.265 stream as a sequence of
NALs. (VPS + )SPS + PPS have short size (8bytes + 28bytes in some environment), so we can assume
SPS + PPS is contained in first fragment.
*/
void sendHeaders(int codec, unsigned char*& buf, int& len, int nalNum) {
    unsigned char* cursor = buf;
    int headersLen = 0;
    int foundHeaders = -1; // Offset by 1 header to find the length until the next header

    while (headersLen <= len) {
        if (headersLen + sizeof(NAL_PREFIX_4B) > (unsigned)len) {
            cursor++;
            headersLen++;
            continue;
        }
        int8_t prefixSize = getNalPrefixSize(cursor);
        if (prefixSize == -1) {
            cursor++;
            headersLen++;
            continue;
        }
        foundHeaders++;
        if (foundHeaders == nalNum) {
            break;
        }
        headersLen += prefixSize;
        cursor += prefixSize;
    }

    if (foundHeaders != nalNum) {
        return;
    }

    SetVideoConfigNals((const unsigned char*)buf, headersLen, codec);

    // move the cursor forward excluding config NALs
    buf = cursor;
    len -= headersLen;
}

void processH264Nals(unsigned char*& buf, int& len) {
    unsigned char prefixSize = getNalPrefixSize(buf);
    unsigned char nalType = buf[prefixSize] & 0x1F;

    if (nalType == H264_NAL_TYPE_AUD && len > prefixSize * 2 + 2) {
        buf += prefixSize + 2;
        len -= prefixSize + 2;
        prefixSize = getNalPrefixSize(buf);
        nalType = buf[prefixSize] & 0x1F;
    }
    if (nalType == H264_NAL_TYPE_SPS) {
        sendHeaders(ALVR_CODEC_H264, buf, len, 2); // 2 headers SPS and PPS
    }
}

void processHevcNals(unsigned char*& buf, int& len) {
    unsigned char prefixSize = getNalPrefixSize(buf);
    unsigned char nalType = (buf[prefixSize] >> 1) & 0x3F;

    if (nalType == HEVC_NAL_TYPE_AUD && len > prefixSize * 2 + 3) {
        buf += prefixSize + 3;
        len -= prefixSize + 3;
        prefixSize = getNalPrefixSize(buf);
        nalType = (buf[prefixSize] >> 1) & 0x3F;
    }
    if (nalType == HEVC_NAL_TYPE_VPS) {
        sendHeaders(ALVR_CODEC_HEVC, buf, len, 3); // 3 headers VPS, SPS and PPS
    }
}

void ParseFrameNals(
    int codec, unsigned char* buf, int len, unsigned long long targetTimestampNs, bool isIdr
) {
    static bool av1GotFrame = false;

    if ((unsigned)len < sizeof(NAL_PREFIX_4B)) {
        return;
    }

    if (codec == ALVR_CODEC_H264) {
        processH264Nals(buf, len);
    } else if (codec == ALVR_CODEC_HEVC) {
        processHevcNals(buf, len);
    } else if (codec == ALVR_CODEC_AV1 && !av1GotFrame) {
        av1GotFrame = true;
        SetVideoConfigNals(0, 0, codec);
    }

    VideoSend(targetTimestampNs, buf, len, isIdr);
}

void ParseFrameNalsMultiview(
    const unsigned char* const* views,
    const int* lengths,
    int count,
    unsigned long long targetTimestampNs,
    bool isIdr
) {
    static const unsigned char HEVC_NAL_TYPE_SPS = 33, HEVC_NAL_TYPE_PPS = 34;
    static std::vector<unsigned char> config, frame;
    config.clear();
    frame.clear();
    for (int v = 0; v < count; v++) {
        const unsigned char* buf = views[v];
        const int len = lengths[v];
        // The NAL units of this view's output, each written back with a 4 byte start code.
        int i = 0;
        while (i + 3 <= len) {
            if (!(buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1)) {
                i++;
                continue;
            }
            const int start = i + 3;
            int end = start;
            while (end + 3 <= len && !(buf[end] == 0 && buf[end + 1] == 0 && (buf[end + 2] == 1 || (buf[end + 2] == 0 && end + 3 < len && buf[end + 3] == 1)))) {
                end++;
            }
            if (end + 3 > len) {
                end = len;
            }
            i = end;
            if (end - start < 2) {
                continue;
            }
            const unsigned char type = (buf[start] >> 1) & 0x3F;
            // One access unit delimiter per access unit: the second view's is left out.
            if (type == HEVC_NAL_TYPE_AUD && v > 0) {
                continue;
            }
            auto& out = type == HEVC_NAL_TYPE_VPS || type == HEVC_NAL_TYPE_SPS || type == HEVC_NAL_TYPE_PPS ? config : frame;
            out.insert(out.end(), NAL_PREFIX_4B, NAL_PREFIX_4B + sizeof(NAL_PREFIX_4B));
            out.insert(out.end(), buf + start, buf + end);
        }
    }
    if (!config.empty()) {
        SetVideoConfigNals(config.data(), (int)config.size(), ALVR_CODEC_HEVC);
    }
    if (!frame.empty()) {
        VideoSend(targetTimestampNs, frame.data(), (int)frame.size(), isIdr);
    }
}

void ParseFrameNalsOfEye(
    int codec,
    unsigned char* buf,
    int len,
    unsigned long long targetTimestampNs,
    bool isIdr,
    int eye,
    bool firstPiece,
    bool lastPiece
) {
    if ((unsigned)len < sizeof(NAL_PREFIX_4B)) {
        return;
    }
    // Both halves carry the same configuration (same size and settings), sent once per IDR.
    if (codec == ALVR_CODEC_H264) {
        processH264Nals(buf, len);
    } else if (codec == ALVR_CODEC_HEVC) {
        processHevcNals(buf, len);
    }
    static std::vector<unsigned char> packet;
    packet.resize(size_t(len) + 4);
    packet[0] = SplitEyePrefix0;
    packet[1] = SplitEyePrefix1;
    packet[2] = 0;
    packet[3] = (unsigned char)eye;
    memcpy(packet.data() + 4, buf, size_t(len));
    const unsigned int size = (unsigned int)packet.size();
    VideoSendPackets(targetTimestampNs, packet.data(), &size, 1, isIdr, firstPiece, lastPiece);
}
