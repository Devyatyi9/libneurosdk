# miniz provenance

This directory vendors the codec-only portion of miniz for the internal
permessage-deflate implementation.

- Upstream: https://github.com/richgel999/miniz
- Tag: `3.1.2`
- Commit: `77d0dce8627735138c51770d1799a1ef48f2117d`
- License: MIT (`LICENSE`)

Vendored upstream files:

- `miniz.c`
- `miniz.h`
- `miniz_common.h`
- `miniz_tdef.c`
- `miniz_tdef.h`
- `miniz_tinfl.c`
- `miniz_tinfl.h`
- `LICENSE`

All listed files except `miniz_tdef.c/.h` are copied byte-for-byte.
`miniz_export.h` is a local static-build shim. `miniz_tdef.c/.h` add a runtime
maximum match distance used to implement RFC 7692 negotiated window limits.
ZIP/archive files, examples, tests, tools, and build-system files are
intentionally excluded. In particular, no `miniz_zip.c` or `miniz_zip.h` is
vendored.
