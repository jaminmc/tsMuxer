#include "matroskaMuxer.h"

#include <fs/systemlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

#include "aac.h"
#include "aacStreamReader.h"
#include "ac3StreamReader.h"
#include "av1.h"
#include "av1StreamReader.h"
#include "avCodecs.h"
#include "dtsStreamReader.h"
#include "h264StreamReader.h"
#include "hevc.h"
#include "hevcStreamReader.h"
#include "lpcmStreamReader.h"
#include "matroskaParser.h"
#include "muxerManager.h"
#include "nalUnits.h"
#include "simplePacketizerReader.h"
#include "vodCoreException.h"
#include "vvc.h"
#include "vvcStreamReader.h"

using namespace std;

// Divisor to convert from internal PTS frequency to milliseconds.
// Internal timestamps use INTERNAL_PTS_FREQ (196 * 27MHz = 5,292,000,000 / sec).
static constexpr int64_t INTERNAL_PTS_PER_MS = INTERNAL_PTS_FREQ / 1000;  // 5292000

// ═══════════════════════════════ EBML Writing Utilities ═══════════════════════════════

int ebml_id_size(uint32_t id)
{
    if (id < 0x80)
        return 0;  // invalid – IDs always have the VINT marker bit
    if (id <= 0xFF)
        return 1;
    if (id <= 0xFFFF)
        return 2;
    if (id <= 0xFFFFFF)
        return 3;
    return 4;
}

int ebml_write_id(uint8_t* dst, const uint32_t id)
{
    const int len = ebml_id_size(id);
    for (int i = len - 1; i >= 0; --i) dst[len - 1 - i] = static_cast<uint8_t>((id >> (i * 8)) & 0xFF);
    return len;
}

int ebml_size_size(uint64_t size)
{
    if (size < 0x7F)
        return 1;
    if (size < 0x3FFF)
        return 2;
    if (size < 0x1FFFFF)
        return 3;
    if (size < 0x0FFFFFFF)
        return 4;
    if (size < 0x07FFFFFFFF)
        return 5;
    if (size < 0x03FFFFFFFFFF)
        return 6;
    if (size < 0x01FFFFFFFFFFFF)
        return 7;
    return 8;
}

int ebml_write_size(uint8_t* dst, uint64_t size) { return ebml_write_size_fixed(dst, size, ebml_size_size(size)); }

int ebml_write_size_fixed(uint8_t* dst, uint64_t size, const int bytes)
{
    // The leading byte has the VINT_MARKER at position (8 - bytes) from MSB
    for (int i = bytes - 1; i >= 0; --i) dst[bytes - 1 - i] = static_cast<uint8_t>((size >> (i * 8)) & 0xFF);
    dst[0] |= static_cast<uint8_t>(1 << (8 - bytes));  // set VINT_MARKER
    return bytes;
}

int ebml_write_unknown_size(uint8_t* dst, const int bytes)
{
    // For an n-byte VINT "unknown size", the first byte has the VINT_MARKER
    // in bit (8-n) and all data bits set to 1.  Remaining bytes are all 0xFF.
    // e.g. 1-byte: 0xFF, 2-byte: 0x7F FF, 8-byte: 0x01 FF FF FF FF FF FF FF
    dst[0] = static_cast<uint8_t>(0xFF >> (bytes - 1));
    for (int i = 1; i < bytes; i++) dst[i] = 0xFF;
    return bytes;
}

// Return the minimum number of bytes needed to store a uint value
static int uint_size(uint64_t value)
{
    if (value == 0)
        return 1;
    int bytes = 0;
    while (value > 0)
    {
        value >>= 8;
        bytes++;
    }
    return bytes;
}

static int sint_size(int64_t value)
{
    if (value >= -128 && value <= 127)
        return 1;
    if (value >= -32768 && value <= 32767)
        return 2;
    if (value >= -8388608 && value <= 8388607)
        return 3;
    if (value >= -2147483648LL && value <= 2147483647LL)
        return 4;
    return 8;
}

int ebml_write_uint(uint8_t* dst, const uint32_t id, uint64_t value)
{
    int pos = ebml_write_id(dst, id);
    const int valSize = uint_size(value);
    pos += ebml_write_size(dst + pos, valSize);
    for (int i = valSize - 1; i >= 0; --i) dst[pos++] = static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
    return pos;
}

int ebml_write_sint(uint8_t* dst, const uint32_t id, int64_t value)
{
    int pos = ebml_write_id(dst, id);
    const int valSize = sint_size(value);
    pos += ebml_write_size(dst + pos, valSize);
    for (int i = valSize - 1; i >= 0; --i) dst[pos++] = static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
    return pos;
}

int ebml_write_float(uint8_t* dst, const uint32_t id, double value)
{
    int pos = ebml_write_id(dst, id);
    pos += ebml_write_size(dst + pos, 8);
    // Write 64-bit IEEE 754 big-endian
    uint64_t bits;
    memcpy(&bits, &value, 8);
    for (int i = 7; i >= 0; --i) dst[pos++] = static_cast<uint8_t>((bits >> (i * 8)) & 0xFF);
    return pos;
}

int ebml_write_string(uint8_t* dst, const uint32_t id, const std::string& value)
{
    int pos = ebml_write_id(dst, id);
    pos += ebml_write_size(dst + pos, value.size());
    memcpy(dst + pos, value.data(), value.size());
    return pos + static_cast<int>(value.size());
}

int ebml_write_binary(uint8_t* dst, const uint32_t id, const uint8_t* data, int len)
{
    int pos = ebml_write_id(dst, id);
    pos += ebml_write_size(dst + pos, len);
    memcpy(dst + pos, data, len);
    return pos + len;
}

int ebml_write_master_open(uint8_t* dst, const uint32_t id, uint64_t contentSize)
{
    int pos = ebml_write_id(dst, id);
    pos += ebml_write_size(dst + pos, contentSize);
    return pos;
}

// ═══════════════════════════════ Matroska Muxer ══════════════════════════════════════

MatroskaMuxer::MatroskaMuxer(MuxerManager* owner)
    : AbstractMuxer(owner),
      m_nextTrackNumber(1),
      m_segmentStartPos(0),
      m_segmentSizePos(0),
      m_clusterTimecodeMs(0),
      m_clusterStartFilePos(0),
      m_clusterOpen(false),
      m_clusterDataSize(0),
      m_segmentInfoPos(0),
      m_tracksPos(0),
      m_cuesPos(0),
      m_firstTimecode(0),
      m_firstTimecodeSet(false),
      m_lastTimecodeMs(0),
      m_durationValueFilePos(0),
      m_headerWritten(false)
{
}

MatroskaMuxer::~MatroskaMuxer() = default;

// ──────────────── Codec name mapping ──────────────────────────────────────────

