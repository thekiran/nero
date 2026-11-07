# Terminal demonstration

The images and animation were rendered from a real PTY session running the
native macOS ARM64 `nero` executable. They are terminal replays, not desktop
screen captures. Only `help`, `version`, `history`, `history -clear`, `clear` and
`exit` were entered. The process exited with status 0.

- `nero-demo.cast`: original timestamped output, asciicast v2 (110 columns × 34 rows).
- `nero-demo.gif`: 23.125-second terminal replay, 1440 × 900, 8 fps.
- `nero-demo.mov`: Motion JPEG QuickTime version, 1152 × 720, 8 fps.
- `nero-startup.png`, `nero-help.png`, `nero-history.png`: terminal frames.

The frame renderer preserves the terminal's displayed text and ANSI colors in a
presentation window. It does not fabricate command output. The video is silent.

## Reproduce on macOS

Build the native executable with `make`. With Pillow installed for Python 3:

```sh
python3 scripts/demo/record_terminal.py
python3 scripts/demo/encode_video.py
```

Intermediate PNG frames are written to `.build/demo-frames`. The replay does not
demonstrate scanner behavior or raw packet engine operation.
