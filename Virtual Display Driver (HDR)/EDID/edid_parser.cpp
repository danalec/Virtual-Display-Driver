#include <vector>
#include <tuple>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <map>
#include <set>
#include <cmath>

#ifdef _WIN32
#include <windows.h>
#endif

// The profile stream carries std::fixed and whatever precision the last write
// set: three decimals for the mode list, four for the chromaticity. A
// luminance written straight into it inherited that, so the real minimum
// display mastering luminance of 0.0034 nits came out as 0.003, and the
// summary printed it as 0.00. Write it on its own stream instead.
std::string FormatLuminance(double nits) {
    std::ostringstream os;
    os << std::defaultfloat << std::setprecision(6) << nits;
    return os.str();
}

struct VideoMode {
    int x, y, Z, refresh;
    VideoMode(int x, int y, int Z, int refresh) : x(x), y(y), Z(Z), refresh(refresh) {}
};

struct HDRMetadata {
    bool hdr10_supported = false;
    bool dolby_vision_supported = false;
    bool hdr10_plus_supported = false;
    double max_luminance = 0.0;
    double min_luminance = 0.0;
    double max_frame_avg_luminance = 0.0;
    
    // Static metadata type 1 (HDR10)
    struct {
        uint16_t display_primaries_x[3] = {0}; // R, G, B
        uint16_t display_primaries_y[3] = {0};
        uint16_t white_point_x = 0;
        uint16_t white_point_y = 0;
        uint16_t max_display_mastering_luminance = 0;
        uint16_t min_display_mastering_luminance = 0;
        uint16_t max_content_light_level = 0;
        uint16_t max_frame_avg_light_level = 0;
    } static_metadata_type1;
};

struct ColorProfile {
    enum ColorSpace {
        sRGB,
        Rec709,
        Rec2020,
        DCI_P3,
        Adobe_RGB,
        UNKNOWN
    };
    
    ColorSpace primary_colorspace = UNKNOWN;
    std::vector<ColorSpace> supported_colorspaces;
    double gamma = 2.2;
    
    // Chromaticity coordinates
    double red_x = 0, red_y = 0;
    double green_x = 0, green_y = 0;
    double blue_x = 0, blue_y = 0;
    double white_x = 0, white_y = 0;
};

class EDIDParser {
private:
    std::vector<uint8_t> edid_data;
    std::vector<VideoMode> video_modes;
    HDRMetadata hdr_metadata;
    ColorProfile color_profile;
    
    // The display's preferred timing, taken from the first detailed timing descriptor of
    // the base block. The profile used to pick the mode with the most pixels instead.
    bool has_preferred_mode = false;
    VideoMode preferred_mode{0, 0, 1000, 60};
    
    // Helper functions
    uint16_t read_uint16_le(const uint8_t* data) {
        return data[0] | (data[1] << 8);
    }
    
    uint16_t read_uint16_be(const uint8_t* data) {
        return (data[0] << 8) | data[1];
    }
    
    // The refresh rate is carried as (nominal Hz, per-mille correction) and the driver
    // recomputes it as nominal * Z / 1000. This used to recognise only the NTSC factor
    // 1000/1001 and forced Z to 1000 for everything else, so a timing whose real rate
    // was 48.0769 Hz was reported as exactly 48 Hz and the difference was lost without
    // a word. Z now carries the residual: the NTSC case still lands on 999 (59.94006 Hz
    // -> 999) and every other rate keeps the fraction it actually has.
    int calculate_z_multiplier(double actual_refresh, int nominal_refresh) {
        if (nominal_refresh <= 0) return 1000;
        return static_cast<int>(actual_refresh * 1000.0 / nominal_refresh + 0.5);
    }
    
    void parse_base_edid() {
        if (edid_data.size() < 128) return;
        
        std::cout << "=== Base EDID Analysis ===" << std::endl;
        
        // Check manufacturer ID
        uint16_t mfg_id = read_uint16_be(&edid_data[8]);
        char mfg_name[4] = {
            (char)('@' + ((mfg_id >> 10) & 0x1F)),
            (char)('@' + ((mfg_id >> 5) & 0x1F)),
            (char)('@' + (mfg_id & 0x1F)),
            0
        };
        std::cout << "Manufacturer: " << mfg_name << std::endl;
        
        // Check for known HDR-supporting manufacturers and models
        uint16_t product_id = read_uint16_le(&edid_data[10]);
        std::cout << "Product ID: 0x" << std::hex << product_id << std::dec << std::endl;
        
        // Check feature support (byte 24)
        uint8_t feature_support = edid_data[24];
        std::cout << "Feature support byte: 0x" << std::hex << (int)feature_support << std::dec << std::endl;
        std::cout << "  Digital input: " << ((feature_support & 0x80) ? "Yes" : "No") << std::endl;
        std::cout << "  Color depth: ";
        if (feature_support & 0x80) {
            uint8_t color_depth = (feature_support >> 4) & 0x07;
            switch (color_depth) {
                case 1: std::cout << "6 bits"; break;
                case 2: std::cout << "8 bits"; break;
                case 3: std::cout << "10 bits"; break;
                case 4: std::cout << "12 bits"; break;
                case 5: std::cout << "14 bits"; break;
                case 6: std::cout << "16 bits"; break;
                default: std::cout << "undefined"; break;
            }
        }
        std::cout << std::endl;

        // The display gamma is EDID byte 23, stored as (gamma * 100) - 100, so 120 means
        // 2.2. 0xFF means the EDID does not define it here -- the gamma then comes from
        // the extension block -- and the sRGB default stands in for it. The value used to
        // be hardcoded at 2.2, so an EDID asking for a different gamma was ignored.
        uint8_t gamma_byte = edid_data[23];
        if (gamma_byte == 0xFF) {
            std::cout << "Display gamma: not defined by this EDID (byte 23 = 0xFF), keeping "
                      << color_profile.gamma << std::endl;
        } else {
            color_profile.gamma = (gamma_byte + 100) / 100.0;
            std::cout << "Display gamma: " << color_profile.gamma
                      << " (EDID byte 23 = 0x" << std::hex << (int)gamma_byte << std::dec
                      << ")" << std::endl;
        }
        
        // Parse display descriptors (might contain HDR info)
        std::cout << "Checking display descriptors for HDR hints..." << std::endl;
        for (int i = 0; i < 4; i++) {
            parse_display_descriptor(&edid_data[54 + i * 18], i);
        }
        
        // Parse established timings (bytes 35-37)
        parse_established_timings();
        
        // Parse standard timings (bytes 38-53)
        parse_standard_timings();
        
        // Parse detailed timing descriptors (bytes 54-125)
        for (int i = 0; i < 4; i++) {
            if (edid_data[54 + i * 18] != 0 || edid_data[54 + i * 18 + 1] != 0) {
                parse_detailed_timing_descriptor(&edid_data[54 + i * 18]);
            }
        }
        
        capture_preferred_mode();
        
        // Parse chromaticity (bytes 25-34)
        parse_chromaticity();
        
        std::cout << "=== End Base EDID Analysis ===" << std::endl << std::endl;
    }
    
