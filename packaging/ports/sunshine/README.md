# Super Mario Sunshine

A native port of Super Mario Sunshine, built from the
[sms-english](https://github.com/chasem-dev/sms-english) decompilation (CC0) with the platform
code of [sms-pc-port](https://github.com/chasem-dev/sms-pc-port) (MIT). The game draws through
[Starlet](https://github.com/Producdevity/starlet) (GPL-3.0). This project is not affiliated with
Nintendo.

No game data is included. You need your own copy of Super Mario Sunshine.

## Installation

1. Dump your own Super Mario Sunshine disc (USA, game ID `GMSE01`) and keep it as `.iso`,
   `.gcm`, `.rvz` or `.wia`. Other formats (NKit, GCZ, CISO, WBFS) need converting first, for
   example with Dolphin.
2. Install the port, then copy the disc image into `sunshine/gamefiles/`. The `sunshine` folder
   sits in your ports folder, next to the `Super Mario Sunshine.sh` launcher. On muOS it is
   `ports/sunshine` at the top of the card, while the launcher is in `ROMS/Ports`, and with two
   cards PortMaster uses the second one. Any filename works.
3. Launch Super Mario Sunshine from the Ports list. The first launch extracts the game files on
   the device, which takes a few minutes and needs about 1.1 GB of free space. You can delete the
   disc image afterwards.

## Saves

The memory card is the folder `sunshine/saves/USA/Card A`, laid out like Dolphin's GCI folder:
each save file is one `.gci` file. Back it up by copying the folder. To move saves between
Dolphin and the port, copy the `.gci` files between Dolphin's `GC/USA/Card A` folder and this one.

## Troubleshooting

`sunshine/log.txt` describes the last launch, including the reason when the game stops;
`sunshine/setup.log` describes the first-launch setup. Include them when you report a problem.

## Controls

Buttons follow the Nintendo layout: A is the right face button, B the
bottom, X the top and Y the left.

| Button | GameCube control |
|--|--|
| A, B, X, Y | A, B, X, Y (Nintendo layout) |
| L1 / R1 | L / R |
| L2 / R2 | Z |
| Left stick | Control stick |
| Right stick | C stick |
| D-pad | D-pad |
| Start | Start |
| Select + Start | Exit and save the memory card |

## Notes

- Requires OpenGL ES 3.1 and glibc 2.30 or newer.

## Thanks

- [sms-english](https://github.com/chasem-dev/sms-english), the Super Mario Sunshine decompilation (CC0).
- [sms-pc-port](https://github.com/chasem-dev/sms-pc-port): platform code (MIT).
- Starlet (GPL-3.0). RVZ decompression: zstd (BSD).
