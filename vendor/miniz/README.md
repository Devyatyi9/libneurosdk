# miniz provenance

This directory vendors the codec-only portion of miniz for the internal
permessage-deflate implementation.

- Upstream: https://github.com/richgel999/miniz
- Tag: `3.1.2`
- Commit: `77d0dce8627735138c51770d1799a1ef48f2117d`
- License: MIT (`LICENSE`)

Copied upstream files, byte-for-byte:

- `miniz.c`
- `miniz.h`
- `miniz_common.h`
- `miniz_tdef.c`
- `miniz_tdef.h`
- `miniz_tinfl.c`
- `miniz_tinfl.h`
- `LICENSE`

`miniz_export.h` is a local static-build shim. ZIP/archive files, examples,
tests, tools, and build-system files are intentionally excluded. In
particular, no `miniz_zip.c` or `miniz_zip.h` is vendored.
