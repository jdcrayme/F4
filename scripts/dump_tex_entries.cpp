// dump_tex_entries.cpp
// Print all TexBankEntry fields for the first N textures.

#include <f4/models/model_database.hpp>
#include <cstdio>
#include <filesystem>

int main(int argc, char* argv[]) {
    std::filesystem::path hdr_path = "../temp/KoreaObj.HDR";
    if (argc >= 2) hdr_path = argv[1];

    f4::models::ModelDatabase db;
    std::string err = db.load_hdr(hdr_path);
    if (!err.empty()) { printf("ERROR: %s\n", err.c_str()); return 1; }

    const auto& te = db.tex_entries();
    int n = std::min(20, (int)te.size());
    printf("%-4s %10s %10s %8s %6s %10s %10s\n",
           "idx", "offset", "size", "dim", "pal", "chroma", "flags");
    for (int i = 0; i < n; ++i) {
        const auto& e = te[i];
        printf("%-4d %10u %10u %8u %6d 0x%08X 0x%08X\n",
               i, e.file_offset, e.file_size, e.dimension,
               e.palette_id, e.chroma_key, e.flags);
    }
    return 0;
}