std::string MatroskaMuxer::codecNameToMatroskaID(const std::string& codecName, int codecID)
{
    // Video
    if (codecName == "V_MPEG4/ISO/AVC")
        return MATROSKA_CODEC_ID_AVC_FOURCC;
    if (codecName == "V_MPEGH/ISO/HEVC")
        return MATROSKA_CODEC_ID_HEVC_FOURCC;
    if (codecName == "V_MPEGI/ISO/VVC")
        return MATROSKA_CODEC_ID_VVC_FOURCC;
    if (codecName == "V_AV1")
        return MATROSKA_CODEC_ID_AV1;
    if (codecName == "V_MS/VFW/WVC1")
        return MATROSKA_CODEC_ID_VIDEO_VFW_FOURCC;
    if (codecName == "V_MPEG-2")
        return MATROSKA_CODEC_ID_VIDEO_MPEG2;

    // Audio
    if (codecName == "A_AC3")
    {
        if (codecID == CODEC_A_EAC3)
            return MATROSKA_CODEC_ID_AUDIO_EAC3;
        if (codecID == CODEC_A_HDAC3)
            return MATROSKA_CODEC_ID_AUDIO_TRUEHD;
        return MATROSKA_CODEC_ID_AUDIO_AC3;
    }
    if (codecName == "A_AAC")
        return MATROSKA_CODEC_ID_AUDIO_AAC;
    if (codecName == "A_DTS")
        return MATROSKA_CODEC_ID_AUDIO_DTS;
    if (codecName == "A_LPCM")
        return MATROSKA_CODEC_ID_AUDIO_PCM_LIT;
    if (codecName == "A_MLP")
        return MATROSKA_CODEC_ID_AUDIO_TRUEHD;
    if (codecName == "A_MP3")
        return MATROSKA_CODEC_ID_AUDIO_MPEG_L3;

    // Subtitles
    if (codecName == "S_TEXT/UTF8")
        return MATROSKA_CODEC_ID_SRT;
    if (codecName == "S_HDMV/PGS")
        return MATROSKA_CODEC_ID_SUBTITLE_PGS;
    if (codecName == "S_SUP")
        return MATROSKA_CODEC_ID_SUBTITLE_PGS;

    return codecName;
}

// ──────────────── intAddStream ───────────────────────────────────────────────

void MatroskaMuxer::intAddStream(const std::string& /*streamName*/, const std::string& codecName, int streamIndex,
                                 const std::map<std::string, std::string>& /*params*/,
                                 AbstractStreamReader* codecReader)
{
    MkvTrackInfo track;
    track.streamIndex = streamIndex;
    track.trackNumber = m_nextTrackNumber++;
    track.codecReader = codecReader;
    track.codecID = codecReader->getCodecInfo().codecID;
    track.matroskaCodecID = codecNameToMatroskaID(codecName, track.codecID);

    // Generate a random UID
    static std::mt19937_64 rng(std::random_device{}());
    track.trackUID = rng();

    // Determine track type & properties
    if (codecName[0] == 'V')
    {
        track.trackType = 1;  // video
        const auto mpegReader = dynamic_cast<MPEGStreamReader*>(codecReader);
        if (mpegReader)
        {
            track.width = mpegReader->getStreamWidth();
            track.height = mpegReader->getStreamHeight();
            track.fps = mpegReader->getFPS();
            track.interlaced = mpegReader->getInterlaced();
        }
    }
    else if (codecName[0] == 'A')
    {
        track.trackType = 2;  // audio
        const auto simpleReader = dynamic_cast<SimplePacketizerReader*>(codecReader);
        if (simpleReader)
        {
            track.sampleRate = simpleReader->getFreq();
            track.channels = simpleReader->getChannels();
        }
        // Bit depth for LPCM
        const auto lpcmReader = dynamic_cast<LPCMStreamReader*>(codecReader);
        if (lpcmReader)
            track.bitDepth = lpcmReader->m_bitsPerSample;
    }
    else if (codecName[0] == 'S')
    {
        track.trackType = 17;  // subtitle
    }

    m_tracks[streamIndex] = track;
}

// ──────────────── Codec Private builders ─────────────────────────────────────

std::vector<uint8_t> MatroskaMuxer::buildAVCDecoderConfigRecord(AbstractStreamReader* reader)
{
    const auto h264 = dynamic_cast<H264StreamReader*>(reader);
    if (!h264)
        return {};

    // Collect SPS and PPS NAL units
    std::vector<std::pair<std::vector<uint8_t>, int>> spsUnits, ppsUnits;

    for (auto& [id, sps] : h264->m_spsMap)
    {
        uint8_t buf[4096];
        const int len = sps->serializeBuffer(buf, buf + sizeof(buf), false);
        if (len > 0)
            spsUnits.push_back({std::vector<uint8_t>(buf, buf + len), id});
    }
    for (auto& [id, pps] : h264->m_ppsMap)
    {
        uint8_t buf[4096];
        const int len = pps->serializeBuffer(buf, buf + sizeof(buf), false);
        if (len > 0)
            ppsUnits.push_back({std::vector<uint8_t>(buf, buf + len), id});
    }

    if (spsUnits.empty())
        return {};

    // Parse first SPS to extract profile/level
    const auto& firstSps = spsUnits[0].first;
    uint8_t profileIdc = firstSps.size() > 1 ? firstSps[1] : 66;
    uint8_t profileCompat = firstSps.size() > 2 ? firstSps[2] : 0;
    uint8_t levelIdc = firstSps.size() > 3 ? firstSps[3] : 30;

    std::vector<uint8_t> record;
    record.push_back(1);  // configurationVersion
    record.push_back(profileIdc);
    record.push_back(profileCompat);
    record.push_back(levelIdc);
    record.push_back(0xFF);  // lengthSizeMinusOne = 3 (4-byte NAL length) | reserved 0xFC
    record.push_back(static_cast<uint8_t>(0xE0 | (spsUnits.size() & 0x1F)));  // numSPS | reserved 0xE0

    for (auto& [data, id] : spsUnits)
    {
        const uint16_t sz = static_cast<uint16_t>(data.size());
        record.push_back(static_cast<uint8_t>(sz >> 8));
        record.push_back(static_cast<uint8_t>(sz & 0xFF));
        record.insert(record.end(), data.begin(), data.end());
    }

    record.push_back(static_cast<uint8_t>(ppsUnits.size()));
    for (auto& [data, id] : ppsUnits)
    {
        const uint16_t sz = static_cast<uint16_t>(data.size());
        record.push_back(static_cast<uint8_t>(sz >> 8));
        record.push_back(static_cast<uint8_t>(sz & 0xFF));
        record.insert(record.end(), data.begin(), data.end());
    }

    return record;
}

