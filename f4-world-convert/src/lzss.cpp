// f4-world-convert/src/lzss.cpp — thin adapter delegating to f4::lzss::decompress().
//
// Previously this was a duplicate of f4-lzss/src/lzss.cpp. Now it simply
// forwards to the canonical implementation and translates the error
// convention (f4::lzss returns empty on failure; lzss_expand throws).

#include <f4/world_convert/lzss.hpp>
#include <f4/lzss/lzss.hpp>

#include <stdexcept>

namespace f4::world_convert {

std::vector<uint8_t> lzss_expand(const uint8_t* input,
                                 std::size_t src_size,
                                 std::size_t uncomp_size) {
    if (uncomp_size == 0) return {};

    auto result = f4::lzss::decompress(input, src_size, uncomp_size);

    // f4::lzss::decompress returns an empty vector on malformed/truncated
    // input. The original lzss_expand threw std::runtime_error in that
    // case, so we preserve that contract.
    if (result.empty() && uncomp_size > 0) {
        throw std::runtime_error("LZSS: decompression failed (truncated or malformed input)");
    }

    return result;
}

} // namespace f4::world_convert
