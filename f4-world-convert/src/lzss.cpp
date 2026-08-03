// f4-world-convert/src/lzss.cpp — faithful port of FreeFalcon LZSS_Expand.

#include <f4/world_convert/lzss.hpp>

#include <cstring>
#include <stdexcept>

namespace f4::world_convert {

namespace {

// The flag-byte state machine. FreeFalcon's InputBit reads bits LSB-first
// from a flag byte; when 8 bits are consumed, a new flag byte is loaded.
// We inline this into the expand loop rather than reproducing the
// pointer-aliasing dance the original C code does.
struct BitReader {
    const uint8_t* cur;
    const uint8_t* end;
    uint8_t flag_byte;
    int flag_mask;   // next bit to test; 0x100 means "reload"

    explicit BitReader(const uint8_t* start, const uint8_t* e)
        : cur(start), end(e), flag_byte(0), flag_mask(0x100) {}

    // Returns the next flag bit (1 = literal, 0 = match) and advances.
    // Sets `reloaded` true if a new flag byte was consumed this call.
    int next_bit(bool& reloaded) {
        reloaded = false;
        if (flag_mask == 0x100) {
            if (cur >= end) throw std::runtime_error("LZSS: truncated flag byte");
            flag_byte = *cur++;
            flag_mask = 1;
            reloaded = true;
        }
        int bit = (flag_byte & flag_mask) ? 1 : 0;
        flag_mask <<= 1;
        return bit;
    }
};

} // namespace

std::vector<uint8_t> lzss_expand(const uint8_t* input,
                                 std::size_t src_size,
                                 std::size_t uncomp_size) {
    if (uncomp_size == 0) return {};

    std::vector<uint8_t> out;
    out.reserve(uncomp_size);
    std::vector<uint8_t> window(LZSS_WINDOW_SIZE, 0);
    int current_position = 1;

    const uint8_t* end = input + src_size;
    BitReader br(input, end);

    std::size_t produced = 0;
    while (produced < uncomp_size) {
        bool reloaded = false;
        int bit = br.next_bit(reloaded);

        if (bit) {
            // Literal: one byte copied directly.
            if (br.cur >= end) throw std::runtime_error("LZSS: truncated literal");
            uint8_t c = *br.cur++;
            out.push_back(c);
            ++produced;
            window[current_position] = c;
            current_position = (current_position + 1) & (LZSS_WINDOW_SIZE - 1);
        } else {
            // Match: 2-byte token [len_hi:4|pos_hi:4] [pos_lo:8].
            if (br.cur + 1 >= end) throw std::runtime_error("LZSS: truncated match token");
            uint8_t byte_a = br.cur[0];
            uint8_t byte_b = br.cur[1];
            br.cur += 2;

            int match_position = (static_cast<int>(byte_a & 0x0F) << 8) | byte_b;
            int match_length = (byte_a >> 4) + LZSS_BREAK_EVEN;

            // Clamp the final match so we don't overrun the output.
            if (match_length > static_cast<int>(uncomp_size - produced)) {
                match_length = static_cast<int>(uncomp_size - produced);
            }

            for (int i = 0; i <= match_length; ++i) {
                uint8_t c = window[(match_position + i) & (LZSS_WINDOW_SIZE - 1)];
                out.push_back(c);
                ++produced;
                window[current_position] = c;
                current_position = (current_position + 1) & (LZSS_WINDOW_SIZE - 1);
            }
        }
    }

    return out;
}

} // namespace f4::world_convert