std::vector<uint8_t> MatroskaMuxer::buildHEVCDecoderConfigRecord(AbstractStreamReader* reader)
{
    const auto hevc = dynamic_cast<HEVCStreamReader*>(reader);
    if (!hevc || !hevc->m_sps || !hevc->m_vps)
        return {};

    // Serialize each parameter set
    uint8_t buf[8192];
    std::vector<uint8_t> vpsData, spsData, ppsData;

    int len = hevc->m_vps->serializeBuffer(buf, buf + sizeof(buf));
    if (len > 0)
        vpsData.assign(buf, buf + len);

    len = hevc->m_sps->serializeBuffer(buf, buf + sizeof(buf));
    if (len > 0)
        spsData.assign(buf, buf + len);

    if (hevc->m_pps)
    {
        len = hevc->m_pps->serializeBuffer(buf, buf + sizeof(buf));
        if (len > 0)
            ppsData.assign(buf, buf + len);
    }

    if (spsData.empty())
        return {};

    // Build HEVCDecoderConfigurationRecord
    const HevcSpsUnit* sps = hevc->m_sps;
    std::vector<uint8_t> record;
    record.push_back(1);  // configurationVersion

    // general_profile_space(2) | general_tier_flag(1) | general_profile_idc(5)
    record.push_back(static_cast<uint8_t>(sps->profile_idc & 0x1F));
    // general_profile_compatibility_flags (4 bytes)
    for (int i = 0; i < 4; i++) record.push_back(0);
    // general_constraint_indicator_flags (6 bytes)
    for (int i = 0; i < 6; i++) record.push_back(0);
    // general_level_idc
    record.push_back(static_cast<uint8_t>(sps->level_idc));
    // min_spatial_segmentation_idc
    record.push_back(0xF0);
    record.push_back(0x00);
    // parallelismType
    record.push_back(0xFC);
    // chromaFormatIdc
    record.push_back(static_cast<uint8_t>(0xFC | (sps->chromaFormat & 0x03)));
    // bitDepthLumaMinus8
    record.push_back(static_cast<uint8_t>(0xF8 | (sps->bit_depth_luma_minus8 & 0x07)));
    // bitDepthChromaMinus8
    record.push_back(static_cast<uint8_t>(0xF8 | (sps->bit_depth_chroma_minus8 & 0x07)));
    // avgFrameRate
    record.push_back(0);
    record.push_back(0);
    // constantFrameRate(2) | numTemporalLayers(3) | temporalIdNested(1) | lengthSizeMinusOne(2)
    record.push_back(0x0F);  // lengthSizeMinusOne=3

    int numArrays = 0;
    if (!vpsData.empty())
        numArrays++;
    if (!spsData.empty())
        numArrays++;
    if (!ppsData.empty())
        numArrays++;
    record.push_back(static_cast<uint8_t>(numArrays));

    // VPS array
    if (!vpsData.empty())
    {
        record.push_back(0x20);  // array_completeness=0 | NAL_unit_type=32 (VPS)
        record.push_back(0);
        record.push_back(1);  // numNalus
        record.push_back(static_cast<uint8_t>(vpsData.size() >> 8));
        record.push_back(static_cast<uint8_t>(vpsData.size() & 0xFF));
        record.insert(record.end(), vpsData.begin(), vpsData.end());
    }

    // SPS array
    if (!spsData.empty())
    {
        record.push_back(0x21);  // NAL_unit_type=33 (SPS)
        record.push_back(0);
        record.push_back(1);
        record.push_back(static_cast<uint8_t>(spsData.size() >> 8));
        record.push_back(static_cast<uint8_t>(spsData.size() & 0xFF));
        record.insert(record.end(), spsData.begin(), spsData.end());
    }

    // PPS array
    if (!ppsData.empty())
    {
        record.push_back(0x22);  // NAL_unit_type=34 (PPS)
        record.push_back(0);
        record.push_back(1);
        record.push_back(static_cast<uint8_t>(ppsData.size() >> 8));
        record.push_back(static_cast<uint8_t>(ppsData.size() & 0xFF));
        record.insert(record.end(), ppsData.begin(), ppsData.end());
    }

    return record;
}

std::vector<uint8_t> MatroskaMuxer::buildVVCDecoderConfigRecord(AbstractStreamReader* reader)
{
    const auto vvc = dynamic_cast<VVCStreamReader*>(reader);
    if (!vvc)
        return {};

    // Use raw buffers from VVC stream reader
    // For VVC, the CodecPrivate is typically the raw VPS+SPS+PPS with 4-byte length prefixes
    // But the standard VVCDecoderConfigurationRecord is complex; for now use the raw parameter sets
    // as CodecPrivate data in Annex B format (start codes)
    std::vector<uint8_t> record;

    // Simply concatenate the parameter set NALUs with 4-byte lengths
    // This is what most MKV muxers do for VVC
    if (vvc->m_vpsBuffer.size() > 0)
    {
        const uint32_t sz = static_cast<uint32_t>(vvc->m_vpsBuffer.size());
        record.push_back(static_cast<uint8_t>(sz >> 24));
        record.push_back(static_cast<uint8_t>(sz >> 16));
        record.push_back(static_cast<uint8_t>(sz >> 8));
        record.push_back(static_cast<uint8_t>(sz));
        record.insert(record.end(), vvc->m_vpsBuffer.data(), vvc->m_vpsBuffer.data() + sz);
    }
    if (vvc->m_spsBuffer.size() > 0)
    {
        const uint32_t sz = static_cast<uint32_t>(vvc->m_spsBuffer.size());
        record.push_back(static_cast<uint8_t>(sz >> 24));
        record.push_back(static_cast<uint8_t>(sz >> 16));
        record.push_back(static_cast<uint8_t>(sz >> 8));
        record.push_back(static_cast<uint8_t>(sz));
        record.insert(record.end(), vvc->m_spsBuffer.data(), vvc->m_spsBuffer.data() + sz);
    }
    if (vvc->m_ppsBuffer.size() > 0)
    {
        const uint32_t sz = static_cast<uint32_t>(vvc->m_ppsBuffer.size());
        record.push_back(static_cast<uint8_t>(sz >> 24));
        record.push_back(static_cast<uint8_t>(sz >> 16));
        record.push_back(static_cast<uint8_t>(sz >> 8));
        record.push_back(static_cast<uint8_t>(sz));
        record.insert(record.end(), vvc->m_ppsBuffer.data(), vvc->m_ppsBuffer.data() + sz);
    }

    return record;
}

std::vector<uint8_t> MatroskaMuxer::buildAV1ConfigRecord(AbstractStreamReader* reader)
{
    const auto av1 = dynamic_cast<AV1StreamReader*>(reader);
    if (!av1 || !av1->m_seqHdrFound)
        return {};

    // AV1CodecConfigurationRecord (4 bytes) + sequence header OBU in low-overhead format
    const auto& hdr = av1->m_seqHdr;
    std::vector<uint8_t> record;

    // marker(1) | version(7) = 0x81
    record.push_back(0x81);
    // seq_profile(3) | seq_level_idx_0(5)
    record.push_back(static_cast<uint8_t>(((hdr.seq_profile & 0x07) << 5) | (hdr.seq_level_idx_0 & 0x1F)));
    // seq_tier_0(1) | high_bitdepth(1) | twelve_bit(1) | monochrome(1) |
    // chroma_subsampling_x(1) | chroma_subsampling_y(1) | chroma_sample_position(2)
    const uint8_t bitDepth = hdr.getBitDepth();
    const uint8_t highBitdepth = (bitDepth > 8) ? 1 : 0;
    const uint8_t twelveBit = (bitDepth == 12) ? 1 : 0;
    record.push_back(static_cast<uint8_t>((0 << 7) |             // seq_tier_0
                                          (highBitdepth << 6) |  // high_bitdepth
                                          (twelveBit << 5) |     // twelve_bit
                                          (hdr.mono_chrome << 4) | (hdr.chroma_subsampling_x << 3) |
                                          (hdr.chroma_subsampling_y << 2) | (hdr.chroma_sample_position & 0x03)));
    // initial_presentation_delay_present(1) | reserved/initial_presentation_delay_minus_one(3) | padding(4)
    record.push_back(0x00);

    // Note: We only write the 4-byte AV1CodecConfigurationRecord here.
    // The sequence header OBU is included in the first frame's data (as part of
    // the low-overhead OBU stream), so the decoder will pick it up from there.
    // This avoids issues with emulation prevention byte round-tripping.

    return record;
}

