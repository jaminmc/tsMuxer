#include "av1StreamReader.h"

#include <fs/systemlog.h>

#include <cstring>
#include <sstream>

#include "avCodecs.h"
#include "nalUnits.h"
#include "tsPacket.h"
#include "vodCoreException.h"
#include "vod_common.h"

using namespace std;

AV1StreamReader::AV1StreamReader()
    : m_seqHdrFound(false),
      m_firstFrame(true),
      m_lastIFrame(false),
      m_firstFileFrame(false),
      m_frameNum(0),
      m_hdrWcgIdc(3)  // 3 = no indication
{
}

const CodecInfo& AV1StreamReader::getCodecInfo() { return av1CodecInfo; }

unsigned AV1StreamReader::getStreamWidth() const { return m_seqHdrFound ? m_seqHdr.max_frame_width : 0; }

unsigned AV1StreamReader::getStreamHeight() const { return m_seqHdrFound ? m_seqHdr.max_frame_height : 0; }

double AV1StreamReader::getStreamFPS(void*) { return m_seqHdrFound ? m_seqHdr.getFPS() : 0.0; }

void AV1StreamReader::updateStreamFps(void*, uint8_t*, uint8_t*, int)
{
    // AV1 sequence headers are not modified in-place for FPS changes.
    // The manual FPS override is applied via setFPS() from the meta file.
}

void AV1StreamReader::incTimings()
{
    if (m_totalFrameNum++ > 0)
        m_curDts += m_pcrIncPerFrame;
    m_curPts = m_curDts;  // AV1 in TS: PTS is always present
    m_frameNum++;
    m_firstFrame = false;
}

// ---------------------------------------------------------------------------
// checkStream - detect AV1 stream from elementary data
// ---------------------------------------------------------------------------

CheckStreamRez AV1StreamReader::checkStream(uint8_t* buffer, const int len)
{
    CheckStreamRez rez;
    uint8_t* end = buffer + len;

    // Two-pass detection: first find a valid sequence header, then verify
    // we can also find a frame OBU (FRAME or FRAME_HEADER) to confirm this
    // is genuinely AV1 and not random data from another codec (e.g. MPEG-2
    // slice data whose start codes look like AV1 OBU headers).
    Av1SequenceHeader candidateSeqHdr;
    uint8_t* seqHdrNal = nullptr;
    uint8_t* seqHdrNextNal = nullptr;
    bool foundSeqHdr = false;
    bool foundFrame = false;

    for (uint8_t* nal = NALUnit::findNextNAL(buffer, end); nal < end - 2; nal = NALUnit::findNextNAL(nal, end))
    {
        Av1ObuHeader obuHdr;
        const int hdrLen = obuHdr.parse(nal, end);
        if (hdrLen < 0)
            continue;

        uint8_t* nextNal = NALUnit::findNALWithStartCode(nal, end, true);
        if (!m_eof && nextNal == end)
            break;

        // Validate OBU type is a known AV1 type (reject random byte patterns)
        const int typeInt = static_cast<int>(obuHdr.obu_type);
        if (typeInt < 0 || (typeInt > 8 && typeInt != 15))
            continue;

        if (!foundSeqHdr && obuHdr.obu_type == Av1ObuType::SEQUENCE_HEADER)
        {
            // Parse the sequence header (strip OBU header bytes first)
            const uint8_t* payload = nal + hdrLen;
            const int payloadLen = static_cast<int>(nextNal - payload);
            // Trim trailing zeros from next start code
            int trimmedLen = payloadLen;
            while (trimmedLen > 0 && payload[trimmedLen - 1] == 0) trimmedLen--;

            Av1SequenceHeader tmpHdr;
            if (tmpHdr.deserialize(payload, trimmedLen) == 0)
            {
                // Additional sanity: aspect ratio must be reasonable
                const double aspect =
                    static_cast<double>(tmpHdr.max_frame_width) / tmpHdr.max_frame_height;
                if (aspect < 0.1 || aspect > 20.0)
                    continue;

                candidateSeqHdr = tmpHdr;
                seqHdrNal = nal;
                seqHdrNextNal = nextNal;
                foundSeqHdr = true;
            }
        }
        else if (obuHdr.obu_type == Av1ObuType::FRAME ||
                 obuHdr.obu_type == Av1ObuType::FRAME_HEADER)
        {
            foundFrame = true;
        }

        // Once we have both a sequence header and a frame, we're confident
        if (foundSeqHdr && foundFrame)
            break;
    }

    if (foundSeqHdr && foundFrame)
    {
        m_seqHdr = candidateSeqHdr;
        m_seqHdrFound = true;
        m_spsPpsFound = true;
        updateFPS(nullptr, seqHdrNal, seqHdrNextNal, 0);

        rez.codecInfo = av1CodecInfo;
        ostringstream str;
        str << "Profile: " << static_cast<int>(m_seqHdr.seq_profile);
        str << "  Level: " << static_cast<int>(m_seqHdr.seq_level_idx_0);
        str << "  Resolution: " << m_seqHdr.max_frame_width << "x" << m_seqHdr.max_frame_height;
        str << "  Bit depth: " << m_seqHdr.getBitDepth() << "bit";
        str << "  Frame rate: ";
        if (m_seqHdr.getFPS() > 0)
            str << m_seqHdr.getFPS();
        else
            str << "not found";
        rez.streamDescr = str.str();
    }

    m_totalFrameNum = m_frameNum = 0;
    m_curDts = m_curPts = 0;

    return rez;
}

