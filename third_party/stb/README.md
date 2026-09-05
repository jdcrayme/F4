# third_party/stb

Vendored headers from https://github.com/nothings/stb (public domain /
MIT dual license, see each header for the full text).

| Header | Version | Used by | Purpose |
|---|---|---|---|
| `stb_image_write.h` | 1.16 | f4-import (`texture_png.cpp`) | PNG encoding for KoreaObj texture export |

Update policy: replace the header wholesale with the new upstream
release; the implementation TU is `f4-import/src/stb_image_write_impl.cpp`.