std::vector<uint8_t> MatroskaMuxer::buildAACConfig(AbstractStreamReader* reader)
{
    const auto aac = dynamic_cast<AACStreamReader*>(reader);
    if (!aac)
        return {};

    // Build a 2-byte AudioSpecificConfig
    // audioObjectType(5 bits) | samplingFrequencyIndex(4 bits) | channelConfiguration(4 bits) | padding(3 bits)
    const uint8_t objectType = aac->m_profile + 1;  // AAC profile is 0-based, objectType is 1-based
    const uint8_t freqIndex = aac->m_sample_rates_index;
    const uint8_t chanConfig = aac->m_channels_index;

    std::vector<uint8_t> config(2);
    config[0] = static_cast<uint8_t>((objectType << 3) | (freqIndex >> 1));
    config[1] = static_cast<uint8_t>(((freqIndex & 1) << 7) | (chanConfig << 3));

    return config;
}

void MatroskaMuxer::buildCodecPrivate(MkvTrackInfo& track)
{
    switch (track.codecID)
    {
    case CODEC_V_MPEG4_H264:
        track.codecPrivate = buildAVCDecoderConfigRecord(track.codecReader);
        break;
    case CODEC_V_MPEG4_H265:
        track.codecPrivate = buildHEVCDecoderConfigRecord(track.codecReader);
        break;
    case CODEC_V_MPEG4_H266:
        track.codecPrivate = buildVVCDecoderConfigRecord(track.codecReader);
        break;
    case CODEC_V_AV1:
        track.codecPrivate = buildAV1ConfigRecord(track.codecReader);
        break;
    case CODEC_A_AAC:
        track.codecPrivate = buildAACConfig(track.codecReader);
        break;
    default:
        // AC3, DTS, LPCM, SRT, PGS, MPEG-2, VC-1 etc. – no codec private needed in MKV
        // (or it's handled differently)
        break;
    }
}

// ──────────────── parseMuxOpt ────────────────────────────────────────────────

void MatroskaMuxer::parseMuxOpt(const std::string& opts)
{
    // Currently no MKV-specific options to parse
    (void)opts;
}

// ──────────────── File I/O helpers ───────────────────────────────────────────

void MatroskaMuxer::writeToFile(const uint8_t* data, int len)
{
    if (len > 0)
        m_file.write(data, len);
}

void MatroskaMuxer::writeToFile(const std::vector<uint8_t>& data)
{
    if (!data.empty())
        m_file.write(data.data(), static_cast<int>(data.size()));
}

// ──────────────── EBML Header ────────────────────────────────────────────────

void MatroskaMuxer::writeEBMLHeader()
{
    // Build the EBML header content
    uint8_t buf[256];
    int pos = 0;
    pos += ebml_write_uint(buf + pos, EBML_ID_EBMLVERSION, 1);
    pos += ebml_write_uint(buf + pos, EBML_ID_EBMLREADVERSION, 1);
    pos += ebml_write_uint(buf + pos, EBML_ID_EBMLMAXIDLENGTH, 4);
    pos += ebml_write_uint(buf + pos, EBML_ID_EBMLMAXSIZELENGTH, 8);
    pos += ebml_write_string(buf + pos, EBML_ID_DOCTYPE, "matroska");
    pos += ebml_write_uint(buf + pos, EBML_ID_DOCTYPEVERSION, 4);
    pos += ebml_write_uint(buf + pos, EBML_ID_DOCTYPEREADVERSION, 2);

    // Write the EBML master element
    uint8_t header[16];
    int hdrLen = ebml_write_id(header, EBML_ID_HEADER);
    hdrLen += ebml_write_size(header + hdrLen, pos);
    writeToFile(header, hdrLen);
    writeToFile(buf, pos);
}

// ──────────────── SegmentInfo ────────────────────────────────────────────────

void MatroskaMuxer::writeSegmentInfo()
{
    m_segmentInfoPos = m_file.pos() - m_segmentStartPos;

    uint8_t buf[512];
    int pos = 0;
    pos += ebml_write_uint(buf + pos, MATROSKA_ID_TIMECODESCALE, 1000000);  // 1 ms

    // Duration placeholder (patched in close() with actual value)
    const int durationElementStart = pos;
    pos += ebml_write_float(buf + pos, MATROSKA_ID_DURATION, 0.0);

    pos += ebml_write_string(buf + pos, MATROSKA_ID_MUXINGAPP, "tsMuxeR");
    pos += ebml_write_string(buf + pos, MATROSKA_ID_WRITINGAPP, "tsMuxeR");

    uint8_t header[16];
    int hdrLen = ebml_write_id(header, MATROSKA_ID_INFO);
    hdrLen += ebml_write_size(header + hdrLen, pos);
    writeToFile(header, hdrLen);

    // MATROSKA_ID_DURATION (0x4489) = 2-byte ID + 1-byte size → float64 at offset +3
    m_durationValueFilePos = m_file.pos() + durationElementStart + 3;

    writeToFile(buf, pos);
}

// ──────────────── Tracks ─────────────────────────────────────────────────────