    void parse_display_descriptor(const uint8_t* desc, int index) {
        if (desc[0] == 0 && desc[1] == 0) {
            // This is a display descriptor, not a timing
            uint8_t desc_type = desc[3];
            
            std::cout << "  Display Descriptor " << index << " - Type: 0x" 
                      << std::hex << (int)desc_type << std::dec;
            
            switch (desc_type) {
                case 0xFF: 
                    std::cout << " (Serial Number)"; 
                    break;
                case 0xFE: 
                    std::cout << " (Unspecified Text)"; 
                    break;
                case 0xFD: 
                    std::cout << " (Display Range Limits)";
                    parse_display_range_limits(&desc[5]);
                    break;
                case 0xFC: 
                    std::cout << " (Display Name)"; 
                    break;
                case 0xFB: 
                    std::cout << " (Color Point Data)"; 
                    break;
                case 0xFA: 
                    std::cout << " (Standard Timing IDs)"; 
                    break;
                case 0xF9: 
                    std::cout << " (Display Color Management)"; 
                    break;
                case 0xF8: 
                    std::cout << " (CVT 3-Byte Timing Codes)"; 
                    break;
                case 0xF7: 
                    std::cout << " (Established Timings III)"; 
                    break;
                default: 
                    std::cout << " (Unknown)"; 
                    break;
            }
            std::cout << std::endl;
            
            // Dump descriptor data for analysis
            std::cout << "    Data: ";
            for (int i = 5; i < 18; i++) {
                std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)desc[i] << " ";
            }
            std::cout << std::dec << std::endl;
        }
    }
    
    void parse_display_range_limits(const uint8_t* range_data) {
        std::cout << std::endl;
        std::cout << "    Min V Rate: " << (int)range_data[0] << " Hz" << std::endl;
        std::cout << "    Max V Rate: " << (int)range_data[1] << " Hz" << std::endl;
        std::cout << "    Min H Rate: " << (int)range_data[2] << " kHz" << std::endl;
        std::cout << "    Max H Rate: " << (int)range_data[3] << " kHz" << std::endl;
        std::cout << "    Max Pixel Clock: " << (int)range_data[4] * 10 << " MHz" << std::endl;
        
        // Check for extended timing information (GTF, CVT, etc.)
        if (range_data[5] != 0) {
            std::cout << "    Timing formula: 0x" << std::hex << (int)range_data[5] << std::dec << std::endl;
        }
    }
    
    void parse_established_timings() {
        const uint8_t* est = &edid_data[35];
        
        // Byte 35 - established timings I
        if (est[0] & 0x80) video_modes.emplace_back(720, 400, 1000, 70);   // 720x400@70Hz
        if (est[0] & 0x40) video_modes.emplace_back(720, 400, 1000, 88);   // 720x400@88Hz
        if (est[0] & 0x20) video_modes.emplace_back(640, 480, 1000, 60);   // 640x480@60Hz
        if (est[0] & 0x10) video_modes.emplace_back(640, 480, 1000, 67);   // 640x480@67Hz
        if (est[0] & 0x08) video_modes.emplace_back(640, 480, 1000, 72);   // 640x480@72Hz
        if (est[0] & 0x04) video_modes.emplace_back(640, 480, 1000, 75);   // 640x480@75Hz
        if (est[0] & 0x02) video_modes.emplace_back(800, 600, 1000, 56);   // 800x600@56Hz
        if (est[0] & 0x01) video_modes.emplace_back(800, 600, 1000, 60);   // 800x600@60Hz
        
        // Byte 36 - established timings II
        if (est[1] & 0x80) video_modes.emplace_back(800, 600, 1000, 72);   // 800x600@72Hz
        if (est[1] & 0x40) video_modes.emplace_back(800, 600, 1000, 75);   // 800x600@75Hz
        if (est[1] & 0x20) video_modes.emplace_back(832, 624, 1000, 75);   // 832x624@75Hz
        if (est[1] & 0x10) video_modes.emplace_back(1024, 768, 1000, 87);  // 1024x768@87Hz
        if (est[1] & 0x08) video_modes.emplace_back(1024, 768, 1000, 60);  // 1024x768@60Hz
        if (est[1] & 0x04) video_modes.emplace_back(1024, 768, 1000, 70);  // 1024x768@70Hz
        if (est[1] & 0x02) video_modes.emplace_back(1024, 768, 1000, 75);  // 1024x768@75Hz
        if (est[1] & 0x01) video_modes.emplace_back(1280, 1024, 1000, 75); // 1280x1024@75Hz
    }
    
    void parse_standard_timings() {
        for (int i = 0; i < 8; i++) {
            const uint8_t* st = &edid_data[38 + i * 2];
            
            if (st[0] == 0x01 && st[1] == 0x01) continue; // Unused
            
            int h_active = (st[0] + 31) * 8;
            int aspect_ratio = (st[1] >> 6) & 0x03;
            int refresh = (st[1] & 0x3F) + 60;
            
            int v_active;
            switch (aspect_ratio) {
                case 0: v_active = h_active * 10 / 16; break; // 16:10
                case 1: v_active = h_active * 3 / 4; break;   // 4:3
                case 2: v_active = h_active * 4 / 5; break;   // 5:4
                case 3: v_active = h_active * 9 / 16; break;  // 16:9
            }
            
            video_modes.emplace_back(h_active, v_active, 1000, refresh);
        }
    }
    
    // Decode a Detailed Timing Descriptor. Returns false when the descriptor is not a
    // timing at all (the first two bytes are zero) or when it carries nothing usable.
    bool decode_detailed_timing(const uint8_t* dtd, int& h_active, int& v_active,
                                double& refresh_rate, bool& interlaced) {
        if (dtd[0] == 0 && dtd[1] == 0) {
            // Display descriptor, not timing
            return false;
        }
        
        // 32 bits, not 16: read_uint16_le returns the raw clock in units of
        // 10 kHz, and multiplying by 10 gives kHz. A 1080p60 DTD is 14850, which
        // is 148500 kHz -- that does not fit in a uint16_t and wrapped to
        // 17428, so every refresh rate above 655.35 MHz came out as garbage.
        // 1920x1080 at 148.5 MHz was reported as 7 Hz, and the profile the tool
        // wrote that into is the profile the driver reads.
        uint32_t pixel_clock = static_cast<uint32_t>(read_uint16_le(dtd)) * 10; // in kHz
        h_active = dtd[2] | ((dtd[4] & 0xF0) << 4);
        int h_blanking = dtd[3] | ((dtd[4] & 0x0F) << 8);
        v_active = dtd[5] | ((dtd[7] & 0xF0) << 4);
        int v_blanking = dtd[6] | ((dtd[7] & 0x0F) << 8);
        interlaced = (dtd[17] & 0x80) != 0;
        
        if (!h_active || !v_active || !pixel_clock) {
            return false;
        }
        
        uint32_t total_pixels = (h_active + h_blanking) * (v_active + v_blanking);
        refresh_rate = (pixel_clock * 1000.0) / total_pixels;
        return true;
    }
    
    void parse_detailed_timing_descriptor(const uint8_t* dtd) {
        int h_active = 0, v_active = 0;
        double refresh_rate = 0.0;
        bool interlaced = false;
        
        if (!decode_detailed_timing(dtd, h_active, v_active, refresh_rate, interlaced)) {
            return;
        }
        
        if (interlaced) {
            // The vertical values of an interlaced descriptor are per field, so the frame
            // is twice as tall and runs at half the field rate. The 1080i24 descriptor in
            // digihome_edid.bin decoded as a 1920x540 mode at 48 Hz: a resolution the
            // display does not have, at a rate that is not the frame rate. The mode list
            // has no interlace field -- the driver reports (width, height, vsyncNum,
            // vsyncDen) -- so an interlaced mode would be reported as progressive, which
            // is a different mode than the display has. Skip it, the same way the
            // interlaced CTA VICs are skipped.
            std::cout << "  Interlaced detailed timing " << h_active << "x" << (v_active * 2)
                      << " skipped: the mode list cannot express interlace" << std::endl;
            return;
        }
        
        int nominal_refresh = static_cast<int>(refresh_rate + 0.5);
        if (nominal_refresh <= 0) {
            return;
        }
        int z_multiplier = calculate_z_multiplier(refresh_rate, nominal_refresh);
        
        video_modes.emplace_back(h_active, v_active, z_multiplier, nominal_refresh);
    }
    
    // The EDID carries the display's preferred timing in the first detailed timing
    // descriptor of the base block (bytes 54-71), and byte 24 bit 1 is the flag that
    // says the descriptor is there. The profile used to pick the mode with the most
    // pixels instead, which named a resolution the display never said it preferred.
    void capture_preferred_mode() {
        int h_active = 0, v_active = 0;
        double refresh_rate = 0.0;
        bool interlaced = false;
        bool found = false;
        
        if (edid_data[24] & 0x02) {
            found = decode_detailed_timing(&edid_data[54], h_active, v_active,
                                           refresh_rate, interlaced);
        }
        
        if (!found) {
            // The EDID does not flag descriptor 1, or descriptor 1 is not a timing:
            // take the first timing the descriptor block holds.
            for (int i = 0; i < 4 && !found; i++) {
                found = decode_detailed_timing(&edid_data[54 + i * 18], h_active, v_active,
                                               refresh_rate, interlaced);
            }
        }
        
        if (!found || interlaced) {
            return;
        }
        
        int nominal_refresh = static_cast<int>(refresh_rate + 0.5);
        if (nominal_refresh <= 0) {
            return;
        }
        
        preferred_mode = VideoMode(h_active, v_active,
                                   calculate_z_multiplier(refresh_rate, nominal_refresh),
                                   nominal_refresh);
        has_preferred_mode = true;
        
        std::cout << "  Preferred timing: " << preferred_mode.x << "x" << preferred_mode.y
                  << " @ " << (preferred_mode.Z * preferred_mode.refresh) / 1000.0
                  << " Hz" << std::endl;
    }
    
    void parse_chromaticity() {
        const uint8_t* chrom = &edid_data[25];
        
        // Extract chromaticity coordinates (10-bit precision)
        color_profile.red_x = ((chrom[2] << 2) | (chrom[0] >> 6)) / 1024.0;
        color_profile.red_y = ((chrom[3] << 2) | ((chrom[0] >> 4) & 0x03)) / 1024.0;
        color_profile.green_x = ((chrom[4] << 2) | ((chrom[0] >> 2) & 0x03)) / 1024.0;
        color_profile.green_y = ((chrom[5] << 2) | (chrom[0] & 0x03)) / 1024.0;
        color_profile.blue_x = ((chrom[6] << 2) | (chrom[1] >> 6)) / 1024.0;
        color_profile.blue_y = ((chrom[7] << 2) | ((chrom[1] >> 4) & 0x03)) / 1024.0;
        color_profile.white_x = ((chrom[8] << 2) | ((chrom[1] >> 2) & 0x03)) / 1024.0;
        color_profile.white_y = ((chrom[9] << 2) | (chrom[1] & 0x03)) / 1024.0;
        
        // Determine primary colorspace based on chromaticity
        determine_primary_colorspace();
    }
    
    void determine_primary_colorspace() {
        // sRGB/Rec.709 coordinates (approximate)
        const double srgb_red_x = 0.64, srgb_red_y = 0.33;
        const double srgb_green_x = 0.30, srgb_green_y = 0.60;
        const double srgb_blue_x = 0.15, srgb_blue_y = 0.06;

        // Rec.2020 coordinates
        const double rec2020_red_x = 0.708, rec2020_red_y = 0.292;
        const double rec2020_green_x = 0.170, rec2020_green_y = 0.797;
        const double rec2020_blue_x = 0.131, rec2020_blue_y = 0.046;

        // DCI-P3 coordinates
        const double dcip3_red_x = 0.680, dcip3_red_y = 0.320;
        const double dcip3_green_x = 0.265, dcip3_green_y = 0.690;
        const double dcip3_blue_x = 0.150, dcip3_blue_y = 0.060;

        double tolerance = 0.02;

        // The blue primaries are part of the comparison. They used to be left out
        // of every test -- and the sRGB coordinates were never used at all --
        // so a display whose red and green drifted but whose blue did not was
        // still called Rec.2020, and anything that matched nothing was called
        // sRGB rather than unknown.
        if (std::abs(color_profile.red_x - rec2020_red_x) < tolerance &&
            std::abs(color_profile.red_y - rec2020_red_y) < tolerance &&
            std::abs(color_profile.green_x - rec2020_green_x) < tolerance &&
            std::abs(color_profile.green_y - rec2020_green_y) < tolerance &&
            std::abs(color_profile.blue_x - rec2020_blue_x) < tolerance &&
            std::abs(color_profile.blue_y - rec2020_blue_y) < tolerance) {
            color_profile.primary_colorspace = ColorProfile::Rec2020;
            color_profile.supported_colorspaces.push_back(ColorProfile::Rec2020);
        }
        // Check for DCI-P3
        else if (std::abs(color_profile.red_x - dcip3_red_x) < tolerance &&
                 std::abs(color_profile.red_y - dcip3_red_y) < tolerance &&
                 std::abs(color_profile.green_x - dcip3_green_x) < tolerance &&
                 std::abs(color_profile.green_y - dcip3_green_y) < tolerance &&
                 std::abs(color_profile.blue_x - dcip3_blue_x) < tolerance &&
                 std::abs(color_profile.blue_y - dcip3_blue_y) < tolerance) {
            color_profile.primary_colorspace = ColorProfile::DCI_P3;
            color_profile.supported_colorspaces.push_back(ColorProfile::DCI_P3);
        }
        // sRGB and Rec.709 share their primaries.
        else if (std::abs(color_profile.red_x - srgb_red_x) < tolerance &&
                 std::abs(color_profile.red_y - srgb_red_y) < tolerance &&
                 std::abs(color_profile.green_x - srgb_green_x) < tolerance &&
                 std::abs(color_profile.green_y - srgb_green_y) < tolerance &&
                 std::abs(color_profile.blue_x - srgb_blue_x) < tolerance &&
                 std::abs(color_profile.blue_y - srgb_blue_y) < tolerance) {
            color_profile.primary_colorspace = ColorProfile::Rec709;
            color_profile.supported_colorspaces.push_back(ColorProfile::Rec709);
        }
        // Not a colour space we know. Say so rather than claiming sRGB.
        else {
            color_profile.primary_colorspace = ColorProfile::UNKNOWN;
            color_profile.supported_colorspaces.push_back(ColorProfile::UNKNOWN);
        }
    }
    
    void parse_cea_extension(const uint8_t* cea_block) {
        if (cea_block[0] != 0x02) return; // Not a CEA extension
        
        // Byte 2 is the offset of the first detailed timing descriptor. The data block
        // collection runs from byte 4 to just before it. CTA-861 gives byte 2 two special
        // values: 04 means the collection is empty (the descriptors start at 4), and 00
        // means there are no descriptors at all and the collection takes up the whole
        // rest of the block ahead of the checksum.
        //
        // dtd_start is a byte read straight out of the EDID file, and a CEA extension
        // block is only 128 bytes. Checking the lower bound alone is not enough: a value
        // above 127 sends both loops below past the end of the block. The caller
        // guarantees 128 bytes are present and not one byte more.
        uint8_t dtd_start = cea_block[2];
        if (dtd_start > 127 || (dtd_start > 0 && dtd_start < 4)) {
            std::cout << "ERROR: DTD start offset " << (int)dtd_start
                      << " is outside the 0..127 range of a CEA extension block" << std::endl;
            return;
        }
        
        // The collection ends where the descriptors begin, or at the last byte before the
        // checksum when there are none. The parser used to reject byte 2 = 00 outright,
        // which threw away the whole collection and with it every mode the video data
        // block advertises.
        const uint8_t collection_end = (dtd_start == 0) ? 127 : dtd_start;
        // The data blocks start at offset 4 of the extension block. Byte 3 is
        // NOT the data block collection offset: in the three EDIDs in this
        // repository it holds 0xF0, 0xF1 and 0x40, and a collection that
        // starts there would run past the end of the block. Walking the
        // collection from offset 4 ends exactly at dtd_start in all three.
        const uint8_t data_block_start = 4;
        std::cout << "=== CEA Extension Block Debug ===" << std::endl;
        std::cout << "CEA revision: " << (int)cea_block[1] << std::endl;
        if (dtd_start == 0) {
            std::cout << "DTD start offset: 0 (the EDID states there are no detailed timing descriptors)" << std::endl;
        } else {
            std::cout << "DTD start offset: " << (int)dtd_start << std::endl;
        }

        // Dump the entire data block collection for debugging
        std::cout << "Data block collection bytes (" << (collection_end - data_block_start) << " bytes): ";
        for (uint8_t i = data_block_start; i < collection_end; i++) {
            std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)cea_block[i] << " ";
        }
        std::cout << std::dec << std::endl;

        // Parse data block collection
        for (uint8_t offset = data_block_start; offset < collection_end; ) {
            uint8_t tag_length = cea_block[offset];
            uint8_t tag = (tag_length >> 5) & 0x07;
            uint8_t length = tag_length & 0x1F;
            
            std::cout << "Data block at offset " << (int)offset << ": tag=" << (int)tag 
                      << ", length=" << (int)length << std::endl;
            
            if (offset + 1 + length > collection_end) {
                std::cout << "ERROR: Data block extends beyond the data block collection!" << std::endl;
                break;
            }
            
            // Dump block data
            std::cout << "  Block data: ";
            for (uint8_t i = 0; i < length; i++) {
                std::cout << std::hex << std::setw(2) << std::setfill('0') 
                          << (int)cea_block[offset + 1 + i] << " ";
            }
            std::cout << std::dec << std::endl;
            
            switch (tag) {
                case 1: // Audio data block
                    std::cout << "  → Audio Data Block" << std::endl;
                    break;
                case 2: // Video data block
                    std::cout << "  → Video Data Block" << std::endl;
                    parse_video_data_block(&cea_block[offset + 1], length);
                    break;
                case 3: // Vendor specific data block
                    std::cout << "  → Vendor Specific Data Block" << std::endl;
                    parse_vendor_specific_block(&cea_block[offset + 1], length);
                    break;
                case 4: // Speaker allocation data block
                    std::cout << "  → Speaker Allocation Data Block" << std::endl;
                    break;
                case 7: // Extended tag
                    std::cout << "  → Extended Tag Block" << std::endl;
                    parse_extended_tag_block(&cea_block[offset + 1], length);
                    break;
                default:
                    std::cout << "  → Unknown/Reserved Block (tag " << (int)tag << ")" << std::endl;
                    break;
            }
            
            offset += 1 + length;
        }
        
        // Parse DTDs in CEA extension
        if (dtd_start == 0) {
            std::cout << "No detailed timing descriptors in this CEA extension block" << std::endl;
        }
        else {
            for (uint8_t offset = dtd_start; offset + 18 <= 128; offset += 18) {
                if (cea_block[offset] == 0 && cea_block[offset + 1] == 0) {
                    std::cout << "  Empty DTD slot at offset " << (int)offset << std::endl;
                    continue;
                }
                std::cout << "  Parsing DTD at offset " << (int)offset << std::endl;
                parse_detailed_timing_descriptor(&cea_block[offset]);
            }
        }
        
        std::cout << "=== End CEA Extension Debug ===" << std::endl << std::endl;
    }
    
    void parse_video_data_block(const uint8_t* vdb, uint8_t length) {
        for (uint8_t i = 0; i < length; i++) {
            uint8_t vic = vdb[i] & 0x7F;
            bool native = (vdb[i] & 0x80) != 0;
            
            // Add video modes based on VIC (Video Identification Code)
            add_vic_mode(vic);
        }
    }
    
    void add_vic_mode(uint8_t vic) {
        struct VICMode {
            int x, y, refresh;
            bool fractional;
        };
        
        static const std::map<uint8_t, VICMode> vic_table = {
            {1, {640, 480, 60, false}}, {2, {720, 480, 60, true}}, {3, {720, 480, 60, true}}, {4, {1280, 720, 60, true}}, 
            {5, {1920, 1080, 60, true}}, {6, {720, 480, 60, true}}, {7, {720, 480, 60, true}}, {8, {720, 240, 60, true}}, 
            {9, {720, 240, 60, true}}, {10, {2880, 480, 60, true}}, {11, {2880, 480, 60, true}}, {12, {2880, 240, 60, true}}, 
            {13, {2880, 240, 60, true}}, {14, {1440, 480, 60, true}}, {15, {1440, 480, 60, true}}, {16, {1920, 1080, 60, true}},
            {17, {720, 576, 50, false}}, {18, {720, 576, 50, false}}, {19, {1280, 720, 50, false}}, {20, {1920, 1080, 50, false}},
            {21, {720, 576, 50, false}}, {22, {720, 576, 50, false}}, {23, {720, 288, 50, false}}, {24, {720, 288, 50, false}},
            {25, {2880, 576, 50, false}}, {26, {2880, 576, 50, false}}, {27, {2880, 288, 50, false}}, {28, {2880, 288, 50, false}},
            {29, {1440, 576, 50, false}}, {30, {1440, 576, 50, false}}, {31, {1920, 1080, 50, false}}, {32, {1920, 1080, 24, true}},
            {33, {1920, 1080, 25, false}}, {34, {1920, 1080, 30, true}}, {35, {2880, 480, 60, true}}, {36, {2880, 480, 60, true}},
            {37, {2880, 576, 50, false}}, {38, {2880, 576, 50, false}}, {39, {1920, 1080, 50, false}}, {40, {1920, 1080, 100, false}},
            {41, {1280, 720, 100, false}}, {42, {720, 576, 100, false}}, {43, {720, 576, 100, false}}, {44, {720, 576, 200, false}},
            {45, {720, 576, 200, false}}, {46, {1920, 1080, 120, true}}, {47, {1280, 720, 120, true}}, {48, {720, 480, 120, true}},
            {49, {720, 480, 120, true}}, {50, {720, 480, 240, true}}, {51, {720, 480, 240, true}}, {52, {720, 576, 200, false}},
            {53, {720, 576, 200, false}}, {54, {720, 576, 240, false}}, {55, {720, 576, 240, false}}, {56, {720, 480, 240, true}},
            {57, {720, 480, 240, true}}, {58, {1280, 720, 120, true}}, {59, {1920, 1080, 120, true}}, {60, {1280, 720, 24, false}},
            {61, {1280, 720, 25, false}}, {62, {1280, 720, 30, false}}, {63, {1920, 1080, 120, false}}, {64, {1920, 1080, 100, false}},
            {65, {1280, 720, 24, false}}, {66, {1280, 720, 25, false}}, {67, {1280, 720, 30, false}}, {68, {1280, 720, 50, false}},
            {69, {1280, 720, 60, false}}, {70, {1280, 720, 100, false}}, {71, {1280, 720, 120, false}}, {72, {1920, 1080, 24, false}},
            {73, {1920, 1080, 25, false}}, {74, {1920, 1080, 30, false}}, {75, {1920, 1080, 50, false}}, {76, {1920, 1080, 60, false}},
            {77, {1920, 1080, 100, false}}, {78, {1920, 1080, 120, false}}, {79, {1680, 720, 24, false}}, {80, {1680, 720, 25, false}},
            {81, {1680, 720, 30, false}}, {82, {1680, 720, 50, false}}, {83, {1680, 720, 60, false}}, {84, {1680, 720, 100, false}},
            {85, {1680, 720, 120, false}}, {86, {2560, 1080, 24, false}}, {87, {2560, 1080, 25, false}}, {88, {2560, 1080, 30, false}},
            {89, {2560, 1080, 50, false}}, {90, {2560, 1080, 60, false}}, {91, {2560, 1080, 100, false}}, {92, {2560, 1080, 120, false}},
            {93, {3840, 2160, 24, false}}, {94, {3840, 2160, 25, false}}, {95, {3840, 2160, 30, true}}, {96, {3840, 2160, 50, false}},
            {97, {3840, 2160, 60, true}}, {98, {4096, 2160, 24, true}}, {99, {4096, 2160, 25, false}}, {100, {4096, 2160, 30, true}},
            {101, {4096, 2160, 50, false}}, {102, {4096, 2160, 60, true}}, {103, {3840, 2160, 24, false}}, {104, {3840, 2160, 25, false}},
            {105, {3840, 2160, 30, false}}, {106, {3840, 2160, 50, false}}, {107, {3840, 2160, 60, false}}, {108, {1280, 720, 48, false}},
            {109, {1280, 720, 48, false}}, {110, {1680, 720, 48, false}}, {111, {1920, 1080, 48, false}}, {112, {1920, 1080, 48, false}},
            {113, {2560, 1080, 48, false}}, {114, {3840, 2160, 48, false}}, {115, {4096, 2160, 48, false}}, {116, {3840, 2160, 100, false}},
            {117, {3840, 2160, 120, false}}, {118, {3840, 2160, 100, false}}, {119, {3840, 2160, 120, false}}, {120, {3840, 2160, 100, false}},
            {121, {5120, 2160, 24, false}}, {122, {5120, 2160, 25, false}}, {123, {5120, 2160, 30, true}}, {124, {5120, 2160, 48, false}},
            {125, {5120, 2160, 50, false}}, {126, {5120, 2160, 60, true}}, {127, {5120, 2160, 100, false}}, {193, {5120, 2160, 120, false}},
            {194, {7680, 4320, 24, false}}, {195, {7680, 4320, 25, false}}, {196, {7680, 4320, 30, true}}, {197, {7680, 4320, 48, false}},
            {198, {7680, 4320, 50, false}}, {199, {7680, 4320, 60, true}}, {200, {7680, 4320, 100, false}}, {201, {7680, 4320, 120, false}},
            {202, {7680, 4320, 24, false}}, {203, {7680, 4320, 25, false}}, {204, {7680, 4320, 30, false}}, {205, {7680, 4320, 48, false}},
            {206, {7680, 4320, 50, false}}, {207, {7680, 4320, 60, false}}, {208, {7680, 4320, 100, false}}, {209, {7680, 4320, 120, false}},
            {210, {10240, 4320, 24, false}}, {211, {10240, 4320, 25, false}}, {212, {10240, 4320, 30, true}}, {213, {10240, 4320, 48, false}},
            {214, {10240, 4320, 50, false}}, {215, {10240, 4320, 60, true}}, {216, {10240, 4320, 100, false}}, {217, {10240, 4320, 120, false}},
            {218, {4096, 2160, 100, false}}, {219, {4096, 2160, 120, false}}
        };
        
        // Four of the CTA-861 VICs are interlaced: 5 is 1920x1080i, 6 is
        // 720(1440)x480i, and 21 and 22 are 720(1440)x576i. The mode list has no
        // interlace field -- the driver reports (width, height, vsyncNum,
        // vsyncDen) -- so an interlaced mode added here would be reported as
        // progressive, which is a different mode than the display has. Skip them
        // rather than report one that does not exist.
        switch (vic) {
            case 5: case 6: case 21: case 22:
                std::cout << "  VIC " << (int)vic << " is interlaced, skipped" << std::endl;
                return;
            default: break;
        }

        auto it = vic_table.find(vic);
        if (it != vic_table.end()) {
            const VICMode& mode = it->second;
            int z_multiplier = mode.fractional ? 999 : 1000;
            video_modes.emplace_back(mode.x, mode.y, z_multiplier, mode.refresh);
        }
    }
    
    void parse_vendor_specific_block(const uint8_t* vsdb, uint8_t length) {
        if (length < 3) return;
        
        // An IEEE OUI is stored in an EDID vendor specific data block least significant
        // byte first. Measured in the three EDIDs of this repository: the HDMI LLC OUI
        // (00-0C-03) appears as 03 0C 00, the HDMI Forum OUI (C4-5D-D8) as D8 5D C4 and
        // the Dolby OUI (00-0D-1E) as 1E 0D 00. The accumulation below is that order.
        // The HDR10+ constant was 0x90848B, which is neither this order nor the other
        // one, so that branch could never be taken -- and the HDMI Forum had no branch
        // at all.
        uint32_t oui = vsdb[0] | (vsdb[1] << 8) | (vsdb[2] << 16);
        
        std::cout << "  Vendor Specific Data Block:" << std::endl;
        std::cout << "    OUI: 0x" << std::hex << std::setw(6) << std::setfill('0') << oui << std::dec << std::endl;
        
        if (oui == 0x000C03) { // HDMI LLC OUI (00-0C-03, stored least significant byte first)
            std::cout << "    → HDMI LLC Vendor Specific Data Block" << std::endl;
            if (length > 5) {
                // Parse HDMI capabilities
                std::cout << "    HDMI capabilities found" << std::endl;
            }
        } else if (oui == 0xC45DD8) { // HDMI Forum OUI (C4-5D-D8)
            std::cout << "    → HDMI Forum Vendor Specific Data Block" << std::endl;
            parse_hdmi_forum_vsdb(vsdb, length);
        } else if (oui == 0x000D1E) { // Dolby Laboratories OUI (00-0D-1E)
            std::cout << "    → Dolby Laboratories Vendor Specific Data Block" << std::endl;
            parse_dolby_vendor_block(&vsdb[3], length - 3);
        } else if (oui == 0x8B4890) { // HDR10+ Technologies OUI (8B-48-90, stored least significant byte first)
            std::cout << "    → HDR10+ Technologies Vendor Specific Data Block" << std::endl;
            parse_hdr10plus_vendor_block(&vsdb[3], length - 3);
        } else {
            std::cout << "    → Unknown Vendor (OUI: 0x" << std::hex << oui << std::dec << ")" << std::endl;
            // Dump unknown vendor data
            std::cout << "    Data: ";
            for (uint8_t i = 3; i < length; i++) {
                std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)vsdb[i] << " ";
            }
            std::cout << std::dec << std::endl;
        }
    }
    
    void parse_dolby_vendor_block(const uint8_t* dolby_data, uint8_t length) {
        std::cout << "      Dolby Vendor Block parsing:" << std::endl;
        std::cout << "      Length: " << (int)length << " bytes" << std::endl;
        
        if (length > 0) {
            std::cout << "      Dolby data: ";
            for (uint8_t i = 0; i < length; i++) {
                std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)dolby_data[i] << " ";
            }
            std::cout << std::dec << std::endl;
            
            // Dolby Vision is often indicated by presence of this vendor block
            hdr_metadata.dolby_vision_supported = true;
            std::cout << "      Dolby Vision Support Detected!" << std::endl;
            
            if (length >= 1) {
                uint8_t dolby_caps = dolby_data[0];
                std::cout << "      Dolby capabilities: 0x" << std::hex << (int)dolby_caps << std::dec << std::endl;
                
                // Parse Dolby Vision profiles/modes if present
                if (length >= 2) {
                    uint8_t profiles = dolby_data[1];
                    std::cout << "      Dolby Vision profiles: 0x" << std::hex << (int)profiles << std::dec << std::endl;
                }
            }
        }
    }
    
    void parse_hdr10plus_vendor_block(const uint8_t* hdr10plus_data, uint8_t length) {
        std::cout << "      HDR10+ Vendor Block parsing:" << std::endl;
        std::cout << "      Length: " << (int)length << " bytes" << std::endl;
        
        if (length > 0) {
            std::cout << "      HDR10+ data: ";
            for (uint8_t i = 0; i < length; i++) {
                std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)hdr10plus_data[i] << " ";
            }
            std::cout << std::dec << std::endl;
            
            hdr_metadata.hdr10_plus_supported = true;
            std::cout << "      HDR10+ Support Detected!" << std::endl;
        }
    }
    
    void parse_extended_tag_block(const uint8_t* etb, uint8_t length) {
        if (length < 1) return;
        
        uint8_t extended_tag = etb[0];
        
        std::cout << "    Extended tag: 0x" << std::hex << (int)extended_tag << std::dec 
                  << " (" << (int)extended_tag << "), payload length: " << (int)(length - 1) << std::endl;
        
        // Dump payload
        std::cout << "    Payload: ";
        for (uint8_t i = 1; i < length; i++) {
            std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)etb[i] << " ";
        }
        std::cout << std::dec << std::endl;
        
        switch (extended_tag) {
            case 0x00: std::cout << "    → Video Capability Data Block" << std::endl; break;
            case 0x01: std::cout << "    → Vendor-Specific Video Data Block" << std::endl; break;
            case 0x02: std::cout << "    → VESA Display Device Data Block" << std::endl; break;
            case 0x03: std::cout << "    → VESA Video Timing Block Extension" << std::endl; break;
            case 0x04: std::cout << "    → HDMI Video Data Block" << std::endl; break;
            case 0x05: std::cout << "    → Colorimetry Data Block" << std::endl; break;
            case 0x06:
                std::cout << "    → HDR Static Metadata Data Block" << std::endl;
                // The length here is the length of the EXTENDED TAG block, and
                // that count includes the extended tag code byte. The payload
                // after the code is therefore length - 1 bytes. Treating it as
                // length would read one byte past the payload -- the first
                // byte of the next data block -- and take it for the maximum
                // mastering luminance.
                parse_hdr_static_metadata(&etb[1], length - 1);
                break;
            case 0x07:
                std::cout << "    → HDR Dynamic Metadata Data Block" << std::endl;
                parse_hdr_dynamic_metadata(&etb[1], length - 1);
                break;
            case 0x08: std::cout << "    → Video Format Preference Data Block" << std::endl; break;
            case 0x09: std::cout << "    → YCbCr 4:2:0 Video Data Block" << std::endl; break;
            case 0x0A: std::cout << "    → YCbCr 4:2:0 Capability Map Data Block" << std::endl; break;
            case 0x0B: std::cout << "    → Miscellaneous Audio Fields" << std::endl; break;
            case 0x0C: std::cout << "    → Vendor-Specific Audio Data Block" << std::endl; break;
            case 0x0D: std::cout << "    → HDMI Audio Data Block" << std::endl; break;
            case 0x0E: std::cout << "    → Room Configuration Data Block" << std::endl; break;
            case 0x0F: std::cout << "    → Speaker Location Data Block" << std::endl; break;
            case 0x10: std::cout << "    → Infoframe Data Block" << std::endl; break;
            case 0x11: std::cout << "    → DisplayID Type VII Video Timing Data Block" << std::endl; break;
            case 0x12: 
                std::cout << "    → HDMI Forum Vendor Specific Data Block" << std::endl;
                parse_hdmi_forum_vsdb(&etb[1], length - 1);
                break;
            case 0x13: std::cout << "    → Device Resource Allocation Data Block" << std::endl; break;
            default:
                std::cout << "    → Unknown Extended Tag (0x" << std::hex << (int)extended_tag << std::dec << ")" << std::endl;
                break;
        }
    }
    
    void parse_hdr_static_metadata(const uint8_t* hdr_data, uint8_t length) {
        std::cout << "      HDR Static Metadata parsing:" << std::endl;
        std::cout << "      Length: " << (int)length << " bytes" << std::endl;
        
        if (length < 2) {
            std::cout << "      ERROR: HDR Static Metadata too short!" << std::endl;
            return;
        }
        
        uint8_t eotf_support = hdr_data[0];
        uint8_t metadata_type_support = hdr_data[1];
        
        std::cout << "      EOTF support byte: 0x" << std::hex << (int)eotf_support << std::dec << std::endl;
        std::cout << "        SDR Gamma: " << ((eotf_support & 0x01) ? "Yes" : "No") << std::endl;
        std::cout << "        HDR Gamma: " << ((eotf_support & 0x02) ? "Yes" : "No") << std::endl;
        std::cout << "        SMPTE ST2084 (HDR10): " << ((eotf_support & 0x04) ? "Yes" : "No") << std::endl;
        std::cout << "        Hybrid Log-Gamma (HLG): " << ((eotf_support & 0x08) ? "Yes" : "No") << std::endl;
        
        std::cout << "      Metadata type support: 0x" << std::hex << (int)metadata_type_support << std::dec << std::endl;
        std::cout << "        Static Metadata Type 1: " << ((metadata_type_support & 0x01) ? "Yes" : "No") << std::endl;
        
        // EOTF support
        hdr_metadata.hdr10_supported = (eotf_support & 0x04) != 0; // PQ (SMPTE ST 2084)
        
        // Static metadata type 1 support (HDR10)
        if (metadata_type_support & 0x01) {
            std::cout << "      Processing Static Metadata Type 1..." << std::endl;
            
            if (length >= 3) {
                hdr_metadata.static_metadata_type1.max_display_mastering_luminance = hdr_data[2];
                std::cout << "      Max Display Mastering Luminance (raw): " << (int)hdr_data[2] << std::endl;
            }
            if (length >= 4) {
                hdr_metadata.static_metadata_type1.min_display_mastering_luminance = hdr_data[3];
                std::cout << "      Min Display Mastering Luminance (raw): " << (int)hdr_data[3] << std::endl;
            }
            if (length >= 5) {
                // MaxCLL and MaxFALL are 16-bit fields. Reading one byte of each
                // gave 16 nits for a display that reports 1040, and the byte
                // that was read as MaxFALL was really the high byte of MaxCLL.
                if (length >= 6) {
                    hdr_metadata.static_metadata_type1.max_content_light_level =
                        (uint16_t)(hdr_data[4] | (hdr_data[5] << 8));
                    std::cout << "      Max Content Light Level (raw): "
                              << hdr_metadata.static_metadata_type1.max_content_light_level << std::endl;
                }
                if (length >= 8) {
                    hdr_metadata.static_metadata_type1.max_frame_avg_light_level =
                        (uint16_t)(hdr_data[6] | (hdr_data[7] << 8));
                    std::cout << "      Max Frame Avg Light Level (raw): "
                              << hdr_metadata.static_metadata_type1.max_frame_avg_light_level << std::endl;
                }
            }
        }

        // Convert to actual luminance values.
        //
        // CTA-861.3: Desired Max Display Mastering Luminance is a 6-bit value in
        // units of 50 nits, and Desired Min Display Mastering Luminance is a
        // 6-bit value in units of 0.0001 nits. The code used an exponential
        // formula on the unmasked byte for the maximum and a linear
        // interpolation for the minimum, which are not the units the field is
        // in. The top two bits of each byte are reserved and have to be masked
        // off before the value is read.
        if (hdr_metadata.static_metadata_type1.max_display_mastering_luminance > 0) {
            hdr_metadata.max_luminance =
                50.0 * (hdr_metadata.static_metadata_type1.max_display_mastering_luminance & 0x3F);
            std::cout << "      Calculated Max Luminance: " << hdr_metadata.max_luminance << " nits" << std::endl;
        }

        if (hdr_metadata.static_metadata_type1.min_display_mastering_luminance > 0) {
            hdr_metadata.min_luminance =
                0.0001 * (hdr_metadata.static_metadata_type1.min_display_mastering_luminance & 0x3F);
            std::cout << "      Calculated Min Luminance: " << hdr_metadata.min_luminance << " nits" << std::endl;
        }
        
        std::cout << "      HDR10 Support Set: " << (hdr_metadata.hdr10_supported ? "Yes" : "No") << std::endl;
    }
    
    void parse_hdr_dynamic_metadata(const uint8_t* hdr_data, uint8_t length) {
        std::cout << "      HDR Dynamic Metadata parsing:" << std::endl;
        std::cout << "      Length: " << (int)length << " bytes" << std::endl;
        
        if (length < 1) {
            std::cout << "      ERROR: HDR Dynamic Metadata too short!" << std::endl;
            return;
        }
        
        uint8_t type_support = hdr_data[0];
        
        std::cout << "      Type support byte: 0x" << std::hex << (int)type_support << std::dec << std::endl;
        std::cout << "        Type 1 (HDR10): " << ((type_support & 0x01) ? "Yes" : "No") << std::endl;
        std::cout << "        Type 2 (Dolby Vision): " << ((type_support & 0x02) ? "Yes" : "No") << std::endl;
        std::cout << "        Type 3: " << ((type_support & 0x04) ? "Yes" : "No") << std::endl;
        std::cout << "        Type 4 (HDR10+): " << ((type_support & 0x08) ? "Yes" : "No") << std::endl;
        
        // Dynamic metadata type support
        hdr_metadata.hdr10_plus_supported = (type_support & 0x08) != 0; // HDR10+ is often type 4
        hdr_metadata.dolby_vision_supported = (type_support & 0x02) != 0; // Dolby Vision
        
        if (length > 1) {
            std::cout << "      Additional bytes: ";
            for (uint8_t i = 1; i < length; i++) {
                std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)hdr_data[i] << " ";
            }
            std::cout << std::dec << std::endl;
        }
        
        std::cout << "      HDR10+ Support Set: " << (hdr_metadata.hdr10_plus_supported ? "Yes" : "No") << std::endl;
        std::cout << "      Dolby Vision Support Set: " << (hdr_metadata.dolby_vision_supported ? "Yes" : "No") << std::endl;
    }
    
    // The HDMI Forum VSDB carries what the forum added on top of the HDMI LLC block.
    // The block starts with the IEEE OUI, both in a CTA-861 extension and in the
    // DisplayID extended tag, so the OUI is part of the data handed over.
    void parse_hdmi_forum_vsdb(const uint8_t* hf_data, uint8_t length) {
        if (length < 3) return;
        
        uint32_t oui = hf_data[0] | (hf_data[1] << 8) | (hf_data[2] << 16);
        if (oui != 0xC45DD8) {
            return;
        }
        
        std::cout << "      HDMI Forum Vendor Block parsing:" << std::endl;
        std::cout << "      Length: " << (int)(length - 3) << " bytes" << std::endl;
        std::cout << "      HDMI Forum data: ";
        for (uint8_t i = 3; i < length; i++) {
            std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)hf_data[i] << " ";
        }
        std::cout << std::dec << std::endl;
    }

