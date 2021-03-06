#include <malloc.h>
#include "FlvMuxer.h"

FlvMuxer::FlvMuxer() {
    rtmp = RTMP_Alloc();
    RTMP_Init(rtmp);
}

FlvMuxer::~FlvMuxer() {
    RTMP_Free(rtmp);
}

int FlvMuxer::start(char *url, int timeout) {
    if (RTMP_IsConnected(rtmp)) {
        stop();
    }

    rtmp->Link.timeout = timeout;

    RTMP_SetupURL(rtmp, url);
    RTMP_EnableWrite(rtmp);

    if (!RTMP_Connect(rtmp, NULL)) {
        debug_print("Connect failure !!!");
        return -1;
    }

    debug_print("Connected with server !!!");

    if (!RTMP_ConnectStream(rtmp, 0)) {
        debug_print("Connect stream failure !!!");
        return -1;
    }

    debug_print("Connected stream !!!");

    return 0;
}

int FlvMuxer::stop() {
    RTMP_Close(rtmp);
    return 0;
}

int FlvMuxer::isPlaying() {
    return RTMP_IsConnected(rtmp);
}

int FlvMuxer::writeVideoData(uint8_t *data, int length, long timestamp) {
    /* Remove NAL start prefix bytes */
    int nalStartPrefixBytes = 4; // 00 00 00 01 : 4 bytes
    if (data[2] == 0x01) { // 00 00 01 : 3 bytes
        nalStartPrefixBytes = 3;
    }

    data += nalStartPrefixBytes;
    length -= nalStartPrefixBytes;

    int type = data[0] & 0x1F;
    RTMPPacket *packet;
    if (type == NAL_SPS) {
        // 103, 66, -64, 13, -38, 5, -126, 90, 1, -31, 16, -115, 64, 0, 0, 0, 1, 104, -50, 6, -30]
        // SPS : length - NAL start prefix bytes - PPS bytes
        // NAL start prefix bytes
        // PPS : last 4 bytes
        int ppsLength = 4;
        int spsLength = length - nalStartPrefixBytes - ppsLength;

        packet = buildSpsPpsPacket(&data[0], spsLength, &data[spsLength + nalStartPrefixBytes], ppsLength);
    } else if (type == NAL_SLICE || type == NAL_SLICE_IDR) {
        packet = buildVideoPacket(type, data, length, timestamp);
    }

    if (RTMP_IsConnected(rtmp)) {
        RTMP_SendPacket(rtmp, packet, TRUE);
    }

    free(packet->m_body);
    free(packet);

    return 0;
}

int FlvMuxer::writeAudioData(uint8_t *data, int length, long timestamp) {
    RTMPPacket *packet = buildAudioPacket(data, length, timestamp);

    if (RTMP_IsConnected(rtmp)) {
        RTMP_SendPacket(rtmp, packet, TRUE);
    }

    free(packet->m_body);
    free(packet);

    return 0;
}