std::vector<uint8_t> MatroskaMuxer::buildTrackEntry(const MkvTrackInfo& track)
{
    // Build inner content of the TrackEntry.
    // Size the buffer dynamically: fixed fields (~256 bytes) + codec private data.
    const size_t bufSize = 512 + track.codecPrivate.size();
    std::vector<uint8_t> inner(bufSize);
    int pos = 0;

    pos += ebml_write_uint(inner.data() + pos, MATROSKA_ID_TRACKNUMBER, track.trackNumber);
    pos += ebml_write_uint(inner.data() + pos, MATROSKA_ID_TRACKUID, track.trackUID);
    pos += ebml_write_uint(inner.data() + pos, MATROSKA_ID_TRACKTYPE, track.trackType);
    pos += ebml_write_uint(inner.data() + pos, MATROSKA_ID_TRACKFLAGLACING, 0);
    pos += ebml_write_string(inner.data() + pos, MATROSKA_ID_CODECID, track.matroskaCodecID);

    if (!track.codecPrivate.empty())
    {
        pos += ebml_write_binary(inner.data() + pos, MATROSKA_ID_CODECPRIVATE, track.codecPrivate.data(),
                                 static_cast<int>(track.codecPrivate.size()));
    }

    if (track.fps > 0)
    {
        const uint64_t durationNs = static_cast<uint64_t>(1000000000.0 / track.fps);
        pos += ebml_write_uint(inner.data() + pos, MATROSKA_ID_TRACKDEFAULTDURATION, durationNs);
    }

    // Video sub-element
    if (track.trackType == 1 && track.width > 0 && track.height > 0)
    {
        uint8_t videoBuf[128];
        int vPos = 0;
        vPos += ebml_write_uint(videoBuf + vPos, MATROSKA_ID_VIDEOPIXELWIDTH, track.width);
        vPos += ebml_write_uint(videoBuf + vPos, MATROSKA_ID_VIDEOPIXELHEIGHT, track.height);
        if (track.interlaced)
            vPos += ebml_write_uint(videoBuf + vPos, MATROSKA_ID_VIDEOFLAGINTERLACED, 1);

        // Write DisplayWidth / DisplayHeight for non-square-pixel content
        if (track.streamAR != VideoAspectRatio::AR_KEEP_DEFAULT && track.streamAR != VideoAspectRatio::AR_VGA)
        {
            unsigned displayWidth = track.width;
            unsigned displayHeight = track.height;
            switch (track.streamAR)
            {
            case VideoAspectRatio::AR_3_4:
                displayWidth = (track.height * 4 + 1) / 3;
                break;
            case VideoAspectRatio::AR_16_9:
                displayWidth = (track.height * 16 + 4) / 9;
                break;
            case VideoAspectRatio::AR_221_100:
                displayWidth = (track.height * 221 + 50) / 100;
                break;
            default:
                break;
            }
            if (displayWidth != track.width || displayHeight != track.height)
            {
                vPos += ebml_write_uint(videoBuf + vPos, MATROSKA_ID_VIDEODISPLAYWIDTH, displayWidth);
                vPos += ebml_write_uint(videoBuf + vPos, MATROSKA_ID_VIDEODISPLAYHEIGHT, displayHeight);
            }
        }

        // Write Video master
        pos += ebml_write_master_open(inner.data() + pos, MATROSKA_ID_TRACKVIDEO, vPos);
        memcpy(inner.data() + pos, videoBuf, vPos);
        pos += vPos;
    }

    // Audio sub-element
    if (track.trackType == 2 && track.sampleRate > 0)
    {
        uint8_t audioBuf[128];
        int aPos = 0;
        aPos += ebml_write_float(audioBuf + aPos, MATROSKA_ID_AUDIOSAMPLINGFREQ, static_cast<double>(track.sampleRate));
        aPos += ebml_write_uint(audioBuf + aPos, MATROSKA_ID_AUDIOCHANNELS, track.channels);
        if (track.bitDepth > 0)
            aPos += ebml_write_uint(audioBuf + aPos, MATROSKA_ID_AUDIOBITDEPTH, track.bitDepth);

        // Write Audio master
        pos += ebml_write_master_open(inner.data() + pos, MATROSKA_ID_TRACKAUDIO, aPos);
        memcpy(inner.data() + pos, audioBuf, aPos);
        pos += aPos;
    }

    inner.resize(pos);
    return inner;
}

void MatroskaMuxer::writeTracks()
{
    m_tracksPos = m_file.pos() - m_segmentStartPos;

    // Build all track entries
    std::vector<uint8_t> allEntries;
    for (auto& [streamIdx, track] : m_tracks)
    {
        std::vector<uint8_t> entryContent = buildTrackEntry(track);

        // Write TrackEntry master header + content
        uint8_t header[16];
        int hdrLen = ebml_write_id(header, MATROSKA_ID_TRACKENTRY);
        hdrLen += ebml_write_size(header + hdrLen, entryContent.size());
        allEntries.insert(allEntries.end(), header, header + hdrLen);
        allEntries.insert(allEntries.end(), entryContent.begin(), entryContent.end());
    }

    // Write Tracks master element
    uint8_t header[16];
    int hdrLen = ebml_write_id(header, MATROSKA_ID_TRACKS);
    hdrLen += ebml_write_size(header + hdrLen, allEntries.size());
    writeToFile(header, hdrLen);
    writeToFile(allEntries);
}

// ──────────────── openDstFile ────────────────────────────────────────────────

void MatroskaMuxer::openDstFile()
{
    m_fileName = m_origFileName;

    if (!m_file.open(m_fileName.c_str(), File::ofWrite))
        THROW(ERR_CANT_CREATE_FILE, "Can't create output file " << m_fileName)

    // 1. Write EBML Header
    writeEBMLHeader();

    // 2. Write Segment header with unknown size (patched at close time)
    uint8_t segBuf[16];
    int pos = ebml_write_id(segBuf, MATROSKA_ID_SEGMENT);
    writeToFile(segBuf, pos);
    m_segmentSizePos = m_file.pos();
    pos = ebml_write_unknown_size(segBuf, 8);
    writeToFile(segBuf, pos);
    m_segmentStartPos = m_file.pos();

    // SegmentInfo and Tracks are deferred to the first muxPacket call,
    // because stream readers haven't parsed their headers yet at this point.
    m_headerWritten = false;
}

void MatroskaMuxer::refreshTrackProperties()
{
    for (auto& [streamIdx, track] : m_tracks)
    {
        if (track.trackType == 1)  // video
        {
            const auto mpegReader = dynamic_cast<MPEGStreamReader*>(track.codecReader);
            if (mpegReader)
            {
                track.width = mpegReader->getStreamWidth();
                track.height = mpegReader->getStreamHeight();
                track.fps = mpegReader->getFPS();
                track.interlaced = mpegReader->getInterlaced();
                track.streamAR = mpegReader->getStreamAR();
            }
        }
        else if (track.trackType == 2)  // audio
        {
            const auto simpleReader = dynamic_cast<SimplePacketizerReader*>(track.codecReader);
            if (simpleReader)
            {
                track.sampleRate = simpleReader->getFreq();
                track.channels = simpleReader->getChannels();
            }
            const auto lpcmReader = dynamic_cast<LPCMStreamReader*>(track.codecReader);
            if (lpcmReader)
                track.bitDepth = lpcmReader->m_bitsPerSample;
        }
    }
}

void MatroskaMuxer::writeDeferredHeader()
{
    // Re-read track properties now that stream readers have parsed their headers
    refreshTrackProperties();

    // Build codec private data for all tracks
    for (auto& [streamIdx, track] : m_tracks) buildCodecPrivate(track);

    // Write SegmentInfo
    writeSegmentInfo();

    // Write Tracks
    writeTracks();

    m_headerWritten = true;
}