public:
    bool load_edid_file(const std::string& filename) {
        std::ifstream file(filename, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "Error: Cannot open file " << filename << std::endl;
            return false;
        }
        
        file.seekg(0, std::ios::end);
        std::streampos endPos = file.tellg();
        file.seekg(0, std::ios::beg);

        // tellg() answers -1 when the seek is not supported -- a named pipe, a
        // device, anything that is not a regular file. Converting that to
        // size_t gives SIZE_MAX, and resizing a vector to SIZE_MAX throws
        // std::length_error, which nothing here catches, so the process ends in
        // std::terminate(). An EDID is 128 or 256 bytes, so the upper bound is
        // generous and the lower bound is the real check.
        if (endPos <= 0) {
            std::cerr << "Error: Cannot determine the size of " << filename
                      << " (the stream is not seekable?)" << std::endl;
            return false;
        }
        size_t file_size = static_cast<size_t>(endPos);
        if (file_size < 128) {
            std::cerr << "Error: EDID file too small" << std::endl;
            return false;
        }
        if (file_size > 64 * 1024) {
            std::cerr << "Error: " << filename << " is " << file_size
                      << " bytes, which is not an EDID" << std::endl;
            return false;
        }

        try {
            edid_data.resize(file_size);
        }
        catch (const std::exception& e) {
            std::cerr << "Error: Could not allocate " << file_size
                      << " bytes for the EDID: " << e.what() << std::endl;
            return false;
        }

        file.read(reinterpret_cast<char*>(edid_data.data()), file_size);
        if (!file || static_cast<size_t>(file.gcount()) != file_size) {
            // A short read leaves the tail of edid_data zero-filled, and the
            // parser would go on to decode a partially-read EDID as if it
            // were complete.
            std::cerr << "Error: Could not read all of " << filename << std::endl;
            return false;
        }

        // The same three rules the driver applies to a custom EDID, so that the
        // tool and the driver agree on what an EDID is.
        //
        // The header magic: every EDID starts with 00 FF FF FF FF FF FF 00. A file
        // without it is not an EDID, and the parser used to report one anyway -- a
        // PNG with 120 bytes of rubbish produced a full monitor profile and exit
        // code 0.
        static const BYTE edid_header[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
        if (file_size < 8 || memcmp(edid_data.data(), edid_header, 8) != 0) {
            std::cerr << "Error: " << filename << " does not start with the EDID header" << std::endl;
            return false;
        }

        // The extension count: byte 126 says how many 128-byte blocks follow the
        // base block, so the file has to be exactly that many blocks. A file that
        // declares more than it carries was silently parsed as a shorter EDID, and
        // one that declares fewer had its extra block ignored while the tool still
        // claimed to have read the extension.
        const size_t declared = 128 * (static_cast<size_t>(edid_data[126]) + 1);
        if (file_size != declared) {
            std::cerr << "Error: " << filename << " is " << file_size
                      << " bytes but declares " << static_cast<int>(edid_data[126])
                      << " extension block(s), so it should be " << declared << " bytes" << std::endl;
            return false;
        }

        // Every block carries its own checksum and the sum of a whole block has to
        // be a multiple of 256. Only the base block was checked, so a file with a
        // corrupt extension produced a full profile and exit code 0.
        for (size_t block = 0; block < file_size; block += 128) {
            unsigned int sum = 0;
            for (size_t i = 0; i < 128; ++i) {
                sum += edid_data[block + i];
            }
            if ((sum & 0xFF) != 0) {
                std::cerr << "Error: the block at offset " << block << " of " << filename
                          << " has a bad checksum" << std::endl;
                return false;
            }
        }

        return true;
    }
    
    void parse_edid() {
        video_modes.clear();
        
        std::cout << "=== EDID Parsing Debug ===" << std::endl;
        std::cout << "Total EDID size: " << edid_data.size() << " bytes" << std::endl;
        
        // Parse base EDID (first 128 bytes)
        std::cout << "Parsing base EDID (128 bytes)..." << std::endl;
        parse_base_edid();
        
        // Parse extensions
        if (edid_data.size() > 128) {
            uint8_t extension_count = edid_data[126];
            std::cout << "Extension count: " << (int)extension_count << std::endl;
            
            for (int ext = 0; ext < extension_count && (ext + 2) * 128 <= edid_data.size(); ext++) {
                const uint8_t* extension = &edid_data[128 + ext * 128];
                
                std::cout << "Extension " << (ext + 1) << " type: 0x" << std::hex 
                          << (int)extension[0] << std::dec << std::endl;
                
                // Dump first 16 bytes of extension for debugging
                std::cout << "Extension " << (ext + 1) << " header: ";
                for (int i = 0; i < 16 && i < 128; i++) {
                    std::cout << std::hex << std::setw(2) << std::setfill('0') 
                              << (int)extension[i] << " ";
                }
                std::cout << std::dec << std::endl;
                
                switch (extension[0]) {
                    case 0x02: // CEA extension
                        std::cout << "Processing CEA extension..." << std::endl;
                        parse_cea_extension(extension);
                        break;
                    case 0x70: // DisplayID extension block
                        // The DisplayID extension block has tag code 0x70, not 0xF0.
                        // It was printed as an unknown type and dropped without a
                        // word, so a profile written from an EDID whose modes live
                        // in that block came out with almost nothing in it. The
                        // block is still not parsed, and now it says so.
                        std::cout << "DisplayID extension block found (not parsed; the modes it carries are missing from the profile)" << std::endl;
                        break;
                    default:
                        std::cout << "Unknown extension type: 0x" << std::hex 
                                  << (int)extension[0] << std::dec << std::endl;
                        break;
                }
            }
        } else {
            std::cout << "No extensions found" << std::endl;
        }
        
        // Remove duplicates
        std::cout << "Video modes before deduplication: " << video_modes.size() << std::endl;
        std::sort(video_modes.begin(), video_modes.end(), 
                  [](const VideoMode& a, const VideoMode& b) {
                      if (a.x != b.x) return a.x < b.x;
                      if (a.y != b.y) return a.y < b.y;
                      if (a.refresh != b.refresh) return a.refresh < b.refresh;
                      return a.Z < b.Z;
                  });
        
        video_modes.erase(std::unique(video_modes.begin(), video_modes.end(),
                                      [](const VideoMode& a, const VideoMode& b) {
                                          return a.x == b.x && a.y == b.y && 
                                                 a.refresh == b.refresh && a.Z == b.Z;
                                      }), 
                          video_modes.end());
        
        std::cout << "Video modes after deduplication: " << video_modes.size() << std::endl;
        std::cout << "=== End EDID Parsing Debug ===" << std::endl << std::endl;
    }
    
    std::vector<std::tuple<int, int, int, int>> get_video_modes() const {
        std::vector<std::tuple<int, int, int, int>> modes;
        for (const auto& mode : video_modes) {
            modes.emplace_back(mode.x, mode.y, mode.Z, mode.refresh);
        }
        return modes;
    }
    
    const HDRMetadata& get_hdr_metadata() const {
        return hdr_metadata;
    }
    
    const ColorProfile& get_color_profile() const {
        return color_profile;
    }
    
    std::string generate_iddcx_profile() const {
        std::stringstream profile;
        
        // Find highest specs mode
        auto highest_mode = std::max_element(video_modes.begin(), video_modes.end(),
            [](const VideoMode& a, const VideoMode& b) {
                int pixels_a = a.x * a.y;
                int pixels_b = b.x * b.y;
                if (pixels_a != pixels_b) return pixels_a < pixels_b;
                return a.refresh < b.refresh;
            });
        
        if (highest_mode == video_modes.end()) {
            return "No video modes found";
        }
        
        // The EDID's preferred timing wins over "the mode with the most pixels": the
        // SONY TV in this repository prefers 3840x2160 and the profile said 4096x2160.
        const VideoMode& preferred = has_preferred_mode ? preferred_mode : *highest_mode;
        
        profile << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
        profile << "<IddCxMonitorConfig>\n";
        profile << "  <MonitorModes>\n";
        
        // Add all video modes
        for (const auto& mode : video_modes) {
            double actual_refresh = (mode.Z * mode.refresh) / 1000.0;
            
            profile << "    <MonitorMode>\n";
            profile << "      <Width>" << mode.x << "</Width>\n";
            profile << "      <Height>" << mode.y << "</Height>\n";
            profile << "      <RefreshRate>" << std::fixed << std::setprecision(3) << actual_refresh << "</RefreshRate>\n";
            profile << "      <RefreshRateMultiplier>" << mode.Z << "</RefreshRateMultiplier>\n";
            profile << "      <NominalRefreshRate>" << mode.refresh << "</NominalRefreshRate>\n";
            profile << "    </MonitorMode>\n";
        }
        
        profile << "  </MonitorModes>\n";
        
        // HDR capabilities
        if (hdr_metadata.hdr10_supported || hdr_metadata.dolby_vision_supported) {
            profile << "  <HDRCapabilities>\n";
            profile << "    <HDR10Supported>" << (hdr_metadata.hdr10_supported ? "true" : "false") << "</HDR10Supported>\n";
            profile << "    <DolbyVisionSupported>" << (hdr_metadata.dolby_vision_supported ? "true" : "false") << "</DolbyVisionSupported>\n";
            profile << "    <HDR10PlusSupported>" << (hdr_metadata.hdr10_plus_supported ? "true" : "false") << "</HDR10PlusSupported>\n";
            
            if (hdr_metadata.max_luminance > 0) {
                profile << "    <MaxLuminance>" << FormatLuminance(hdr_metadata.max_luminance) << "</MaxLuminance>\n";
            }
            if (hdr_metadata.min_luminance > 0) {
                profile << "    <MinLuminance>" << FormatLuminance(hdr_metadata.min_luminance) << "</MinLuminance>\n";
            }
            
            profile << "  </HDRCapabilities>\n";
        }
        
        // Color profile
        profile << "  <ColorProfile>\n";
        
        std::string colorspace_name;
        switch (color_profile.primary_colorspace) {
            case ColorProfile::sRGB: colorspace_name = "sRGB"; break;
            case ColorProfile::Rec709: colorspace_name = "Rec709"; break;
            case ColorProfile::Rec2020: colorspace_name = "Rec2020"; break;
            case ColorProfile::DCI_P3: colorspace_name = "DCI-P3"; break;
            case ColorProfile::Adobe_RGB: colorspace_name = "Adobe RGB"; break;
            default: colorspace_name = "Unknown"; break;
        }
        
        profile << "    <PrimaryColorSpace>" << colorspace_name << "</PrimaryColorSpace>\n";
        profile << "    <Gamma>" << color_profile.gamma << "</Gamma>\n";
        
        profile << "    <Chromaticity>\n";
        profile << "      <RedX>" << std::fixed << std::setprecision(4) << color_profile.red_x << "</RedX>\n";
        profile << "      <RedY>" << color_profile.red_y << "</RedY>\n";
        profile << "      <GreenX>" << color_profile.green_x << "</GreenX>\n";
        profile << "      <GreenY>" << color_profile.green_y << "</GreenY>\n";
        profile << "      <BlueX>" << color_profile.blue_x << "</BlueX>\n";
        profile << "      <BlueY>" << color_profile.blue_y << "</BlueY>\n";
        profile << "      <WhiteX>" << color_profile.white_x << "</WhiteX>\n";
        profile << "      <WhiteY>" << color_profile.white_y << "</WhiteY>\n";
        profile << "    </Chromaticity>\n";
        profile << "  </ColorProfile>\n";
        
        // Preferred mode (the EDID's own preferred timing when it has one)
        profile << "  <PreferredMode>\n";
        profile << "    <Width>" << preferred.x << "</Width>\n";
        profile << "    <Height>" << preferred.y << "</Height>\n";
        profile << "    <RefreshRate>" << std::fixed << std::setprecision(3) << (preferred.Z * preferred.refresh) / 1000.0 << "</RefreshRate>\n";
        profile << "  </PreferredMode>\n";
        
        profile << "</IddCxMonitorConfig>\n";
        
        return profile.str();
    }
    
    void print_summary() const {
        std::cout << "=== EDID Parser Results ===" << std::endl;
        std::cout << "Video Modes Found: " << video_modes.size() << std::endl;
        
        for (const auto& mode : video_modes) {
            double actual_refresh = (mode.Z * mode.refresh) / 1000.0;
            std::cout << "  " << mode.x << "x" << mode.y << "@" 
                      << std::fixed << std::setprecision(2) << actual_refresh << "Hz"
                      << " (Z=" << mode.Z << ", nominal=" << mode.refresh << "Hz)" << std::endl;
        }
        
        std::cout << "\nHDR Support:" << std::endl;
        std::cout << "  HDR10: " << (hdr_metadata.hdr10_supported ? "Yes" : "No") << std::endl;
        std::cout << "  Dolby Vision: " << (hdr_metadata.dolby_vision_supported ? "Yes" : "No") << std::endl;
        std::cout << "  HDR10+: " << (hdr_metadata.hdr10_plus_supported ? "Yes" : "No") << std::endl;
        
        if (hdr_metadata.max_luminance > 0) {
            std::cout << "  Max Luminance: " << FormatLuminance(hdr_metadata.max_luminance) << " nits" << std::endl;
        }
        if (hdr_metadata.min_luminance > 0) {
            std::cout << "  Min Luminance: " << FormatLuminance(hdr_metadata.min_luminance) << " nits" << std::endl;
        }
        
        std::cout << "\nColor Profile:" << std::endl;
        std::string colorspace_name;
        switch (color_profile.primary_colorspace) {
            case ColorProfile::sRGB: colorspace_name = "sRGB/Rec.709"; break;
            case ColorProfile::Rec709: colorspace_name = "Rec.709"; break;
            case ColorProfile::Rec2020: colorspace_name = "Rec.2020"; break;
            case ColorProfile::DCI_P3: colorspace_name = "DCI-P3"; break;
            case ColorProfile::Adobe_RGB: colorspace_name = "Adobe RGB"; break;
            default: colorspace_name = "Unknown"; break;
        }
        std::cout << "  Primary Colorspace: " << colorspace_name << std::endl;
        std::cout << "  Gamma: " << color_profile.gamma << std::endl;
        std::cout << "  Chromaticity - Red: (" << std::fixed << std::setprecision(3) 
                  << color_profile.red_x << ", " << color_profile.red_y << ")" << std::endl;
        std::cout << "  Chromaticity - Green: (" << color_profile.green_x << ", " << color_profile.green_y << ")" << std::endl;
        std::cout << "  Chromaticity - Blue: (" << color_profile.blue_x << ", " << color_profile.blue_y << ")" << std::endl;
        std::cout << "  Chromaticity - White: (" << color_profile.white_x << ", " << color_profile.white_y << ")" << std::endl;
        
        if (has_preferred_mode) {
            std::cout << "\nPreferred Mode:" << std::endl;
            std::cout << "  " << preferred_mode.x << "x" << preferred_mode.y << "@"
                      << std::fixed << std::setprecision(2)
                      << (preferred_mode.Z * preferred_mode.refresh) / 1000.0
                      << "Hz" << std::endl;
        }
    }
};

