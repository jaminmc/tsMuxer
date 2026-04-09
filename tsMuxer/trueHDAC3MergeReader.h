#ifndef TRUE_HD_AC3_MERGE_READER_H_
#define TRUE_HD_AC3_MERGE_READER_H_

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "abstractDemuxer.h"
#include "ac3Codec.h"
#include "avPacket.h"
#include "mlpStreamReader.h"

// Interleaves a standalone TrueHD (A_MLP) track with a separate AC-3 track from the
// same MKV for Blu-ray–style TrueHD + core muxing (merge-ac3-track meta option).

class TrueHDAC3MergeReader final : public MLPStreamReader
{
   public:
    explicit TrueHDAC3MergeReader(const std::map<std::string, std::string>& addParams);
    TrueHDAC3MergeReader(const TrueHDAC3MergeReader&) = delete;
    TrueHDAC3MergeReader& operator=(const TrueHDAC3MergeReader&) = delete;

    [[nodiscard]] int mergeAc3TrackPid() const { return m_mergeAc3Pid; }

    void setNewStyleAudioPES(bool value) { m_useNewStyleAudioPES = value; }

    void setAc3SideData(const uint8_t* data, uint32_t len);

    const CodecInfo& getCodecInfo() override;
    int readPacket(AVPacket& avPacket) override;
    int flushPacket(AVPacket& avPacket) override;
    void writePESExtension(PESPacket* pesPacket, const AVPacket& avPacket) override;
    [[nodiscard]] bool needMPLSCorrection() const override;
    const std::string getStreamInfo() override;

   private:
    struct Ac3QueuedFrame
    {
        std::vector<uint8_t> data;
        int samples = 0;
        int sample_rate = 0;
    };

    void extractAc3FramesFromAccum();
    void fillDelayedFromQueue();

    int m_mergeAc3Pid;
    bool m_useNewStyleAudioPES;

    std::vector<uint8_t> m_ac3Accum;
    std::deque<Ac3QueuedFrame> m_ac3FrameQueue;

    bool m_thdDemuxWaitAc3;
    MemoryBlock m_delayedAc3Buffer;
    MemoryBlock m_immediateAc3Buffer;
    AVPacket m_delayedAc3Packet;
    int m_demuxedTHDSamplesForAc3;
    int64_t m_nextAc3Time;
    int m_ac3SamplesPerSyncFrame;
    int m_pendingEmitSamples;
    int m_pendingEmitSampleRate;

    struct Ac3FrameParser : AC3Codec
    {
        int parse(uint8_t* b, uint8_t* e, int& sk) { return decodeFrame(b, e, sk); }
        uint8_t* findAc3Sync(uint8_t* b, const uint8_t* e) { return findFrame(b, e); }
        [[nodiscard]] int frameSamples() const { return m_samples; }
        [[nodiscard]] int frameSampleRate() const { return m_sample_rate; }
    };
    Ac3FrameParser m_ac3Parser;
};

#endif