void MatroskaMuxer::replayBufferedPackets()
{
    if (m_preHeaderPackets.empty())
        return;

    // Determine the minimum PTS across all buffered packets so that no track
    // produces negative relative timestamps.
    int64_t minPts = m_preHeaderPackets[0].pts;
    for (const auto& pkt : m_preHeaderPackets) minPts = std::min(minPts, pkt.pts);

    m_firstTimecode = minPts;
    m_firstTimecodeSet = true;

    // Replay all buffered packets through the normal mux path
    for (auto& pkt : m_preHeaderPackets)
    {
        AVPacket tmpPacket;
        tmpPacket.stream_index = pkt.stream_index;
        tmpPacket.pts = pkt.pts;
        tmpPacket.dts = pkt.pts;
        tmpPacket.flags = pkt.flags;
        tmpPacket.data = pkt.data.data();
        tmpPacket.size = static_cast<int>(pkt.data.size());
        muxPacketInternal(tmpPacket);
    }
    m_preHeaderPackets.clear();
    m_preHeaderPackets.shrink_to_fit();
}

// ──────────────── Cluster writing ────────────────────────────────────────────

void MatroskaMuxer::startCluster(int64_t timecodeMs)
{
    if (m_clusterOpen)
        flushCluster();

    m_clusterTimecodeMs = timecodeMs;
    m_clusterBuf.clear();
    m_clusterDataSize = 0;
    m_clusterOpen = true;

    // Record cluster position for cue entries
    m_clusterStartFilePos = m_file.pos() - m_segmentStartPos;

    // Write ClusterTimecode into buffer
    uint8_t buf[16];
    const int len = ebml_write_uint(buf, MATROSKA_ID_CLUSTERTIMECODE, static_cast<uint64_t>(timecodeMs));
    m_clusterBuf.insert(m_clusterBuf.end(), buf, buf + len);
    m_clusterDataSize += len;
}

// ──────────────── Frame data conversion ──────────────────────────────────────

std::vector<uint8_t> MatroskaMuxer::convertAV1ToLowOverhead(const uint8_t* data, int size)
{
    // Convert from start-code-separated OBUs (with emulation prevention bytes)
    // to MKV's "low overhead bitstream format" (obu_has_size_field=1, LEB128 sizes).
    //
    // Per the AV1-in-Matroska spec:
    //   - Temporal delimiter OBUs are stripped.
    //   - Duplicate SEQUENCE_HEADER OBUs are deduplicated (keep only the last one
    //     before the first FRAME/FRAME_HEADER).  The duplicates arise because
    //     extractData() prepends the SH from the codec private, but the SimpleBlock
    //     itself usually contains its own SH with potentially different trailing bits.

    uint8_t* const dataStart = const_cast<uint8_t*>(data);
    uint8_t* const dataEnd = const_cast<uint8_t*>(data + size);

    // Temporary buffer for removing emulation prevention bytes
    std::vector<uint8_t> rawBuf(size);

    // ---- Pass 1: collect each OBU in low-overhead form ----
    struct ConvertedObu
    {
        Av1ObuType type;
        std::vector<uint8_t> bytes;  // header + LEB128 size + raw payload
    };
    std::vector<ConvertedObu> obus;
    obus.reserve(16);

    uint8_t* curObu = NALUnit::findNextNAL(dataStart, dataEnd);

    while (curObu < dataEnd)
    {
        Av1ObuHeader obuHdr;
        const int hdrLen = obuHdr.parse(curObu, dataEnd);
        if (hdrLen < 0)
            break;

        // Find the start of the NEXT start code to determine current OBU's boundary
        uint8_t* nextStartCode = NALUnit::findNALWithStartCode(curObu, dataEnd, true);

        // OBU payload runs from curObu+hdrLen to nextStartCode
        // Trim trailing zero bytes (they're part of the start code prefix, not the OBU)
        uint8_t* obuPayloadEnd = nextStartCode;
        while (obuPayloadEnd > curObu + hdrLen && obuPayloadEnd[-1] == 0) obuPayloadEnd--;

        const uint8_t* payload = curObu + hdrLen;
        const int payloadWithEPLen = static_cast<int>(obuPayloadEnd - payload);

        // Remove emulation prevention bytes from payload
        int rawPayloadLen = 0;
        if (payloadWithEPLen > 0)
        {
            rawPayloadLen =
                av1_remove_emulation_prevention(payload, payload + payloadWithEPLen, rawBuf.data(), rawBuf.size());
            if (rawPayloadLen < 0)
            {
                // Fallback: use payload as-is
                memcpy(rawBuf.data(), payload, payloadWithEPLen);
                rawPayloadLen = payloadWithEPLen;
            }
        }

        // Skip temporal delimiter OBUs (not needed in MKV)
        if (obuHdr.obu_type != Av1ObuType::TEMPORAL_DELIMITER)
        {
            ConvertedObu obu;
            obu.type = obuHdr.obu_type;

            // Write OBU header byte(s) with obu_has_size_field=1 (bit 1)
            const uint8_t hdrByte = curObu[0] | 0x02;
            obu.bytes.push_back(hdrByte);
            if (obuHdr.obu_extension_flag)
                obu.bytes.push_back(curObu[1]);

            // Write LEB128-encoded payload size
            uint8_t leb128Buf[8];
            const int leb128Len = encodeLeb128(leb128Buf, static_cast<uint64_t>(rawPayloadLen));
            obu.bytes.insert(obu.bytes.end(), leb128Buf, leb128Buf + leb128Len);

            // Write raw payload (emulation prevention bytes removed)
            if (rawPayloadLen > 0)
                obu.bytes.insert(obu.bytes.end(), rawBuf.data(), rawBuf.data() + rawPayloadLen);

            obus.push_back(std::move(obu));
        }

        // Advance to the next OBU (skip past the next start code)
        if (nextStartCode < dataEnd)
            curObu = NALUnit::findNextNAL(nextStartCode, dataEnd);
        else
            break;
    }

    // ---- Pass 2: deduplicate SEQUENCE_HEADER OBUs ----
    // If multiple SEQUENCE_HEADERs appear before the first FRAME/FRAME_HEADER,
    // keep only the last one (from the SimpleBlock data, not the codec private copy).
    int lastShIdx = -1;
    int firstFrameIdx = static_cast<int>(obus.size());
    for (int i = 0; i < static_cast<int>(obus.size()); i++)
    {
        if (obus[i].type == Av1ObuType::SEQUENCE_HEADER)
            lastShIdx = i;
        if (obus[i].type == Av1ObuType::FRAME || obus[i].type == Av1ObuType::FRAME_HEADER)
        {
            firstFrameIdx = i;
            break;
        }
    }

    // ---- Pass 3: emit the final byte stream ----
    std::vector<uint8_t> result;
    result.reserve(size);

    for (int i = 0; i < static_cast<int>(obus.size()); i++)
    {
        // Skip duplicate SEQUENCE_HEADERs that precede the first FRAME
        if (obus[i].type == Av1ObuType::SEQUENCE_HEADER && i < firstFrameIdx && i != lastShIdx)
            continue;

        result.insert(result.end(), obus[i].bytes.begin(), obus[i].bytes.end());
    }

    return result;
}

