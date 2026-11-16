# Super Mario Sunshine Linux ARM

A native Linux AArch64 port of Super Mario Sunshine for handhelds that run
[PortMaster](https://portmaster.games). The game is built from
[sms-english](https://github.com/chasem-dev/sms-english), the decompilation of the GameCube game,
with the platform code of [sms-pc-port](https://github.com/chasem-dev/sms-pc-port) (MIT). It
draws with [Starlet](https://github.com/Producdevity/starlet), a GameCube and Wii compatibility
layer. This project is not affiliated with Nintendo.

No game data is distributed. You need your own copy of Super Mario Sunshine (USA, game ID
GMSE01) as an ISO, GCM, RVZ or WIA image. The PortMaster package and its player notes are in
[packaging/ports/sunshine](packaging/ports/sunshine/README.md).

## Source layout

- `third_party/sms`: the pinned decompilation, never edited (`third_party/sms.lock.json`).
- `third_party/sms-pc-port`: sms-pc-port, pinned; its platform code is compiled in and its
  decompilation patches are applied.
- `third_party/starlet`: Starlet, pinned to the revision the release was built with.
- `patches/sms`: the patch series (`series` lists sms-pc-port's patches, then this port's own),
  applied to a generated copy of the decompilation at build time by `tools/prepare_sms.py`.
- `src/platform`: console services the SDK calls land on, besides Starlet's modules.
- `src/graphics`: GX support code.
- `src/compat`: SDK structure layout and compatibility headers.
- `src/setup`: `sunshine-setup`, which extracts the disc image on the device with Starlet's
  disc module.
- `src/application`: the executable.
- `packaging/ports/sunshine`, `tools/package-portmaster.sh`: the PortMaster package.

## Build

Requirements: CMake 3.24+, Ninja, Python 3.9+, Clang and LLD, and an aarch64 sysroot with SDL2
and GLES 3.1. Clone with the submodules:

```sh
git clone --recurse-submodules <repository URL>
```

Handheld build:

```sh
cmake -S . -B build/aarch64 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux.cmake -DSMS_SYSROOT=SYSROOT \
    -DSMS_CLANG=clang -DSMS_CLANGXX=clang++ -DSMS_LLD=ld.lld -DSMS_GLES=ON
cmake --build build/aarch64 --target sunshine sunshine-setup
```

The build renames the game's global `operator new` and `delete` with `llvm-objcopy`. When it is not
on `PATH` (Homebrew's LLVM on a Mac), set `LLVM_BIN` to its directory before configuring.

`-DSMS_STARLET_DIR=DIR` builds against another Starlet checkout, and `-DSMS_DECOMP_DIR=DIR`
against a decompilation working tree instead of the pinned one.

Host tests of the port's own code (Clang or GCC on the build machine):

```sh
cmake -S tests -B build/host-tests && cmake --build build/host-tests && ctest --test-dir build/host-tests
```

### Profile-guided optimization

Build an instrumented binary with `-DSMS_PROFILE_GENERATE=ON
-DSMS_PROFILE_RUNTIME=.../libclang_rt.profile-aarch64.a`, run it on a device with
`LLVM_PROFILE_FILE=/tmp/NAME.profraw`, merge the `.profraw` files with `llvm-profdata merge -o
FILE.profdata`, then build the release with `-DSMS_PROFILE_USE=FILE.profdata`. The instrumented
binary writes its profile from its exit paths (`SMS_RUN_SECONDS` or a frontend quit). The
profile stays out of the repository; a profile from other sources only matches loosely.

## Package

The packager needs `screenshot.jpg` and `cover.jpg` (640x480) in `packaging/ports/sunshine`.
They are device captures and stay out of the repository.

```sh
tools/package-portmaster.sh build/aarch64 dist
```

It writes the port folder `dist/sunshine` and `dist/sunshine.zip`. The package ships
`packaging/ports/sunshine/sunshine/programs.list`, the merged Starlet program lists recorded on devices; set
`PROGRAM_LIST=FILE` to ship another one. Builds configured with
`-DSMS_DEV_TOOLS=ON` are refused.

`tools/assets/GMSE01-gamedata.sha1` lists the SHA-1 of every file `sunshine-setup` extracts. The
package ships it as `sunshine-gamedata.sha1`, and setup rejects any file that differs.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## Licenses

This project is licensed under the GNU General Public License v3.0; see
[LICENSE](LICENSE). The decompilation is CC0-1.0, sms-pc-port is MIT, Starlet is GPL-3.0, and
the Zstandard decoder Starlet links is BSD-licensed; see
[third-party notices](THIRD_PARTY_NOTICES.md).