// Usage example and main function
int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cout << "Usage: " << argv[0] << " <edid.bin>" << std::endl;
        return 1;
    }
    
    EDIDParser parser;
    
    if (!parser.load_edid_file(argv[1])) {
        return 1;
    }
    
    parser.parse_edid();
    
    // Get video modes as requested tuple format
    auto modes = parser.get_video_modes();
    
    std::cout << "Video modes in tuple format {x, y, Z, refresh}:" << std::endl;
    for (const auto& mode : modes) {
        std::cout << "{" << std::get<0>(mode) << ", " << std::get<1>(mode) 
                  << ", " << std::get<2>(mode) << ", " << std::get<3>(mode) << "}" << std::endl;
    }
    
    std::cout << std::endl;
    parser.print_summary();
    
    // Generate IDDCX profile
    std::cout << "\n=== IDDCX Profile ===" << std::endl;
    std::cout << parser.generate_iddcx_profile() << std::endl;
    
    // Save profile to file
    std::ofstream profile_file("monitor_profile.xml");
    if (!profile_file.is_open()) {
        // Silently keeping the old profile and exiting 0 tells the user
        // nothing, and the stale file stays in place.
        std::cerr << "Error: Could not open monitor_profile.xml for writing" << std::endl;
        return 1;
    }
    profile_file << parser.generate_iddcx_profile();
    profile_file.close();
    if (!profile_file) {
        std::cerr << "Error: Could not write monitor_profile.xml" << std::endl;
        return 1;
    }
    std::cout << "\nIDDCX profile saved to monitor_profile.xml" << std::endl;

    return 0;
}