std::vector<uint8_t> MatroskaMuxer::convertAnnexBToLengthPrefixed(const uint8_t* data, int size)
{
    // Convert Annex B start-code-separated NALUs to 4-byte length-prefixed NALUs.
    // This is the format required for H.264/HEVC/VVC in Matroska.

    const uint8_t* end = data + size;
    std::vector<uint8_t> result;
    result.reserve(size);

    uint8_t* curPos = NALUnit::findNextNAL(const_cast<uint8_t*>(data), const_cast<uint8_t*>(end));

    while (curPos < end)
    {
        // Find the next start code to determine NALU boundaries
        uint8_t* nextNal = NALUnit::findNALWithStartCode(curPos, const_cast<uint8_t*>(end), true);

        // NALU data runs from curPos to (nextNal minus trailing zeros of the start code)
        uint8_t* naluEnd = nextNal;
        if (nextNal < end)
        {
            // Back up past the trailing zeros that are part of the next start code
            while (naluEnd > curPos && naluEnd[-1] == 0) naluEnd--;
        }

        const int naluSize = static_cast<int>(naluEnd - curPos);
        if (naluSize > 0)
        {
            // Write 4-byte big-endian length
            result.push_back(static_cast<uint8_t>((naluSize >> 24) & 0xFF));
            result.push_back(static_cast<uint8_t>((naluSize >> 16) & 0xFF));
            result.push_back(static_cast<uint8_t>((naluSize >> 8) & 0xFF));
            result.push_back(static_cast<uint8_t>(naluSize & 0xFF));
            // Write NALU data
            result.insert(result.end(), curPos, naluEnd);
        }

        // Advance to next NALU
        curPos = NALUnit::findNextNAL(nextNal, const_cast<uint8_t*>(end));
    }

    return result;
}

void MatroskaMuxer::flushCluster()
{
    if (!m_clusterOpen || m_clusterBuf.empty())
        return;

    // Write Cluster master element with known size
    uint8_t header[16];
    int hdrLen = ebml_write_id(header, MATROSKA_ID_CLUSTER);
    hdrLen += ebml_write_size(header + hdrLen, m_clusterBuf.size());
    writeToFile(header, hdrLen);
    writeToFile(m_clusterBuf);

    m_clusterBuf.clear();
    m_clusterOpen = false;
    m_clusterDataSize = 0;
}

// ──────────────── muxPacket ──────────────────────────────────────────────────

void MatroskaMuxer::flushPendingFrame(MkvTrackInfo& track)
{
    if (!track.hasPendingFrame || track.pendingFrameData.empty())
    {
        track.hasPendingFrame = false;
        track.pendingFrameData.clear();
        return;
    }

    // Convert the accumulated raw data to MKV format
    const uint8_t* frameData = track.pendingFrameData.data();
    int frameSize = static_cast<int>(track.pendingFrameData.size());
    std::vector<uint8_t> convertedData;

    switch (track.codecID)
    {
    case CODEC_V_AV1:
        convertedData = convertAV1ToLowOverhead(frameData, frameSize);
        break;
    case CODEC_V_MPEG4_H264:
    case CODEC_V_MPEG4_H265:
    case CODEC_V_MPEG4_H266:
        convertedData = convertAnnexBToLengthPrefixed(frameData, frameSize);
        break;
    default:
        break;
    }

    if (!convertedData.empty())
    {
        frameData = convertedData.data();
        frameSize = static_cast<int>(convertedData.size());
    }

    // Compute PTS relative to stream start (convert internal PTS units to milliseconds)
    const int64_t relMs = (track.pendingPts - m_firstTimecode) / INTERNAL_PTS_PER_MS;

    // Track the maximum timecode for the Duration element
    if (relMs > m_lastTimecodeMs)
        m_lastTimecodeMs = relMs;

    // Decide whether to start a new cluster
    const bool needNewCluster = !m_clusterOpen || (relMs - m_clusterTimecodeMs >= CLUSTER_MAX_DURATION_MS) ||
                                (m_clusterDataSize >= CLUSTER_MAX_SIZE) ||
                                (track.trackType == 1 && (track.pendingFlags & AVPacket::IS_IFRAME) && m_clusterOpen &&
                                 (relMs - m_clusterTimecodeMs >= 1000));

    if (needNewCluster)
        startCluster(relMs);

    // Record cue entry for video keyframes
    if (track.trackType == 1 && (track.pendingFlags & AVPacket::IS_IFRAME))
    {
        CueEntry cue;
        cue.timecodeMs = relMs;
        cue.trackNumber = track.trackNumber;
        cue.clusterOffset = m_clusterStartFilePos;
        m_cueEntries.push_back(cue);
    }

    // Write SimpleBlock
    const int16_t relTimeMs = static_cast<int16_t>(relMs - m_clusterTimecodeMs);

    uint8_t trackNumBuf[8];
    const int trackNumLen = ebml_write_size(trackNumBuf, track.trackNumber);
    const int blockPayloadSize = trackNumLen + 2 + 1 + frameSize;

    uint8_t header[16];
    int hdrLen = ebml_write_id(header, MATROSKA_ID_SIMPLEBLOCK);
    hdrLen += ebml_write_size(header + hdrLen, blockPayloadSize);

    m_clusterBuf.insert(m_clusterBuf.end(), header, header + hdrLen);
    m_clusterBuf.insert(m_clusterBuf.end(), trackNumBuf, trackNumBuf + trackNumLen);
    m_clusterBuf.push_back(static_cast<uint8_t>((relTimeMs >> 8) & 0xFF));
    m_clusterBuf.push_back(static_cast<uint8_t>(relTimeMs & 0xFF));

    uint8_t flags = 0;
    if (track.pendingFlags & AVPacket::IS_IFRAME)
        flags |= 0x80;
    m_clusterBuf.push_back(flags);

    m_clusterBuf.insert(m_clusterBuf.end(), frameData, frameData + frameSize);
    m_clusterDataSize += hdrLen + blockPayloadSize;

    track.pendingFrameData.clear();
    track.hasPendingFrame = false;
}

bool MatroskaMuxer::muxPacket(AVPacket& avPacket)
{
    if (avPacket.data == nullptr || avPacket.size == 0)
        return true;

    auto it = m_tracks.find(avPacket.stream_index);
    if (it == m_tracks.end())
        return true;

    // Before the header is written, buffer packets and wait until all tracks
    // have delivered at least one packet.  This ensures all codec readers are
    // fully initialized (e.g. audio sample rate, channels) before we write the
    // Matroska track headers.
    if (!m_headerWritten)
    {
        m_seenStreams.insert(avPacket.stream_index);

        // Buffer a copy of this packet
        BufferedPacket bp;
        bp.stream_index = avPacket.stream_index;
        bp.pts = avPacket.pts;
        bp.flags = avPacket.flags;
        bp.data.assign(avPacket.data, avPacket.data + avPacket.size);
        m_preHeaderPackets.push_back(std::move(bp));

        if (m_seenStreams.size() >= m_tracks.size())
        {
            writeDeferredHeader();
            replayBufferedPackets();
        }
        return true;
    }

    // Track first timecode for relative calculations
    if (!m_firstTimecodeSet)
    {
        m_firstTimecode = avPacket.pts;
        m_firstTimecodeSet = true;
    }

    return muxPacketInternal(avPacket);
}

