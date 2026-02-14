#include "av1.h"

#include <cstring>

#include "nalUnits.h"
#include "vodCoreException.h"
#include "vod_common.h"

// ---------------------------------------------------------------------------
// LEB128 decoding (AV1 spec Section 4.10.5)
// ---------------------------------------------------------------------------

int64_t decodeLeb128(const uint8_t* buf, const uint8_t* end, int& bytesRead)
{
    int64_t value = 0;
    bytesRead = 0;
    for (int i = 0; i < 8; i++)
    {
        if (buf + i >= end)
            return -1;
        const uint8_t byte = buf[i];
        value |= static_cast<int64_t>(byte & 0x7f) << (i * 7);
        bytesRead = i + 1;
        if ((byte & 0x80) == 0)
            return value;
    }
    return -1;  // too many bytes
}

// ---------------------------------------------------------------------------
// LEB128 encoding (AV1 spec Section 4.10.5)
// ---------------------------------------------------------------------------

int encodeLeb128(uint8_t* dst, uint64_t value)
{
    int bytes = 0;
    do
    {
        uint8_t byte = static_cast<uint8_t>(value & 0x7F);
        value >>= 7;
        if (value > 0)
            byte |= 0x80;  // more bytes follow
        dst[bytes++] = byte;
    } while (value > 0);
    return bytes;
}

// ---------------------------------------------------------------------------
// Av1ObuHeader
// ---------------------------------------------------------------------------

int Av1ObuHeader::parse(const uint8_t* buf, const uint8_t* end)
{
    if (buf >= end)
        return -1;

    const uint8_t b0 = buf[0];
    // obu_forbidden_bit must be 0
    if (b0 & 0x80)
        return -1;

    // obu_reserved_1bit must be 0 (AV1 spec Section 5.3.2)
    if (b0 & 0x01)
        return -1;

    obu_type = static_cast<Av1ObuType>((b0 >> 3) & 0x0f);
    obu_extension_flag = (b0 >> 2) & 1;
    obu_has_size_field = (b0 >> 1) & 1;

    int headerBytes = 1;

    if (obu_extension_flag)
    {
        if (buf + 1 >= end)
            return -1;
        const uint8_t b1 = buf[1];
        temporal_id = (b1 >> 5) & 0x07;
        spatial_id = (b1 >> 3) & 0x03;
        headerBytes = 2;
    }
    else
    {
        temporal_id = 0;
        spatial_id = 0;
    }

    return headerBytes;
}

// ---------------------------------------------------------------------------
// Av1SequenceHeader
// ---------------------------------------------------------------------------

Av1SequenceHeader::Av1SequenceHeader()
    : seq_profile(0),
      still_picture(false),
      reduced_still_picture_header(false),
      seq_level_idx_0(0),
      seq_tier_0(0),
      timing_info_present_flag(false),
      num_units_in_display_tick(0),
      time_scale(0),
      equal_picture_interval(false),
      num_ticks_per_picture_minus_1(0),
      max_frame_width(0),
      max_frame_height(0),
      high_bitdepth(false),
      twelve_bit(false),
      mono_chrome(false),
      color_primaries(2),  // CP_UNSPECIFIED
      transfer_characteristics(2),
      matrix_coefficients(2),
      color_range(false),
      chroma_subsampling_x(0),
      chroma_subsampling_y(0),
      chroma_sample_position(0),
      separate_uv_delta_q(false),
      frame_id_numbers_present_flag(false)
{
}

int Av1SequenceHeader::getBitDepth() const
{
    if (seq_profile == 2 && high_bitdepth)
        return twelve_bit ? 12 : 10;
    if (high_bitdepth)
        return 10;
    return 8;
}

double Av1SequenceHeader::getFPS() const
{
    if (!timing_info_present_flag || num_units_in_display_tick == 0)
        return 0.0;
    double fps = static_cast<double>(time_scale) / num_units_in_display_tick;
    if (equal_picture_interval)
        fps /= (num_ticks_per_picture_minus_1 + 1);
    return fps;
}

// AV1 spec Section 5.5.3 - timing_info()
bool Av1SequenceHeader::parseTimingInfo(BitStreamReader& reader)
{
    try
    {
        num_units_in_display_tick = reader.getBits(32);
        time_scale = reader.getBits(32);
        equal_picture_interval = reader.getBit();
        if (equal_picture_interval)
            num_ticks_per_picture_minus_1 = parseUvlc(reader);
        else
            num_ticks_per_picture_minus_1 = 0;
        return true;
    }
    catch (BitStreamException&)
    {
        return false;
    }
}

