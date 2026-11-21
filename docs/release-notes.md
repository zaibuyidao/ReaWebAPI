# ReaWebAPI

- Added stereo post-FX/pre-fader and post-fader track playback taps for native audio and analyzers. Playback was verified in REAPER 7.79–7.86 on macOS arm64 and Linux x86_64, and in 7.86 on Windows x64 with the shared cache reader. On all three platforms, startup detection checks the mixer code and track cache layout so compatible builds can use the taps without a per-release entry.
