#ifndef MATROSKA_MUXER_H_
#define MATROSKA_MUXER_H_

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "abstractMuxer.h"
#include "avPacket.h"

// ──────────────────────────────── EBML writing helpers ────────────────────────────────

// Return the number of bytes needed for an EBML element ID.
int ebml_id_size(uint32_t id);

// Write an EBML element ID to dst, return number of bytes written.
int ebml_write_id(uint8_t* dst, uint32_t id);

// Return the number of bytes needed to encode `size` as an EBML VINT (data-size).
int ebml_size_size(uint64_t size);

// Write an EBML data-size VINT to dst using the minimum number of bytes.
// Returns the number of bytes written.
int ebml_write_size(uint8_t* dst, uint64_t size);

// Write an EBML data-size VINT using exactly `bytes` bytes.
// Returns the number of bytes written (== bytes).
int ebml_write_size_fixed(uint8_t* dst, uint64_t size, int bytes);

// "Unknown" size encoded as 0xFF (1 byte) or all-ones for the given width.
int ebml_write_unknown_size(uint8_t* dst, int bytes);

// Write helpers for typed EBML elements – each writes <ID><size><payload>.
// Returns total bytes written.
int ebml_write_uint(uint8_t* dst, uint32_t id, uint64_t value);
int ebml_write_sint(uint8_t* dst, uint32_t id, int64_t value);
int ebml_write_float(uint8_t* dst, uint32_t id, double value);
int ebml_write_string(uint8_t* dst, uint32_t id, const std::string& value);
int ebml_write_binary(uint8_t* dst, uint32_t id, const uint8_t* data, int len);
int ebml_write_master_open(uint8_t* dst, uint32_t id, uint64_t contentSize);

// ──────────────────────────────── Matroska Muxer ─────────────────────────────────────

class MatroskaMuxer final : public AbstractMuxer
{
   public:
    MatroskaMuxer(MuxerManager* owner);
    ~MatroskaMuxer() override;

    void intAddStream(const std::string& streamName, const std::string& codecName, int streamIndex,
                      const std::map<std::string, std::string>& params, AbstractStreamReader* codecReader) override;
    bool muxPacket(AVPacket& avPacket) override;
    bool doFlush() override;
    bool close() override;
    void openDstFile() override;
    void parseMuxOpt(const std::string& opts) override;

   private:
    // ── Track information collected during intAddStream ──
    struct MkvTrackInfo
    {
        int streamIndex;              // external stream index
        int trackNumber;              // 1-based Matroska track number
        uint64_t trackUID;            // random UID
        std::string matroskaCodecID;  // e.g. "V_MPEG4/ISO/AVC"
        int codecID;                  // internal CODEC_* constant
        uint8_t trackType;            // 1=video, 2=audio, 17=subtitle
        AbstractStreamReader* codecReader;

        // Video-specific
        unsigned width;
        unsigned height;
        double fps;
        bool interlaced;
        VideoAspectRatio streamAR;

        // Audio-specific
        int sampleRate;
        int channels;
        int bitDepth;

        // Codec-private data (built at openDstFile time)
        std::vector<uint8_t> codecPrivate;

        // Frame accumulation: the MPEG stream reader may split large frames
        // into multiple packets with the same PTS. We buffer them here and
        // write a single SimpleBlock when PTS changes or at flush time.
        std::vector<uint8_t> pendingFrameData;
        int64_t pendingPts;
        uint8_t pendingFlags;
        bool hasPendingFrame;

        MkvTrackInfo()
            : streamIndex(0),
              trackNumber(0),
              trackUID(0),
              codecID(0),
              trackType(0),
              codecReader(nullptr),
              width(0),
              height(0),
              fps(0),
              interlaced(false),
              streamAR(VideoAspectRatio::AR_KEEP_DEFAULT),
              sampleRate(0),
              channels(0),
              bitDepth(0),
              pendingPts(0),
              pendingFlags(0),
              hasPendingFrame(false)
        {
        }
    };

    // ── Cue point for building the Cues element at close ──
    struct CueEntry
    {
        int64_t timecodeMs;  // cluster-relative time in ms
        int trackNumber;
        int64_t clusterOffset;  // byte offset of the cluster from segment data start
    };

    // Helper: map internal codec name to Matroska codec ID string
    static std::string codecNameToMatroskaID(const std::string& codecName, int codecID);

    // Build codec-private data for a track
    void buildCodecPrivate(MkvTrackInfo& track);

