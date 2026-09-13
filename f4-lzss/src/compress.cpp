// f4-lzss/src/compress.cpp
//
// LZSS compression — a faithful port of FreeFalcon's LZSS_Compress()
// (src/utils/lzss.cpp — the Nelson & Gailly "Data Compression Book"
// carman LZSS, buffer-I/O variant adapted for Falcon by Dave Lewak and
// Kevin Klemmick).
//
// This is the WRITE-side twin of lzss.cpp's LZSS_Expand port, and it is
// BYTE-IDENTICAL to FreeFalcon's compressor: given the same input it
// emits the same match choices (binary-tree descent with the book's
// `i >= match_length` tie rule), the same token stream, and the same
// trailing partial group. Verified byte-for-byte against every LZSS
// sub-file of both committed .cam fixtures:
//   save1.cam    .cmp 4412/4412  .uni 34154/34154  .obj 113753/113753
//   TestCamp.cam .cmp 4570/4570  .uni 155038/155038 .obd 114/114
//
// Algorithm (Nelson & Gailly, 2nd ed., chapter 8 — blocked I/O variant):
//   - 4096-byte ring window, current_position starts at 1; ring slot p
//     holds input byte p-1 (mod 4096). The window is seeded with the
//     first LOOK_AHEAD_SIZE = 17 input bytes and slides one byte per
//     consumed input byte (the new byte always lands at slot
//     current_position + 16).
//   - A binary tree of the 17-byte strings starting at each in-window
//     position orders match candidates. add_string() descends comparing
//     up to LOOK_AHEAD_SIZE bytes; `i >= match_length` (NOT >) means the
//     LAST equal-length candidate on the descent path wins; a full
//     17-byte match REPLACES the duplicate node (the tree holds unique
//     17-byte strings).
//   - Per consumed byte: delete_string(the slot about to be overwritten,
//     current_position + 17), write the new byte, current_position++,
//     add_string(current_position). The delete is a no-op until the ring
//     first wraps (positions ahead of the read head were never added).
//   - Token emission: literal (flag bit 1) when match_length <=
//     BREAK_EVEN (1); otherwise a pair (flag bit 0) with
//     b0 = ((length - 2) << 4) | (position >> 8), b1 = position & 0xFF.
//     Tokens blocked in (flag byte, 8 tokens) groups; a partial final
//     group is flushed once, at the end.
//
// Two FreeFalcon quirks are preserved because they are observable in the
// byte stream:
//   - The compressor reads one byte past the logical end on the final
//     advance steps (the value is discarded — never written to the
//     window). This port substitutes a 0 instead of touching out-of-
//     bounds memory; the emitted stream is identical.
//   - There is no "output grew past input" abort: FreeFalcon's version
//     stripped the carman overflow check (the empty-comment markers in
//     the upstream file), so incompressible inputs simply produce a
//     stream slightly larger than the input.

#include <f4/lzss/lzss.hpp>

#include <cstddef>
#include <cstring>
#include <vector>

