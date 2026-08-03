#include "trueHDAC3MergeReader.h"

#include <fs/systemlog.h>

#include <sstream>

#include "abstractStreamReader.h"
#include "avCodecs.h"
#include "pesPacket.h"
#include "vodCoreException.h"
#include "vod_common.h"

TrueHDAC3MergeReader::TrueHDAC3MergeReader(const std::map<std::string, std::string>& addParams)
    : m_mergeAc3Pid(0),
      m_mergeFromFile(false),
      m_useNewStyleAudioPES(false),
      m_ac3Eof(false),
      m_thdDemuxWaitAc3(true),
      m_demuxedTHDSamplesForAc3(0),
      m_nextAc3Time(0),
      m_ac3SamplesPerSyncFrame(0),
      m_pendingEmitSamples(0),
      m_pendingEmitSampleRate(0)
{
    const auto itTrack = addParams.find("merge-ac3-track");
    const auto itFile = addParams.find("merge-ac3-file");
    if ((itTrack == addParams.end() || itTrack->second.empty()) &&
        (itFile == addParams.end() || itFile->second.empty()))
        THROW(ERR_INVALID_CODEC_FORMAT, "internal: TrueHDAC3MergeReader without merge-ac3-* source")
    if (itTrack != addParams.end() && !itTrack->second.empty())
        m_mergeAc3Pid = strToInt32(itTrack->second.c_str());
    if (itFile != addParams.end() && !itFile->second.empty())
        m_mergeFromFile = true;
}

const CodecInfo& TrueHDAC3MergeReader::getCodecInfo() { return trueHDCodecInfo; }

void TrueHDAC3MergeReader::setAc3SideData(const uint8_t* data, const uint32_t len)
{
    if (data == nullptr || len == 0)
        return;
    const size_t off = m_ac3Accum.size();
    m_ac3Accum.resize(off + len);
    memcpy(m_ac3Accum.data() + off, data, len);
    extractAc3FramesFromAccum();
}

void TrueHDAC3MergeReader::setAc3Eof()
{
    m_ac3Eof = true;
    extractAc3FramesFromAccum();
}

void TrueHDAC3MergeReader::preserveUnconsumedThd()
{
    if (m_curPos >= m_bufEnd)
        return;
    const size_t left = static_cast<size_t>(m_bufEnd - m_curPos);
    if (left > m_tmpBuffer.size())
        m_tmpBuffer.resize(left);
    memmove(m_tmpBuffer.data(), m_curPos, left);
    m_tmpBufferLen = static_cast<int64_t>(left);
    m_curPos = m_bufEnd;
}

void TrueHDAC3MergeReader::restorePreservedThd()
{
    if (m_tmpBufferLen <= 0)
        return;
    m_curPos = m_buffer = m_tmpBuffer.data();
    m_bufEnd = m_buffer + m_tmpBufferLen;
    m_tmpBufferLen = 0;
}

int TrueHDAC3MergeReader::requestMoreAc3Data()
{
    if (m_ac3Eof)
        return -1;
    // SimplePacketizerReader::setBuffer replaces the active buffer. Preserve any
    // unconsumed TrueHD so the next setBuffer appends instead of dropping it.
    preserveUnconsumedThd();
    return AbstractStreamReader::NEED_MORE_DATA;
}