// AV1 spec Section 4.10.3 - uvlc()
uint32_t Av1SequenceHeader::parseUvlc(BitStreamReader& reader)
{
    unsigned leadingZeros = 0;
    while (leadingZeros < 32 && reader.getBit() == 0) leadingZeros++;
    if (leadingZeros >= 32)
        return UINT32_MAX;
    const uint32_t value = reader.getBits(leadingZeros);
    return value + (1u << leadingZeros) - 1;
}

// AV1 spec Section 5.5.2 - color_config()
bool Av1SequenceHeader::parseColorConfig(BitStreamReader& reader)
{
    try
    {
        high_bitdepth = reader.getBit();
        if (seq_profile == 2 && high_bitdepth)
            twelve_bit = reader.getBit();
        else
            twelve_bit = false;

        if (seq_profile == 1)
            mono_chrome = false;
        else
            mono_chrome = reader.getBit();

        const bool color_description_present_flag = reader.getBit();
        if (color_description_present_flag)
        {
            color_primaries = reader.getBits<uint8_t>(8);
            transfer_characteristics = reader.getBits<uint8_t>(8);
            matrix_coefficients = reader.getBits<uint8_t>(8);
        }
        else
        {
            color_primaries = 2;  // CP_UNSPECIFIED
            transfer_characteristics = 2;
            matrix_coefficients = 2;
        }

        if (mono_chrome)
        {
            color_range = reader.getBit();
            chroma_subsampling_x = 1;
            chroma_subsampling_y = 1;
            chroma_sample_position = 0;  // CSP_UNKNOWN
            separate_uv_delta_q = false;
            return true;
        }

        if (color_primaries == AV1_CP_BT_709 && transfer_characteristics == AV1_TC_SRGB &&
            matrix_coefficients == AV1_MC_IDENTITY)
        {
            // sRGB special case
            color_range = true;
            chroma_subsampling_x = 0;
            chroma_subsampling_y = 0;
        }
        else
        {
            color_range = reader.getBit();
            if (seq_profile == 0)
            {
                chroma_subsampling_x = 1;
                chroma_subsampling_y = 1;
            }
            else if (seq_profile == 1)
            {
                chroma_subsampling_x = 0;
                chroma_subsampling_y = 0;
            }
            else
            {
                // profile 2
                if (getBitDepth() == 12)
                {
                    chroma_subsampling_x = reader.getBit();
                    if (chroma_subsampling_x)
                        chroma_subsampling_y = reader.getBit();
                    else
                        chroma_subsampling_y = 0;
                }
                else
                {
                    chroma_subsampling_x = 1;
                    chroma_subsampling_y = 0;
                }
            }
            if (chroma_subsampling_x && chroma_subsampling_y)
                chroma_sample_position = reader.getBits<uint8_t>(2);
        }

        separate_uv_delta_q = reader.getBit();
        return true;
    }
    catch (BitStreamException&)
    {
        return false;
    }
}