bool MatroskaMuxer::muxPacketInternal(AVPacket& avPacket)
{
    auto it = m_tracks.find(avPacket.stream_index);
    if (it == m_tracks.end())
        return true;

    MkvTrackInfo& track = it->second;

    // If this packet has a different PTS than the pending frame, flush the pending frame first.
    // This handles the case where the MPEG stream reader splits large frames into multiple
    // packets with the same PTS.
    if (track.hasPendingFrame && avPacket.pts != track.pendingPts)
        flushPendingFrame(track);

    // Accumulate data
    if (!track.hasPendingFrame)
    {
        track.pendingPts = avPacket.pts;
        track.pendingFlags = avPacket.flags;
        track.hasPendingFrame = true;
    }
    else
    {
        // Same PTS - merge flags (keep keyframe flag if any chunk has it)
        track.pendingFlags |= (avPacket.flags & AVPacket::IS_IFRAME);
    }
    track.pendingFrameData.insert(track.pendingFrameData.end(), avPacket.data, avPacket.data + avPacket.size);

    return true;
}

// ──────────────── doFlush ────────────────────────────────────────────────────

bool MatroskaMuxer::doFlush()
{
    // Flush all pending accumulated frames
    for (auto& [streamIdx, track] : m_tracks) flushPendingFrame(track);

    flushCluster();
    return true;
}

// ──────────────── Cues ───────────────────────────────────────────────────────

void MatroskaMuxer::writeCues()
{
    if (m_cueEntries.empty())
        return;

    m_cuesPos = m_file.pos() - m_segmentStartPos;

    // Build all cue point entries
    std::vector<uint8_t> allPoints;

    for (const auto& cue : m_cueEntries)
    {
        // CueTrackPositions content
        uint8_t ctpBuf[64];
        int ctpLen = 0;
        ctpLen += ebml_write_uint(ctpBuf + ctpLen, MATROSKA_ID_CUETRACK, cue.trackNumber);
        ctpLen += ebml_write_uint(ctpBuf + ctpLen, MATROSKA_ID_CUECLUSTERPOSITION, cue.clusterOffset);

        // CuePoint content
        uint8_t cpBuf[128];
        int cpLen = 0;
        cpLen += ebml_write_uint(cpBuf + cpLen, MATROSKA_ID_CUETIME, static_cast<uint64_t>(cue.timecodeMs));

        // CueTrackPositions master
        cpLen += ebml_write_master_open(cpBuf + cpLen, MATROSKA_ID_CUETRACKPOSITION, ctpLen);
        memcpy(cpBuf + cpLen, ctpBuf, ctpLen);
        cpLen += ctpLen;

        // PointEntry master
        uint8_t peBuf[8];
        int peLen = ebml_write_id(peBuf, MATROSKA_ID_POINTENTRY);
        peLen += ebml_write_size(peBuf + peLen, cpLen);
        allPoints.insert(allPoints.end(), peBuf, peBuf + peLen);
        allPoints.insert(allPoints.end(), cpBuf, cpBuf + cpLen);
    }

    // Write Cues master element
    uint8_t header[16];
    int hdrLen = ebml_write_id(header, MATROSKA_ID_CUES);
    hdrLen += ebml_write_size(header + hdrLen, allPoints.size());
    writeToFile(header, hdrLen);
    writeToFile(allPoints);
}

// ──────────────── SeekHead ───────────────────────────────────────────────────

void MatroskaMuxer::writeSeekHead()
{
    // Build seek entries for SegmentInfo, Tracks, and Cues
    struct SeekItem
    {
        uint32_t id;
        int64_t pos;
    };

    std::vector<SeekItem> items;
    items.push_back({MATROSKA_ID_INFO, m_segmentInfoPos});
    items.push_back({MATROSKA_ID_TRACKS, m_tracksPos});
    if (m_cuesPos > 0)
        items.push_back({MATROSKA_ID_CUES, m_cuesPos});

    std::vector<uint8_t> allEntries;

    for (const auto& item : items)
    {
        uint8_t entryBuf[64];
        int entryLen = 0;

        // SeekID
        uint8_t idBytes[4];
        const int idLen = ebml_write_id(idBytes, item.id);
        entryLen += ebml_write_binary(entryBuf + entryLen, MATROSKA_ID_SEEKID, idBytes, idLen);

        // SeekPosition
        entryLen += ebml_write_uint(entryBuf + entryLen, MATROSKA_ID_SEEKPOSITION, static_cast<uint64_t>(item.pos));

        // SeekEntry master
        uint8_t header[8];
        int hdrLen = ebml_write_id(header, MATROSKA_ID_SEEKENTRY);
        hdrLen += ebml_write_size(header + hdrLen, entryLen);
        allEntries.insert(allEntries.end(), header, header + hdrLen);
        allEntries.insert(allEntries.end(), entryBuf, entryBuf + entryLen);
    }

    // Write SeekHead master element
    uint8_t header[16];
    int hdrLen = ebml_write_id(header, MATROSKA_ID_SEEKHEAD);
    hdrLen += ebml_write_size(header + hdrLen, allEntries.size());
    writeToFile(header, hdrLen);
    writeToFile(allEntries);
}

// ──────────────── close ──────────────────────────────────────────────────────

bool MatroskaMuxer::close()
{
    // If the header was never written (e.g. a track never sent data), force-write
    // it now so the file is at least structurally valid.
    if (!m_headerWritten && !m_tracks.empty())
    {
        writeDeferredHeader();
        replayBufferedPackets();
    }

    // Flush any pending accumulated frames for all tracks
    for (auto& [streamIdx, track] : m_tracks) flushPendingFrame(track);

    // Flush any remaining cluster data
    flushCluster();

    // Write Cues
    writeCues();

    // Write SeekHead at the end
    writeSeekHead();

    // Patch the Segment size now that we know the total length
    const int64_t segmentEnd = m_file.pos();
    const uint64_t segmentSize = static_cast<uint64_t>(segmentEnd - m_segmentStartPos);
    m_file.seek(m_segmentSizePos);
    uint8_t sizeBuf[8];
    ebml_write_size_fixed(sizeBuf, segmentSize, 8);
    m_file.write(sizeBuf, 8);

    // Patch Duration element: highest PTS + one frame duration
    if (m_durationValueFilePos > 0 && m_lastTimecodeMs > 0)
    {
        double frameDurationMs = 0.0;
        for (const auto& [idx, track] : m_tracks)
        {
            if (track.trackType == 1 && track.fps > 0)
            {
                frameDurationMs = 1000.0 / track.fps;
                break;
            }
        }
        const double durationMs = static_cast<double>(m_lastTimecodeMs) + frameDurationMs;

        // Write as big-endian IEEE 754 float64
        uint64_t bits;
        memcpy(&bits, &durationMs, 8);
        uint8_t durationBuf[8];
        for (int i = 7; i >= 0; i--)
        {
            durationBuf[i] = static_cast<uint8_t>(bits & 0xFF);
            bits >>= 8;
        }
        m_file.seek(m_durationValueFilePos);
        m_file.write(durationBuf, 8);
    }

    m_file.close();
    return true;
}