void TrueHDAC3MergeReader::extractAc3FramesFromAccum()
{
    while (!m_ac3Accum.empty())
    {
        uint8_t* start = m_ac3Accum.data();
        uint8_t* end = start + m_ac3Accum.size();
        uint8_t* frame = m_ac3Parser.findAc3Sync(start, end);
        if (frame == nullptr)
        {
            if (m_ac3Accum.size() > 65536)
                m_ac3Accum.erase(m_ac3Accum.begin(), m_ac3Accum.begin() + (m_ac3Accum.size() - 4096));
            return;
        }
        if (frame > start)
        {
            m_ac3Accum.erase(m_ac3Accum.begin(), m_ac3Accum.begin() + (frame - start));
            continue;
        }
        int skipBytes = 0;
        const int flen = m_ac3Parser.parse(frame, end, skipBytes);
        if (flen == NOT_ENOUGH_BUFFER)
            return;
        if (flen <= 0)
        {
            m_ac3Accum.erase(m_ac3Accum.begin());
            continue;
        }
        if (m_ac3Parser.isEAC3())
        {
            THROW(ERR_INVALID_CODEC_FORMAT,
                  "merge-ac3-track: E-AC-3 is not supported as the TrueHD core; use a classic AC-3 track or "
                  "transcode with ffmpeg -c:a ac3 (see tsMuxer --help).")
        }
        const int total = flen + skipBytes;
        Ac3QueuedFrame q;
        q.data.assign(frame, frame + total);
        q.samples = m_ac3Parser.frameSamples();
        q.sample_rate = m_ac3Parser.frameSampleRate();
        if (m_ac3SamplesPerSyncFrame == 0 && q.samples > 0)
            m_ac3SamplesPerSyncFrame = q.samples;
        m_ac3FrameQueue.push_back(std::move(q));
        m_ac3Accum.erase(m_ac3Accum.begin(), m_ac3Accum.begin() + total);
    }
}

void TrueHDAC3MergeReader::fillDelayedFromQueue()
{
    if (m_ac3FrameQueue.empty())
        return;
    const Ac3QueuedFrame front = std::move(m_ac3FrameQueue.front());
    m_ac3FrameQueue.pop_front();
    m_pendingEmitSamples = front.samples;
    m_pendingEmitSampleRate = front.sample_rate;
    m_delayedAc3Buffer.clear();
    m_delayedAc3Buffer.append(front.data.data(), front.data.size());
    m_delayedAc3Packet.flags = m_flags + AVPacket::IS_COMPLETE_FRAME | AVPacket::FORCE_NEW_FRAME;
    m_delayedAc3Packet.stream_index = m_streamIndex;
    m_delayedAc3Packet.codecID = getCodecInfo().codecID;
    m_delayedAc3Packet.codec = static_cast<BaseAbstractStreamReader*>(this);
    m_delayedAc3Packet.duration = 0;
    m_delayedAc3Packet.data = m_delayedAc3Buffer.data();
    m_delayedAc3Packet.size = static_cast<int>(front.data.size());
}

