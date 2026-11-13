# clean-cam

A denoised webcam for MacBooks running [Asahi Linux](https://asahilinux.org).

On Linux the built-in FaceTime HD camera is much grainier than on macOS, because macOS
cleans each frame up in software and Linux doesn't. `clean-cam` adds a second camera,
**"FaceTime HD (Clean)"**, with motion-adaptive temporal noise reduction: still parts of the
picture are averaged over several frames, moving parts pass through so they don't smear.

- **Only runs when used.** The real camera stays off until an app starts streaming from the clean
  one, and turns off 3 s after the last app stops. Idle cost: 0% CPU, ~5 MB RAM.
- **Small and fast.** One C file, no dependencies. 1080p30 uses about 11% of one core on an M1 Pro
  (the filter itself is under 1 ms per frame; ffmpeg's `hqdn3d` needs ~13 ms).

## Install

```bash
curl -fsSL https://raw.githubusercontent.com/maxberggren/clean-cam/main/install.sh | bash
```

Then pick **FaceTime HD (Clean)** as the camera in your browser or video app.

The installer (Arch-based systems) installs `v4l2loopback-dkms` and your kernel's headers
(plus clang if your kernel was built with it, as Asahi's is), sets up `/dev/video42`,
builds `clean-cam` into `~/.local/bin` and enables a systemd user service.

Uninstall: `~/.local/src/clean-cam/uninstall.sh`

## Tuning

Edit `ExecStart` in `~/.config/systemd/user/clean-cam.service`, then
`systemctl --user daemon-reload && systemctl --user restart clean-cam`.

| Option | Default | |
|---|---|---|
| `-n N` | `5` | Strength 0–10 (0 = off) |
| `-t Y,UV` | `20,24` | Motion threshold in levels. Higher = smoother, but ghost trails when you move |
| `-s WxH` | `1920x1080` | Also `1280x720`, `640x480`, … |
| `-f FPS` | `30` | |
| `-d` | off | Let the camera drop its frame rate in low light (longer exposure, less noise) |
| `-g MS` | `3000` | How long the camera stays on after the last app stops |
| `-i DEV` / `-o DEV` | auto / `/dev/video42` | Source camera / loopback device |

`clean-cam -b` benchmarks the filter on synthetic noise.

## Notes

- Only one app can stream from the clean camera at a time.
- If an app is using the raw camera, the clean one can't start; it retries every 2 s.
- Needs v4l2loopback ≥ 0.13 (for its client-usage event).
- Works with any NV12 V4L2 camera via `-i`, but it's only been tested on an M1 Pro MacBook Pro.

## License

MIT
