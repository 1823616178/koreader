# Vendored dependencies

These are checked in (rather than fetched at build time) because the
Kindle cross-build has no package manager on the target, and the build
must stay reproducible offline.

- `cJSON/` -- cJSON 1.7.19 (`cJSON.h`: `CJSON_VERSION_MAJOR/MINOR/PATCH`).
  Source plus header; built from source by the Makefile.
- `fbink/` -- FBInk, master commit `886f25f1` (see the build steps in
  `kindle/README.md`: release v1.25.0 predates `fbink_fill_rect_gray()`).
  Only `fbink.h` and the prebuilt static `libfbink.a` are checked in;
  `tools/build.sh` verifies the final binary is ARM and does *not*
  dynamically link libfbink.

To upgrade either one: replace the files, update the version/commit
above, rebuild, and re-run `make test` plus the device checklist in
`kindle/README.md` -- the FBInk ABI is tied to the Kindle firmware's
framebuffer behaviour, so a version bump is a device-tested change,
not a drop-in.