RTMPPacket *
FlvMuxer::buildSpsPpsPacket(uint8_t *sps, int spsLength, uint8_t *pps, int ppsLength) {
    RTMPPacket *packet = (RTMPPacket *) malloc(sizeof(RTMPPacket));
    memset(packet, 0, sizeof(RTMPPacket));

    packet->m_headerType = RTMP_PACKET_SIZE_MEDIUM;
    packet->m_packetType = RTMP_PACKET_TYPE_VIDEO;
    packet->m_hasAbsTimestamp = 0;
    packet->m_nChannel = STREAM_CHANNEL_VIDEO;
    packet->m_nTimeStamp = 0;
    packet->m_nInfoField2 = rtmp->m_stream_id;
    packet->m_nBodySize = spsLength + ppsLength + 16;
    packet->m_body = (char *) malloc(packet->m_nBodySize);
    memset(packet->m_body, 0, packet->m_nBodySize);

    int index = 0;
    uint8_t *body = (uint8_t *) packet->m_body;

    body[index++] = 0x17; // 1:Key Frame 7:AVC (H.264)
    body[index++] = 0x00; // AVC sequence header

    /* Start Time */
    body[index++] = 0x00;
    body[index++] = 0x00;
    body[index++] = 0x00;

    /* AVC Decoder Configuration Record */
    body[index++] = 0x01;   // Configuration Version
    body[index++] = sps[1]; // AVC Profile Indication
    body[index++] = sps[2]; // Profile Compatibility
    body[index++] = sps[3]; // AVC Level Indication
    body[index++] = 0xFF;   // Length Size Minus One

    /* Sequence Parameter Sets */
    body[index++] = 0xE1; // Num Of Sequence Parameter Sets
    /* SPS Length */
    body[index++] = (spsLength >> 8) & 0xFF;
    body[index++] = spsLength & 0xFF;
    /* SPS Data*/
    memcpy(&body[index], sps, spsLength);
    index += spsLength;

    /* Picture Parameter Sets */
    body[index++] = 0x01; // Num Of Picture Parameter Sets
    /* PPS Length */
    body[index++] = (ppsLength >> 8) & 0xFF;
    body[index++] = ppsLength & 0xFF;
    /* PPS Data */
    memcpy(&body[index], pps, ppsLength);

    return packet;
}

RTMPPacket *FlvMuxer::buildVideoPacket(int nalType, uint8_t *data, int length, long timestamp) {
    RTMPPacket *packet = (RTMPPacket *) malloc(sizeof(RTMPPacket));
    memset(packet, 0, sizeof(RTMPPacket));

    packet->m_headerType = RTMP_PACKET_SIZE_LARGE;
    packet->m_packetType = RTMP_PACKET_TYPE_VIDEO;
    packet->m_hasAbsTimestamp = 0;
    packet->m_nChannel = STREAM_CHANNEL_VIDEO;
    packet->m_nTimeStamp = timestamp;
    packet->m_nInfoField2 = rtmp->m_stream_id;
    packet->m_nBodySize = length + 9;
    packet->m_body = (char *) malloc(packet->m_nBodySize);
    memset(packet->m_body, 0, packet->m_nBodySize);

    uint8_t *body = (uint8_t *) packet->m_body;

    // Key Frame
    body[0] = (nalType == NAL_SLICE_IDR) ? 0x17 : 0x27;

    body[1] = NAL_SLICE; // NAL_UNIT
    body[2] = 0x00;
    body[3] = 0x00;
    body[4] = 0x00;

    body[5] = (length >> 24) & 0xff;
    body[6] = (length >> 16) & 0xff;
    body[7] = (length >> 8) & 0xff;
    body[8] = (length) & 0xff;

    /* Video Data */
    memcpy(&body[9], data, length);

    return packet;
}

RTMPPacket *FlvMuxer::buildAudioPacket(uint8_t *data, int length, long timestamp) {
    RTMPPacket *packet = (RTMPPacket *) malloc(sizeof(RTMPPacket));
    memset(packet, 0, sizeof(RTMPPacket));

    packet->m_headerType = RTMP_PACKET_SIZE_LARGE;
    packet->m_packetType = RTMP_PACKET_TYPE_AUDIO;
    packet->m_hasAbsTimestamp = 0;
    packet->m_nChannel = STREAM_CHANNEL_AUDIO;
    packet->m_nTimeStamp = (length == 2) ? 0 : timestamp; // 'length == 2' : AAC spec data
    packet->m_nInfoField2 = rtmp->m_stream_id;
    packet->m_nBodySize = length + 2;
    packet->m_body = (char *) malloc(packet->m_nBodySize);
    memset(packet->m_body, 0, packet->m_nBodySize);

    uint8_t *body = (uint8_t *) packet->m_body;

    /* AAC RAW Data */
    // FLV audio sequence header/config frame (starts with 0xAF 0x00)
    body[0] = 0xAF;
    body[1] = 0x00;

    /* AAC Data */
    memcpy(&body[2], data, length);

    return packet;
}