namespace f4::lzss {

namespace {

// ── Codec constants (FreeFalcon utils/lzss.cpp) ───────────────────────────
constexpr int INDEX_BIT_COUNT      = 12;
constexpr int LENGTH_BIT_COUNT     = 4;
constexpr int WINDOW_SIZE          = 1 << INDEX_BIT_COUNT;               // 4096
constexpr int RAW_LOOK_AHEAD_SIZE  = 1 << LENGTH_BIT_COUNT;              // 16
constexpr int BREAK_EVEN           = (1 + INDEX_BIT_COUNT + LENGTH_BIT_COUNT) / 9;  // 1
constexpr int LOOK_AHEAD_SIZE      = RAW_LOOK_AHEAD_SIZE + BREAK_EVEN;   // 17
constexpr int TREE_ROOT            = WINDOW_SIZE;
constexpr int END_OF_STREAM        = 0;
constexpr int UNUSED               = 0;

inline int mod_window(int a) { return a & (WINDOW_SIZE - 1); }

struct TreeEntry {
    int parent;
    int smaller_child;
    int larger_child;
};

// The per-call compression context (FreeFalcon's LZSS_COMP_CTXT; the
// game made the book's globals stack-allocated for thread safety).
struct CompCtxt {
    unsigned char window[WINDOW_SIZE] = {};
    TreeEntry tree[WINDOW_SIZE + 1] = {};
    unsigned char data_buffer[17] = {};
    int flag_bit_mask = 0;
    unsigned int buffer_offset = 0;
    unsigned int old_buffer_offset = 0;
    int compressed_size = 0;
    int inc_output_string = 0;
};

void init_tree(int r, CompCtxt* ctxt) {
    for (int i = 0; i < (WINDOW_SIZE + 1); ++i) {
        ctxt->tree[i].parent = UNUSED;
        ctxt->tree[i].larger_child = UNUSED;
        ctxt->tree[i].smaller_child = UNUSED;
    }
    ctxt->tree[TREE_ROOT].larger_child = r;
    ctxt->tree[r].parent = TREE_ROOT;
    ctxt->tree[r].larger_child = UNUSED;
    ctxt->tree[r].smaller_child = UNUSED;
}

void contract_node(int old_node, int new_node, CompCtxt* ctxt) {
    ctxt->tree[new_node].parent = ctxt->tree[old_node].parent;
    if (ctxt->tree[ctxt->tree[old_node].parent].larger_child == old_node)
        ctxt->tree[ctxt->tree[old_node].parent].larger_child = new_node;
    else
        ctxt->tree[ctxt->tree[old_node].parent].smaller_child = new_node;
    ctxt->tree[old_node].parent = UNUSED;
}

void replace_node(int old_node, int new_node, CompCtxt* ctxt) {
    const int parent = ctxt->tree[old_node].parent;
    if (ctxt->tree[parent].smaller_child == old_node)
        ctxt->tree[parent].smaller_child = new_node;
    else
        ctxt->tree[parent].larger_child = new_node;
    ctxt->tree[new_node] = ctxt->tree[old_node];
    ctxt->tree[ctxt->tree[new_node].smaller_child].parent = new_node;
    ctxt->tree[ctxt->tree[new_node].larger_child].parent = new_node;
    ctxt->tree[old_node].parent = UNUSED;
}

int find_next_node(int node, CompCtxt* ctxt) {
    int next = ctxt->tree[node].smaller_child;
    while (ctxt->tree[next].larger_child != UNUSED)
        next = ctxt->tree[next].larger_child;
    return next;
}

void delete_string(int p, CompCtxt* ctxt) {
    if (ctxt->tree[p].parent == UNUSED) return;
    if (ctxt->tree[p].larger_child == UNUSED) {
        contract_node(p, ctxt->tree[p].smaller_child, ctxt);
    } else if (ctxt->tree[p].smaller_child == UNUSED) {
        contract_node(p, ctxt->tree[p].larger_child, ctxt);
    } else {
        const int replacement = find_next_node(p, ctxt);
        delete_string(replacement, ctxt);
        replace_node(p, replacement, ctxt);
    }
}

// Add the string at new_node to the tree, returning the best match
// length against the nodes already present. The book's `i >=
// match_length` tie rule: the LAST equal-length candidate on the
// descent path wins. A full LOOK_AHEAD_SIZE match replaces the
// duplicate node and returns immediately.
int add_string(int new_node, int* match_position, CompCtxt* ctxt) {
    if (new_node == END_OF_STREAM) return 0;

    int test_node = ctxt->tree[TREE_ROOT].larger_child;
    int match_length = 0;

    for (;;) {
        int i = 0;
        int delta = 0;
        for (; i < LOOK_AHEAD_SIZE; ++i) {
            delta = ctxt->window[mod_window(new_node + i)] -
                    ctxt->window[mod_window(test_node + i)];
            if (delta != 0) break;
        }
        if (i >= match_length) {
            match_length = i;
            *match_position = test_node;
            if (match_length >= LOOK_AHEAD_SIZE) {
                replace_node(test_node, new_node, ctxt);
                return match_length;
            }
        }
        int* child = (delta >= 0) ? &ctxt->tree[test_node].larger_child
                                  : &ctxt->tree[test_node].smaller_child;
        if (*child == UNUSED) {
            *child = new_node;
            ctxt->tree[new_node].parent = test_node;
            ctxt->tree[new_node].larger_child = UNUSED;
            ctxt->tree[new_node].smaller_child = UNUSED;
            return match_length;
        }
        test_node = *child;
    }
}

// ── Blocked output (flag byte + 8 tokens per group) ───────────────────────

void init_output_buffer(CompCtxt* ctxt) {
    ctxt->data_buffer[0] = 0;
    ctxt->flag_bit_mask = 1;
    ctxt->old_buffer_offset = ctxt->buffer_offset;
    ctxt->buffer_offset = 1;
}

void flush_output_buffer(std::vector<uint8_t>& out, CompCtxt* ctxt) {
    if (ctxt->buffer_offset == 1) return;
    out.insert(out.end(), ctxt->data_buffer,
               ctxt->data_buffer + ctxt->buffer_offset);
    ctxt->compressed_size += static_cast<int>(ctxt->buffer_offset);
    init_output_buffer(ctxt);
}

void output_char(int data, std::vector<uint8_t>& out, CompCtxt* ctxt) {
    ctxt->data_buffer[ctxt->buffer_offset++] = static_cast<unsigned char>(data);
    ctxt->data_buffer[0] |= static_cast<unsigned char>(ctxt->flag_bit_mask);
    ctxt->flag_bit_mask <<= 1;
    ctxt->inc_output_string = 0;
    if (ctxt->flag_bit_mask == 0x100) {
        ctxt->inc_output_string = 1;
        flush_output_buffer(out, ctxt);
    }
}

void output_pair(int position, int length, std::vector<uint8_t>& out, CompCtxt* ctxt) {
    ctxt->data_buffer[ctxt->buffer_offset] =
        static_cast<unsigned char>(length << 4);
    ctxt->data_buffer[ctxt->buffer_offset++] |=
        static_cast<unsigned char>(position >> 8);
    ctxt->data_buffer[ctxt->buffer_offset++] =
        static_cast<unsigned char>(position & 0xff);
    ctxt->flag_bit_mask <<= 1;
    ctxt->inc_output_string = 0;
    if (ctxt->flag_bit_mask == 0x100) {
        ctxt->inc_output_string = 1;
        flush_output_buffer(out, ctxt);
    }
}

} // namespace

std::vector<uint8_t> compress(const uint8_t* src, std::size_t src_size) {
    std::vector<uint8_t> out;
    if (!src || src_size == 0) return out;

    // Worst case is all-literals: N data bytes + ceil(N/8) flag bytes.
    out.reserve(src_size + src_size / 8 + 16);

    CompCtxt ctxt;
    init_output_buffer(&ctxt);

    int current_position = 1;
    std::size_t in_pos = 0;          // next unread input byte
    int remaining = static_cast<int>(src_size);

    // Seed the window with the first LOOK_AHEAD_SIZE input bytes. The
    // game's loop writes window[current_position + i] without MOD_WINDOW
    // — safe because current_position is 1 here and i < 17.
    int i = 0;
    for (; i < LOOK_AHEAD_SIZE; ++i) {
        const unsigned char c = src[in_pos];
        ++in_pos;
        --remaining;
        if (remaining < 0) break;
        ctxt.window[current_position + i] = c;
    }
    int look_ahead_bytes = i;

    init_tree(current_position, &ctxt);
    int match_length = 0;
    int match_position = 0;

    while (look_ahead_bytes > 0) {
        if (match_length > look_ahead_bytes) match_length = look_ahead_bytes;

        int replace_count;
        if (match_length <= BREAK_EVEN) {
            replace_count = 1;
            output_char(ctxt.window[current_position], out, &ctxt);
        } else {
            output_pair(match_position, match_length - (BREAK_EVEN + 1),
                        out, &ctxt);
            replace_count = match_length;
        }

        for (i = 0; i < replace_count; ++i) {
            delete_string(mod_window(current_position + LOOK_AHEAD_SIZE), &ctxt);
            // The game reads *input_string unconditionally here — one
            // byte past the logical end on the final steps — and
            // discards it when size goes negative. Substitute a 0 for
            // that out-of-bounds read; the stream is unaffected.
            const unsigned char c =
                (in_pos < src_size) ? src[in_pos] : static_cast<unsigned char>(0);
            --remaining;
            if (remaining < 0) {
                --look_ahead_bytes;
            } else {
                ++in_pos;
                ctxt.window[mod_window(current_position + LOOK_AHEAD_SIZE)] = c;
            }
            current_position = mod_window(current_position + 1);
            if (look_ahead_bytes)
                match_length = add_string(current_position, &match_position, &ctxt);
        }
    }

    // If the last output_char/output_pair call completed a group, the
    // flush already happened inside it; only a partial final group
    // reaches this flush (FreeFalcon guards on inc_output_string; the
    // buffer_offset == 1 early-return inside flush is that guard).
    flush_output_buffer(out, &ctxt);
    return out;
}

std::vector<uint8_t> compress(const std::vector<uint8_t>& src) {
    return compress(src.data(), src.size());
}

} // namespace f4::lzss