// AV1 spec Section 5.5 - sequence_header_obu()
int Av1SequenceHeader::deserialize(const uint8_t* buf, const int size)
{
    if (size < 3)
        return -1;

    // Allocate decode buffer for emulation prevention removal
    auto decodeBuf = std::make_unique<uint8_t[]>(size);
    const int decodedLen = av1_remove_emulation_prevention(buf, buf + size, decodeBuf.get(), size);
    const uint8_t* data = (decodedLen > 0) ? decodeBuf.get() : buf;
    const int dataLen = (decodedLen > 0) ? decodedLen : size;

    try
    {
        BitStreamReader reader;
        reader.setBuffer(const_cast<uint8_t*>(data), data + dataLen);

        seq_profile = reader.getBits<uint8_t>(3);
        // AV1 only defines profiles 0, 1, and 2
        if (seq_profile > 2)
            return -1;

        still_picture = reader.getBit();
        reduced_still_picture_header = reader.getBit();

        if (reduced_still_picture_header)
        {
            timing_info_present_flag = false;
            seq_level_idx_0 = reader.getBits<uint8_t>(5);
            seq_tier_0 = 0;
        }
        else
        {
            timing_info_present_flag = reader.getBit();
            bool decoder_model_info_present_flag = false;
            if (timing_info_present_flag)
            {
                if (!parseTimingInfo(reader))
                    return -1;
                decoder_model_info_present_flag = reader.getBit();
                if (decoder_model_info_present_flag)
                {
                    // decoder_model_info(): skip the fields
                    reader.skipBits(5);   // buffer_delay_length_minus_1
                    reader.skipBits(32);  // num_units_in_decoding_tick
                    reader.skipBits(5);   // buffer_removal_time_length_minus_1
                    reader.skipBits(5);   // frame_presentation_time_length_minus_1
                }
            }

            const bool initial_display_delay_present_flag = reader.getBit();
            const unsigned operating_points_cnt_minus_1 = reader.getBits(5);

            for (unsigned i = 0; i <= operating_points_cnt_minus_1; i++)
            {
                reader.skipBits(12);  // operating_point_idc[i]
                const uint8_t level = reader.getBits<uint8_t>(5);
                uint8_t tier = 0;
                if (level > 7)
                    tier = reader.getBit();
                if (i == 0)
                {
                    seq_level_idx_0 = level;
                    seq_tier_0 = tier;
                }
                if (decoder_model_info_present_flag)
                {
                    const bool decoder_model_present = reader.getBit();
                    if (decoder_model_present)
                    {
                        // operating_parameters_info(): skip
                        reader.skipBits(5 + 1);  // buffer_delay_length_minus_1 bits + 1, simplified
                        // This is approximate - we skip the detailed parsing
                        // since we only need basic stream info
                        reader.skipBits(10);  // rough skip
                    }
                }
                if (initial_display_delay_present_flag)
                {
                    const bool present = reader.getBit();
                    if (present)
                        reader.skipBits(4);  // initial_display_delay_minus_1
                }
            }
        }

        // frame_width_bits_minus_1 and frame_height_bits_minus_1
        const unsigned frame_width_bits = reader.getBits(4) + 1;
        const unsigned frame_height_bits = reader.getBits(4) + 1;
        max_frame_width = reader.getBits(frame_width_bits) + 1;
        max_frame_height = reader.getBits(frame_height_bits) + 1;

        // Sanity check: reject obviously invalid resolutions.
        // Minimum 16x16 avoids false positives from random byte patterns,
        // maximum 65536 is the AV1 spec maximum.
        if (max_frame_width < 16 || max_frame_height < 16 || max_frame_width > 65536 || max_frame_height > 65536)
            return -1;

        if (!reduced_still_picture_header)
            frame_id_numbers_present_flag = reader.getBit();
        else
            frame_id_numbers_present_flag = false;

        if (frame_id_numbers_present_flag)
        {
            reader.skipBits(4);  // delta_frame_id_length_minus_2
            reader.skipBits(3);  // additional_frame_id_length_minus_1
        }

        // use_128x128_superblock
        reader.skipBit();
        // enable_filter_intra
        reader.skipBit();
        // enable_intra_edge_filter
        reader.skipBit();

        if (!reduced_still_picture_header)
        {
            // enable_interintra_compound
            reader.skipBit();
            // enable_masked_compound
            reader.skipBit();
            // enable_warped_motion
            reader.skipBit();
            // enable_dual_filter
            reader.skipBit();
            const bool enable_order_hint = reader.getBit();
            if (enable_order_hint)
            {
                // enable_jnt_comp
                reader.skipBit();
                // enable_ref_frame_mvs
                reader.skipBit();
            }

            const bool seq_choose_screen_content_tools = reader.getBit();
            if (!seq_choose_screen_content_tools)
                reader.skipBit();  // seq_force_screen_content_tools

            // We don't need anything past here for basic stream info,
            // but color_config follows after a few more fields.
            // For robustness, let's try to skip to color_config.

            const bool seq_choose_integer_mv = reader.getBit();
            if (!seq_choose_integer_mv)
                reader.skipBit();  // seq_force_integer_mv

            if (enable_order_hint)
                reader.skipBits(3);  // order_hint_bits_minus_1
        }

        // enable_superres
        reader.skipBit();
        // enable_cdef
        reader.skipBit();
        // enable_restoration
        reader.skipBit();

        // color_config()
        if (!parseColorConfig(reader))
            return -1;

        // film_grain_params_present (last field before trailing bits)
        reader.skipBit();

        return 0;
    }
    catch (BitStreamException&)
    {
        return -1;
    }
}

// ---------------------------------------------------------------------------
// Av1FrameHeader
// ---------------------------------------------------------------------------

Av1FrameHeader::Av1FrameHeader()
    : show_existing_frame(false), frame_type(Av1FrameType::KEY_FRAME), show_frame(true), frame_to_show_map_idx(0)
{
}

int Av1FrameHeader::deserialize(const uint8_t* buf, const int size, const Av1SequenceHeader& seqHdr)
{
    if (size < 1)
        return -1;

    // Allocate decode buffer for emulation prevention removal
    const int parseLen = (size > 16) ? 16 : size;  // only need first few bytes
    auto decodeBuf = std::make_unique<uint8_t[]>(parseLen);
    const int decodedLen = av1_remove_emulation_prevention(buf, buf + parseLen, decodeBuf.get(), parseLen);
    const uint8_t* data = (decodedLen > 0) ? decodeBuf.get() : buf;
    const int dataLen = (decodedLen > 0) ? decodedLen : parseLen;

    try
    {
        BitStreamReader reader;
        reader.setBuffer(const_cast<uint8_t*>(data), data + dataLen);

        if (seqHdr.reduced_still_picture_header)
        {
            show_existing_frame = false;
            frame_type = Av1FrameType::KEY_FRAME;
            show_frame = true;
            return 0;
        }

        show_existing_frame = reader.getBit();
        if (show_existing_frame)
        {
            frame_to_show_map_idx = reader.getBits<uint8_t>(3);
            frame_type = Av1FrameType::KEY_FRAME;  // not actually used
            show_frame = true;
            return 0;
        }

        frame_type = static_cast<Av1FrameType>(reader.getBits(2));
        show_frame = reader.getBit();
        return 0;
    }
    catch (BitStreamException&)
    {
        return -1;
    }
}

