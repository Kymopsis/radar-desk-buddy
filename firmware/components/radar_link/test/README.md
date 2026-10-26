# Host regression test

Run from this directory with a C11 compiler:

```powershell
gcc -std=c11 -Wall -Wextra -Werror -I../include ../radar_protocol.c test_radar_protocol.c -lm -o radar_protocol_test.exe
./radar_protocol_test.exe
```

The test composes synthetic TinyFrame packets and covers fragmented and concatenated input, header/payload checksum rejection plus recovery, an unknown zero-length type, 2D and 3D target decoding, 3D `0x0A08` cloud telemetry, nearest-of-multiple targets, and a no-target `0x0A04` after the firmware mode is known.
