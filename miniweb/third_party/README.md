# Vendored dependencies

- `quickjs/` — QuickJS-ng 0.18.0, MIT. Upstream: https://github.com/quickjs-ng/quickjs
  - Only `quickjs.c`, `dtoa.c`, `libregexp.c`, `libunicode.c` and their headers are compiled by this project.
- `stb_image.h`, `stb_image_write.h`, `stb_truetype.h` — stb, public domain / MIT. Upstream: https://github.com/nothings/stb
- `nanosvg.h`, `nanosvgrast.h` — nanosvg, zlib license. Upstream: https://github.com/memononen/nanosvg

The React/Vue bundles under `../test/vendor/` are for compatibility tests only and are not part of the engine; they remain under their respective MIT licenses.