// ---------------------------------------------------------------------------
// AV1CodecConfigurationRecord parsing
// ---------------------------------------------------------------------------

std::vector<std::vector<uint8_t>> av1_extract_priv_data(const uint8_t* buff, const int size)
{
    std::vector<std::vector<uint8_t>> result;

    if (size < 4)
        return result;

    // AV1CodecConfigurationRecord header (4 bytes):
    // marker(1) + version(7) + seq_profile(3) + seq_level_idx_0(5)
    // seq_tier_0(1) + high_bitdepth(1) + twelve_bit(1) + monochrome(1) +
    // chroma_subsampling_x(1) + chroma_subsampling_y(1) + chroma_sample_position(2)
    // reserved(3) + initial_presentation_delay_present(1) +
    // initial_presentation_delay_minus_one/reserved(4)

    // Check marker bit and version
    if ((buff[0] & 0x80) == 0)
        return result;  // marker must be 1
    const uint8_t version = buff[0] & 0x7f;
    if (version != 1)
        return result;

    // The remaining bytes after the 4-byte header are configOBUs
    const uint8_t* obuData = buff + 4;
    int obuDataLen = size - 4;

    // Parse the OBUs from the config record (low-overhead format with size fields)
    const uint8_t* cur = obuData;
    const uint8_t* end = obuData + obuDataLen;

    while (cur < end)
    {
        Av1ObuHeader hdr;
        const int hdrLen = hdr.parse(cur, end);
        if (hdrLen < 0)
            break;

        int obuPayloadSize = 0;
        if (hdr.obu_has_size_field)
        {
            int leb128Bytes = 0;
            const int64_t sz = decodeLeb128(cur + hdrLen, end, leb128Bytes);
            if (sz < 0)
                break;
            obuPayloadSize = static_cast<int>(sz);

            // Total OBU bytes = header + leb128 size field + payload
            const int totalObuBytes = hdrLen + leb128Bytes + obuPayloadSize;
            if (cur + totalObuBytes > end)
                break;

            // Store the raw OBU (header + payload, without the size field)
            // as a start-code-prefixed entry
            std::vector<uint8_t> obu;
            obu.reserve(3 + hdrLen + obuPayloadSize);
            // Start code
            obu.push_back(0x00);
            obu.push_back(0x00);
            obu.push_back(0x01);
            // OBU header byte(s) - clear the has_size_field bit for TS format
            // (In TS format, size is determined by start codes, not size fields)
            uint8_t headerByte = cur[0] & ~0x02;  // clear obu_has_size_field
            obu.push_back(headerByte);
            if (hdr.obu_extension_flag)
                obu.push_back(cur[1]);
            // OBU payload (need emulation prevention)
            const uint8_t* payloadStart = cur + hdrLen + leb128Bytes;
            // Add emulation prevention to payload
            const size_t maxEncoded = obuPayloadSize * 2 + 16;
            auto tmpBuf = std::make_unique<uint8_t[]>(maxEncoded);
            const int encodedLen =
                av1_add_emulation_prevention(payloadStart, payloadStart + obuPayloadSize, tmpBuf.get(), maxEncoded);
            if (encodedLen > 0)
            {
                obu.insert(obu.end(), tmpBuf.get(), tmpBuf.get() + encodedLen);
            }
            else
            {
                // No emulation prevention needed or error - use raw payload
                obu.insert(obu.end(), payloadStart, payloadStart + obuPayloadSize);
            }

            result.push_back(std::move(obu));
            cur += totalObuBytes;
        }
        else
        {
            // Without size field, we can't determine OBU boundaries
            break;
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Emulation prevention (same algorithm as H.264/HEVC Annex-B)
// ---------------------------------------------------------------------------

int av1_add_emulation_prevention(const uint8_t* src, const uint8_t* srcEnd, uint8_t* dst, size_t dstSize)
{
    return NALUnit::encodeNAL(src, srcEnd, dst, dstSize);
}

int av1_remove_emulation_prevention(const uint8_t* src, const uint8_t* srcEnd, uint8_t* dst, size_t dstSize)
{
    return NALUnit::decodeNAL(src, srcEnd, dst, dstSize);
}
