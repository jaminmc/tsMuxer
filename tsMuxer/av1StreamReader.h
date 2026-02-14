#ifndef AV1_STREAM_READER_H_
#define AV1_STREAM_READER_H_

#include "abstractDemuxer.h"
#include "av1.h"
#include "mpegStreamReader.h"

class AV1StreamReader final : public MPEGStreamReader
{
    friend class MatroskaMuxer;

   public:
    AV1StreamReader();
    ~AV1StreamReader() override = default;
    int getTSDescriptor(uint8_t* dstBuff, bool blurayMode, bool hdmvDescriptors) override;
    CheckStreamRez checkStream(uint8_t* buffer, int len);
    [[nodiscard]] bool needSPSForSplit() const override { return false; }

   protected:
    const CodecInfo& getCodecInfo() override;
    int intDecodeNAL(uint8_t* buff) override;

    double getStreamFPS(void* curNalUnit) override;
    [[nodiscard]] unsigned getStreamWidth() const override;
    [[nodiscard]] unsigned getStreamHeight() const override;
    bool getInterlaced() override { return false; }
    bool isIFrame() override { return m_lastIFrame; }

    void updateStreamFps(void* nalUnit, uint8_t* buff, uint8_t* nextNal, int oldSpsLen) override;
    int writeAdditionData(uint8_t* dstBuffer, uint8_t* dstEnd, AVPacket& avPacket,
                          PriorityDataInfo* priorityData) override;
    void onSplitEvent() override { m_firstFileFrame = true; }

   private:
    void incTimings();
    uint8_t* writeNalPrefix(uint8_t* curPos) const;
    uint8_t* writeBuffer(MemoryBlock& srcData, uint8_t* dstBuffer, const uint8_t* dstEnd) const;

    Av1SequenceHeader m_seqHdr;
    bool m_seqHdrFound;
    bool m_firstFrame;
    bool m_lastIFrame;
    bool m_firstFileFrame;
    int m_frameNum;

    MemoryBlock m_seqHdrBuffer;

    // Fields for the AV1 video descriptor (from sequence header or container config)
    uint8_t m_hdrWcgIdc;
};

#endif  // AV1_STREAM_READER_H_
