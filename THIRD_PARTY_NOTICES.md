# Third-party notices

## sms-english decompilation

The `third_party/sms` dependency is the Super Mario Sunshine decompilation (USA, GMSE01):
https://github.com/chasem-dev/sms-english. It is dedicated under CC0 1.0 Universal; the
dedication is retained in `third_party/sms/LICENSE`. The exact revision is
`967436b8c623834d38a57d34923775f45eca8861`, recorded in `third_party/sms.lock.json` and the
submodule. The port's changes to it are the patches in `patches/sms`, applied to a generated
copy at build time. The PortMaster package ships the dedication as
`licenses/LICENSE.sms-english.txt`.

Game data is user supplied and is not covered by the decompilation's dedication.

## sms-pc-port

The `third_party/sms-pc-port` dependency is https://github.com/chasem-dev/sms-pc-port, licensed
under the MIT License. The PortMaster package contains code compiled from it and ships the
notice below as `licenses/LICENSE.sms-pc-port.txt`.

The port uses:

- its decompilation patches listed as `sms-pc-port/...` in `patches/sms/series`, applied to the
  generated copy of the decompilation;
- `platform/endian` (the endian dispatch and the per-format converters) and
  `platform/misc/endian.cpp`, `msl_math.c` and `msl_rand.c`;
- `platform/mtx` (the matrix routines);
- `platform/audio/dsp_hle.cpp` and `dsp_mixer.cpp` (the DSP emulation and mixer);
- `src/port_compat.h` and the headers in `src/port_include`.

```
MIT License

Copyright (c) 2026 SoftwareAura LLC

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Starlet

The game draws through Starlet, the GameCube and Wii compatibility layer in
`third_party/starlet` (or `SMS_STARLET_DIR`), and `sunshine-setup` uses its disc module.
Starlet is licensed under the GNU General Public License, version 3. The PortMaster package
ships its licence as `licenses/LICENSE.starlet.txt`.

## Zstandard

`sunshine-setup` links Starlet's disc module, which links the Zstandard decompressor
(https://github.com/facebook/zstd) from Starlet's pinned `third_party/zstd` submodule, under its
BSD license in `third_party/zstd/LICENSE` of the Starlet tree. The PortMaster package ships it
as `licenses/LICENSE.zstd.txt`.

## System libraries

SDL2, OpenGL ES and the C/C++ runtime come from the device and are not shipped.
