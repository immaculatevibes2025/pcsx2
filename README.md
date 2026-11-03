# PS2 Pro Logic II Surround Test

Test program that plays sounds matrix-encoded the way PS2 games encode Dolby Pro Logic II, so you can check that a decoder (PCSX2's expansion modes or a receiver) puts each one in the right speaker.

## Running

- **PCSX2:** System → Start File… → `surround-test.elf`. Set the expansion mode you want to test (or Disabled to let your receiver decode).
- **Real PS2:** run `surround-test.elf` from uLaunchELF (or any ELF loader) with the PS2 connected to a Pro Logic II receiver.

## Controls

| Button | Action |
|---|---|
| D-pad | Select test |
| X | Play selected test (repeats until stopped) |
| START | Auto-cycle through every test |
| O | Stop |

Each test speaks its name from the position being tested, then plays pink noise bursts there. The room diagram shows where the sound should be.

## Tests

1. Front Left, Center, Front Right, Surround Right, Surround Left, Both Surrounds
2. Phantom images between Front Left/Center and Center/Front Right
3. "Game style" surround mixes (Lt:Rt of 1:-0.5 and 1:-0.3, left and right) that PS2 games use; a receiver keeps these in the surround
4. Star Ocean 3 surround positions, measured from the game's own surround test (Lt:Rt -0.45:0.90 and -0.87:0.50)
5. Circle sweep: one lap around the room every 12 seconds, should move smoothly with no jumps
5. Subwoofer: 40 Hz tone (subwoofer in PCSX2 Game mode or with receiver bass management)
6. Dialogue + Surround Left effect: two sources at once, the hardest case for a matrix decoder
7. Fast sweep: one lap every 1.5 seconds, checks steering keeps up
8. Quiet levels: Surround Left at 0, -20 and -40 dB, should stay in the surround at every level
9. Ambience: uncorrelated noise, should spread evenly and not lock onto one speaker

## Encoding

Every position is an angle on the Lt/Rt plane: Lt = cos θ, Rt = sin θ. Surround positions use a negative gain on one side, which is how PS2 games use phase-inverted SPU2 voice volumes.