int TrueHDAC3MergeReader::readPacket(AVPacket& avPacket)
{
    while (true)
    {
        // Priority 1: Return pending delayed AC3 packet if waiting for it
        if (m_thdDemuxWaitAc3 && !m_delayedAc3Buffer.isEmpty())
        {
            avPacket = m_delayedAc3Packet;
            m_delayedAc3Buffer.clear();
            m_thdDemuxWaitAc3 = false;
            avPacket.dts = avPacket.pts = m_nextAc3Time;
            avPacket.flags |= AVPacket::IS_CORE_PACKET;
            if (m_pendingEmitSampleRate > 0 && m_pendingEmitSamples > 0)
                m_nextAc3Time +=
                    static_cast<int64_t>(INTERNAL_PTS_FREQ) * m_pendingEmitSamples / m_pendingEmitSampleRate;
            return 0;
        }

        // Priority 2: Return queued AC3 frame if waiting and delayed buffer empty
        if (m_thdDemuxWaitAc3 && m_delayedAc3Buffer.isEmpty() && !m_ac3FrameQueue.empty())
        {
            Ac3QueuedFrame q = std::move(m_ac3FrameQueue.front());
            m_ac3FrameQueue.pop_front();
            m_immediateAc3Buffer.clear();
            m_immediateAc3Buffer.append(q.data.data(), q.data.size());
            avPacket.flags = m_flags + AVPacket::IS_COMPLETE_FRAME | AVPacket::FORCE_NEW_FRAME;
            avPacket.stream_index = m_streamIndex;
            avPacket.codecID = getCodecInfo().codecID;
            avPacket.codec = static_cast<BaseAbstractStreamReader*>(this);
            avPacket.data = m_immediateAc3Buffer.data();
            avPacket.size = static_cast<int>(q.data.size());
            avPacket.duration = 0;
            avPacket.dts = avPacket.pts = m_nextAc3Time;
            avPacket.flags |= AVPacket::IS_CORE_PACKET;
            if (q.sample_rate > 0 && q.samples > 0)
                m_nextAc3Time += static_cast<int64_t>(INTERNAL_PTS_FREQ) * q.samples / q.sample_rate;
            m_thdDemuxWaitAc3 = false;
            if (m_ac3SamplesPerSyncFrame == 0)
                m_ac3SamplesPerSyncFrame = q.samples;
            return 0;
        }

        // Priority 3: Need more AC3 data if waiting and none queued
        if (m_thdDemuxWaitAc3 && m_ac3FrameQueue.empty())
        {
            const int need = requestMoreAc3Data();
            if (need >= 0)
                return need;
            // AC3 EOF: continue emitting remaining TrueHD without further core frames
            m_thdDemuxWaitAc3 = false;
        }

        // Priority 4: Keep one AC3 frame pre-buffered so the next sync point can emit it.
        // This also forces periodic AC3 reads so the side track cannot starve.
        if (!m_thdDemuxWaitAc3 && m_delayedAc3Buffer.isEmpty())
        {
            if (m_ac3FrameQueue.empty())
            {
                const int need = requestMoreAc3Data();
                if (need >= 0)
                    return need;
            }
            else
                fillDelayedFromQueue();
        }

        // Read next TrueHD packet
        const int rez = SimplePacketizerReader::readPacket(avPacket);
        if (rez != 0)
            return rez;

        if (m_samplerate <= 0)
            return 0;

        avPacket.dts = avPacket.pts = m_totalTHDSamples * INTERNAL_PTS_FREQ / m_samplerate;

        m_totalTHDSamples += m_samples;
        m_demuxedTHDSamplesForAc3 += m_samples;
        if (m_ac3SamplesPerSyncFrame > 0 && m_demuxedTHDSamplesForAc3 >= m_ac3SamplesPerSyncFrame)
        {
            m_demuxedTHDSamplesForAc3 -= m_ac3SamplesPerSyncFrame;
            // After AC-3 EOF, emit remaining TrueHD without waiting for more core frames.
            if (!m_ac3Eof || !m_ac3FrameQueue.empty() || !m_delayedAc3Buffer.isEmpty())
                m_thdDemuxWaitAc3 = true;
        }
        return 0;
    }
}

int TrueHDAC3MergeReader::flushPacket(AVPacket& avPacket)
{
    const int rez = MLPStreamReader::flushPacket(avPacket);
    if (rez > 0)
    {
        if (!(avPacket.flags & AVPacket::PRIORITY_DATA) && m_samplerate > 0)
            avPacket.pts = avPacket.dts = m_totalTHDSamples * INTERNAL_PTS_FREQ / m_samplerate;
    }
    return rez;
}

bool TrueHDAC3MergeReader::needMPLSCorrection() const { return false; }

void TrueHDAC3MergeReader::writePESExtension(PESPacket* pesPacket, const AVPacket& avPacket)
{
    if (m_useNewStyleAudioPES)
    {
        pesPacket->flagsLo |= 1;
        uint8_t* data = reinterpret_cast<uint8_t*>(pesPacket) + pesPacket->getHeaderLength();
        *data++ = 0x01;
        *data++ = 0x81;
        if (avPacket.flags & AVPacket::IS_CORE_PACKET)
            *data = 0x76;
        else
            *data = 0x72;
        pesPacket->m_pesHeaderLen += 3;
    }
}

const std::string TrueHDAC3MergeReader::getStreamInfo()
{
    std::ostringstream str;
    if (m_mergeFromFile)
        str << "TRUE-HD + AC-3 core (merged from file). ";
    else
        str << "TRUE-HD + AC-3 core (merged from track " << m_mergeAc3Pid << "). ";
    str << MLPStreamReader::getStreamInfo();
    return str.str();
}
