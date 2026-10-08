# Getting the PSS-270 voices

The 100 PSS-270 voices are Yamaha's work, so they are not part of this repo. `firmware/mega/pss270_mega/pss270_tones.h` is a **placeholder**: all 100 entries are the same plain tone. The firmware, the menu, the arpeggiator and the tone editor all work with it. You just do not get the real voices until you fill in the table.

## What goes in the table

One entry per voice 00-99:

```
/*00*/ {{b0,b1,b2,b3,b4,b5,b6,b7}, flags, builtin},   // name
```

- `b0..b7` are the 8 bytes the PSS CPU writes to the YM2413's user-tone registers `0x00-0x07` when that voice is selected.
- `flags` marks what the original CPU adds on top of the tone (`F_PARTIAL`, `F_LAYER`, `F_BUILTIN`, `F_VIB`, `F_PITCHEG`, `F_RETRIG`). They are only information for now. The Mega does not act on them.
- `builtin` is the chip-ROM instrument number used for the second layer of some voices.

## Ways to get the bytes from your own keyboard

1. **Sniff them.** In stock mode the Mega can watch the chip's bus. Run the console command `sniff`, then select a voice on the keyboard within 10 seconds. The user-tone writes show up as address `0x00`-`0x07` followed by a data byte. It records the last 160 writes and can be noisy, so check a voice over two or three runs.
2. **Look for published rips** of the PSS-270 patches (for example plgDavid's OPLL patch rips on GitHub), mind their licence, and convert them to the layout above.

Once a voice is in the table you can still tweak it live with the tone editor: **Load voice**, change it, **Save to Mega**.

## Voice names

`pss270_voices.h` has the 100 voice names from the service manual (they are just labels), plus a flag for the voices that use two chip channels.