    // Build the AVCDecoderConfigurationRecord from H.264 SPS/PPS
    static std::vector<uint8_t> buildAVCDecoderConfigRecord(AbstractStreamReader* reader);
    // Build the HEVCDecoderConfigurationRecord from HEVC VPS/SPS/PPS
    static std::vector<uint8_t> buildHEVCDecoderConfigRecord(AbstractStreamReader* reader);
    // Build the VVCDecoderConfigurationRecord
    static std::vector<uint8_t> buildVVCDecoderConfigRecord(AbstractStreamReader* reader);
    // Build AV1CodecConfigurationRecord
    static std::vector<uint8_t> buildAV1ConfigRecord(AbstractStreamReader* reader);
    // Build AAC AudioSpecificConfig
    static std::vector<uint8_t> buildAACConfig(AbstractStreamReader* reader);

    // Write the EBML header to the output file
    void writeEBMLHeader();
    // Write Segment Info element into m_segmentBody
    void writeSegmentInfo();
    // Write Tracks element into m_segmentBody
    void writeTracks();
    // Write a single TrackEntry and return its serialized bytes
    std::vector<uint8_t> buildTrackEntry(const MkvTrackInfo& track);

    // Start a new cluster at the given timecode (milliseconds)
    void startCluster(int64_t timecodeMs);
    // Flush the current cluster to the file
    void flushCluster();

    // Write Cues element at end of file
    void writeCues();
    // Write SeekHead element at end of file
    void writeSeekHead();

    // Low-level: write bytes to the output file
    void writeToFile(const uint8_t* data, int len);
    void writeToFile(const std::vector<uint8_t>& data);

    // ── Data members ──
    File m_file;
    std::string m_fileName;

    std::map<int, MkvTrackInfo> m_tracks;  // keyed by streamIndex
    int m_nextTrackNumber;

    // Segment layout
    int64_t m_segmentStartPos;  // file position of the first byte after the Segment header
    int64_t m_segmentSizePos;   // file position where the segment's VINT size is written

    // Cluster buffering
    std::vector<uint8_t> m_clusterBuf;  // current cluster data
    int64_t m_clusterTimecodeMs;        // timecode of current cluster in ms
    int64_t m_clusterStartFilePos;      // file offset where current cluster begins
    bool m_clusterOpen;
    int64_t m_clusterDataSize;  // data written in current cluster

    // Cue tracking
    std::vector<CueEntry> m_cueEntries;

    // Positions of key elements (relative to segment data start) for SeekHead
    int64_t m_segmentInfoPos;
    int64_t m_tracksPos;
    int64_t m_cuesPos;

    // Timecode tracking
    int64_t m_firstTimecode;  // first PTS seen (in INTERNAL_PTS_FREQ units) – used as reference
    bool m_firstTimecodeSet;
    int64_t m_lastTimecodeMs;        // last PTS seen (in ms relative to start) for Duration
    int64_t m_durationValueFilePos;  // absolute file position of the Duration float64 value

    // Deferred header writing: SegmentInfo + Tracks are written once ALL tracks
    // have received at least one packet, because stream readers haven't fully
    // initialized (e.g. audio sample rate / channels) at openDstFile time.
    bool m_headerWritten;
    std::set<int> m_seenStreams;  // stream indices that have sent at least one packet

    // Buffered packets accumulated before the header is written.
    struct BufferedPacket
    {
        int stream_index;
        int64_t pts;
        uint32_t flags;
        std::vector<uint8_t> data;
    };
    std::vector<BufferedPacket> m_preHeaderPackets;

    // Refresh track properties from codec readers (called when readers are initialized)
    void refreshTrackProperties();
    // Write the deferred header (SegmentInfo + Tracks)
    void writeDeferredHeader();
    // Replay buffered pre-header packets (sets m_firstTimecode to min PTS)
    void replayBufferedPackets();

    // Flush any pending accumulated frame data for a track as a single SimpleBlock.
    void flushPendingFrame(MkvTrackInfo& track);

    // Internal mux logic (called after header is written)
    bool muxPacketInternal(AVPacket& avPacket);

    // Convert frame data from internal start-code format to MKV format.
    // For AV1: start-code OBUs → low-overhead OBUs (obu_has_size_field + LEB128)
    // For H.264/HEVC/VVC: Annex B start codes → 4-byte length-prefixed NALUs
    // Returns empty vector if no conversion needed (data is passed through as-is).
    static std::vector<uint8_t> convertAV1ToLowOverhead(const uint8_t* data, int size);
    static std::vector<uint8_t> convertAnnexBToLengthPrefixed(const uint8_t* data, int size);

    // Cluster splitting thresholds
    static constexpr int64_t CLUSTER_MAX_DURATION_MS = 5000;      // 5 seconds
    static constexpr int64_t CLUSTER_MAX_SIZE = 5 * 1024 * 1024;  // 5 MB
};

class MatroskaMuxerFactory final : public AbstractMuxerFactory
{
   public:
    [[nodiscard]] std::unique_ptr<AbstractMuxer> newInstance(MuxerManager* owner) const override
    {
        return std::make_unique<MatroskaMuxer>(owner);
    }
};

#endif