// ---------------------------------------------------------------------------
// intDecodeNAL - process OBUs between start codes
// ---------------------------------------------------------------------------

int AV1StreamReader::intDecodeNAL(uint8_t* buff)
{
    bool frameFound = false;
    m_spsPpsFound = false;
    m_lastIFrame = false;

    const uint8_t* prevPos = nullptr;
    uint8_t* curPos = buff;
    uint8_t* nextNal = NALUnit::findNextNAL(curPos, m_bufEnd);

    if (!m_eof && nextNal == m_bufEnd)
        return NOT_ENOUGH_BUFFER;

    while (curPos < m_bufEnd)
    {
        Av1ObuHeader obuHdr;
        const int hdrLen = obuHdr.parse(curPos, m_bufEnd);
        if (hdrLen < 0)
        {
            // Invalid OBU, skip to next
            prevPos = curPos;
            curPos = nextNal;
            nextNal = NALUnit::findNextNAL(curPos, m_bufEnd);
            if (!m_eof && nextNal == m_bufEnd)
                return NOT_ENOUGH_BUFFER;
            continue;
        }

        // Calculate payload (between OBU header and next start code)
        uint8_t* nextNalWithStartCode = nextNal;
        // Back up past trailing zeros that are part of the next start code
        while (nextNalWithStartCode > curPos + hdrLen && nextNalWithStartCode[-1] == 0) nextNalWithStartCode--;

        const uint8_t* payload = curPos + hdrLen;
        const int payloadLen = static_cast<int>(nextNalWithStartCode - payload);

        switch (obuHdr.obu_type)
        {
        case Av1ObuType::SEQUENCE_HEADER:
            if (payloadLen > 0 && m_seqHdr.deserialize(payload, payloadLen) == 0)
            {
                m_seqHdrFound = true;
                m_spsPpsFound = true;
                updateFPS(nullptr, curPos, nextNalWithStartCode, 0);

                // Store the sequence header for writeAdditionData
                if (nextNalWithStartCode > curPos)
                {
                    const int dataLen = static_cast<int>(nextNalWithStartCode - curPos);
                    m_seqHdrBuffer.resize(dataLen);
                    memcpy(m_seqHdrBuffer.data(), curPos, dataLen);
                }

                // Determine HDR/WCG indicator from color properties
                if (m_seqHdr.transfer_characteristics == AV1_TC_PQ ||
                    m_seqHdr.transfer_characteristics == AV1_TC_HLG)
                {
                    // HDR content
                    if (m_seqHdr.color_primaries == AV1_CP_BT_2020)
                        m_hdrWcgIdc = 2;  // Both HDR and WCG
                    else
                        m_hdrWcgIdc = 2;  // HDR (assume WCG too for PQ/HLG)
                }
                else if (m_seqHdr.color_primaries == AV1_CP_BT_2020)
                {
                    m_hdrWcgIdc = 1;  // WCG only
                }
                else if (m_seqHdr.color_primaries == AV1_CP_BT_709 || m_seqHdr.color_primaries == AV1_CP_BT_601)
                {
                    m_hdrWcgIdc = 0;  // SDR
                }
                else
                {
                    m_hdrWcgIdc = 3;  // No indication
                }
            }
            break;

        case Av1ObuType::TEMPORAL_DELIMITER:
            // A temporal delimiter signals the start of a new access unit.
            // If we already found a frame, this is the boundary.
            if (frameFound)
            {
                m_lastDecodedPos = prevPos;
                incTimings();
                return 0;
            }
            break;

        case Av1ObuType::FRAME:
        case Av1ObuType::FRAME_HEADER:
        {
            // Parse frame header for keyframe detection
            if (m_seqHdrFound && payloadLen > 0)
            {
                Av1FrameHeader frmHdr;
                if (frmHdr.deserialize(payload, payloadLen, m_seqHdr) == 0)
                {
                    if (frmHdr.frame_type == Av1FrameType::KEY_FRAME ||
                        frmHdr.frame_type == Av1FrameType::INTRA_ONLY_FRAME)
                    {
                        m_lastIFrame = true;
                    }
                }
            }

            if (frameFound)
            {
                // Another frame found - boundary
                m_lastDecodedPos = prevPos;
                incTimings();
                return 0;
            }
            frameFound = true;
            break;
        }

        case Av1ObuType::TILE_GROUP:
            // Tile groups belong to the current frame
            break;

        default:
            break;
        }

        prevPos = curPos;
        curPos = nextNal;
        nextNal = NALUnit::findNextNAL(curPos, m_bufEnd);

        if (!m_eof && nextNal == m_bufEnd)
            return NOT_ENOUGH_BUFFER;
    }

    if (m_eof)
    {
        if (frameFound)
            incTimings();
        m_lastDecodedPos = m_bufEnd;
        return 0;
    }
    return NEED_MORE_DATA;
}

