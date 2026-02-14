#include "mlpStreamReader.h"
#include "nalUnits.h"
#include "tsPacket.h"
#include "vodCoreException.h"
#include "vod_common.h"

int MLPStreamReader::getHeaderLen() { return MLP_HEADER_LEN; }

const std::string MLPStreamReader::getStreamInfo()
{
    std::ostringstream str;

    if (m_subType == MlpSubType::stTRUEHD)
        str << "TRUE-HD";
    else if (m_subType == MlpSubType::stMLP)
        str << "MLP";
    else
        str << "UNKNOWN";

    if (m_substreams == 4)
        str << " + ATMOS";
    str << ". ";
    str << "Peak bitrate: " << m_bitrate / 1000 << "Kbps ";
    str << "Sample Rate: " << m_samplerate / 1000 << "KHz ";
    str << "Channels: " << static_cast<int>(m_channels);
    return str.str();
}

int MLPStreamReader::decodeFrame(uint8_t* buff, uint8_t* end, int& skipBytes, int& skipBeforeBytes)
{
    skipBytes = 0;
    skipBeforeBytes = 0;
    if (MLPCodec::decodeFrame(buff, end))
        return getFrameSize(buff);
    return 0;
}

int MLPStreamReader::getTSDescriptor(uint8_t* dstBuff, bool blurayMode, bool hdmvDescriptors)
{
    // Ensure we have decoded at least one frame so codec parameters are valid
    uint8_t* frame = MLPCodec::findFrame(m_buffer, m_bufEnd);
    if (frame == nullptr)
        return 0;
    int skipBytes = 0;
    int skipBeforeBytes = 0;
    if (decodeFrame(frame, m_bufEnd, skipBytes, skipBeforeBytes) < 1)
        return 0;

    if (hdmvDescriptors)
    {
        // Blu-ray core specifications - HDMV TrueHD/MLP audio registration descriptor
        *dstBuff++ = static_cast<uint8_t>(TSDescriptorTag::REGISTRATION);  // descriptor tag
        *dstBuff++ = 8;                                                    // descriptor length
        *dstBuff++ = 'H';
        *dstBuff++ = 'D';
        *dstBuff++ = 'M';
        *dstBuff++ = 'V';
        *dstBuff++ = 0xff;                                             // stuffing_bits
        *dstBuff++ = static_cast<uint8_t>(StreamType::AUDIO_TRUE_HD);  // stream_coding_type
        const int audio_presentation_type = (m_channels > 2) ? 6 : (m_channels == 2) ? 3 : 1;
        const int sampling_frequency = (m_samplerate == 192000) ? 5 : (m_samplerate == 96000) ? 4 : 1;
        *dstBuff++ = static_cast<uint8_t>(audio_presentation_type << 4 | sampling_frequency);
        *dstBuff++ = 0xff;  // stuffing_bits

        return 10;  // total descriptor length (2 header + 8 data)
    }

    // Non-HDMV: SMPTE-RA registered format identifier for MLP audio
    // https://smpte-ra.org/registered-mpeg-ts-ids
    *dstBuff++ = static_cast<uint8_t>(TSDescriptorTag::REGISTRATION);  // descriptor tag
    *dstBuff++ = 4;                                                    // descriptor length
    *dstBuff++ = 'm';
    *dstBuff++ = 'l';
    *dstBuff++ = 'p';
    *dstBuff++ = 'a';

    return 6;  // total descriptor length
}

int MLPStreamReader::readPacket(AVPacket& avPacket)
{
    while (true)
    {
        const int rez = SimplePacketizerReader::readPacket(avPacket);
        if (rez != 0)
            return rez;

        // thg packet
        avPacket.dts = avPacket.pts = m_totalTHDSamples * INTERNAL_PTS_FREQ / m_samplerate;

        m_totalTHDSamples += m_samples;
        m_demuxedTHDSamples += m_samples;
        if (m_demuxedTHDSamples >= m_samples)
        {
            m_demuxedTHDSamples -= m_samples;
        }
        return 0;
    }
}

int MLPStreamReader::flushPacket(AVPacket& avPacket)
{
    const int rez = SimplePacketizerReader::flushPacket(avPacket);
    if (rez > 0)
    {
        if (!(avPacket.flags & AVPacket::PRIORITY_DATA))
            avPacket.pts = avPacket.dts =
                m_totalTHDSamples * INTERNAL_PTS_FREQ / m_samplerate;  // replace time to a next HD packet
    }
    return rez;
}