// ---------------------------------------------------------------------------
// getTSDescriptor - AV1 registration descriptor + AV1 video descriptor
// ---------------------------------------------------------------------------

int AV1StreamReader::getTSDescriptor(uint8_t* dstBuff, bool blurayMode, const bool hdmvDescriptors)
{
    if (m_firstFrame)
        checkStream(m_buffer, static_cast<int>(m_bufEnd - m_buffer));

    uint8_t* start = dstBuff;

    // 1. Registration descriptor (AOM spec Section 2.1)
    // descriptor_tag = 0x05, descriptor_length = 4, format_identifier = 'AV01'
    *dstBuff++ = static_cast<uint8_t>(TSDescriptorTag::REGISTRATION);  // 0x05
    *dstBuff++ = 4;                                                    // descriptor length
    *dstBuff++ = 'A';
    *dstBuff++ = 'V';
    *dstBuff++ = '0';
    *dstBuff++ = '1';

    // 2. AV1 video descriptor (AOM spec Section 2.2)
    // descriptor_tag = 0x80, descriptor_length = 4
    *dstBuff++ = 0x80;  // AV1 video descriptor tag
    *dstBuff++ = 4;     // descriptor length

    // Byte 0: marker(1) + version(7)
    *dstBuff++ = 0x81;  // marker=1, version=1

    // Byte 1: seq_profile(3) + seq_level_idx_0(5)
    *dstBuff++ = static_cast<uint8_t>((m_seqHdr.seq_profile << 5) | (m_seqHdr.seq_level_idx_0 & 0x1f));

    // Byte 2: seq_tier_0(1) + high_bitdepth(1) + twelve_bit(1) + monochrome(1) +
    //          chroma_subsampling_x(1) + chroma_subsampling_y(1) + chroma_sample_position(2)
    *dstBuff++ = static_cast<uint8_t>((m_seqHdr.seq_tier_0 << 7) | (m_seqHdr.high_bitdepth << 6) |
                                      (m_seqHdr.twelve_bit << 5) | (m_seqHdr.mono_chrome << 4) |
                                      (m_seqHdr.chroma_subsampling_x << 3) | (m_seqHdr.chroma_subsampling_y << 2) |
                                      (m_seqHdr.chroma_sample_position & 0x03));

    // Byte 3: hdr_wcg_idc(2) + reserved(1) + initial_presentation_delay_present(1) + reserved(4)
    *dstBuff++ = static_cast<uint8_t>((m_hdrWcgIdc & 0x03) << 6);

    return static_cast<int>(dstBuff - start);
}

// ---------------------------------------------------------------------------
// writeAdditionData - insert Sequence Header OBU before first frame
// ---------------------------------------------------------------------------

uint8_t* AV1StreamReader::writeNalPrefix(uint8_t* curPos) const
{
    if (!m_shortStartCodes)
        *curPos++ = 0;
    *curPos++ = 0;
    *curPos++ = 0;
    *curPos++ = 1;
    return curPos;
}

uint8_t* AV1StreamReader::writeBuffer(MemoryBlock& srcData, uint8_t* dstBuffer, const uint8_t* dstEnd) const
{
    if (srcData.isEmpty())
        return dstBuffer;
    const size_t bytesLeft = dstEnd - dstBuffer;
    const size_t requiredBytes = srcData.size() + 3 + (m_shortStartCodes ? 0 : 1);
    if (bytesLeft < requiredBytes)
        return dstBuffer;

    dstBuffer = writeNalPrefix(dstBuffer);
    memcpy(dstBuffer, srcData.data(), srcData.size());
    dstBuffer += srcData.size();
    return dstBuffer;
}

int AV1StreamReader::writeAdditionData(uint8_t* dstBuffer, uint8_t* dstEnd, AVPacket& avPacket,
                                       PriorityDataInfo* priorityData)
{
    uint8_t* curPos = dstBuffer;

    const bool needInsSeqHdr = m_firstFileFrame && !(avPacket.flags & AVPacket::IS_SPS_PPS_IN_GOP);
    if (needInsSeqHdr)
    {
        avPacket.flags |= AVPacket::IS_SPS_PPS_IN_GOP;
        curPos = writeBuffer(m_seqHdrBuffer, curPos, dstEnd);
    }

    m_firstFileFrame = false;
    return static_cast<int>(curPos - dstBuffer);
}
