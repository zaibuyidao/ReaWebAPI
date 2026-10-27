# ReaWebAPI

- Fix `ReaWeb_Open` optional boolean binding for Lua `false/true` and EEL2 `0/1`, preserving omitted-argument and window reuse behavior on Windows, macOS and Linux.
- Add EEL2 WebView examples for request/reply messaging and bidirectional track volume, pan, color, mute and solo, with frame-channel state refresh, `0..4` volume gain, acknowledged and bounded slider writes, final-value protection and close-time cleanup.
- Include EEL2 examples and bilingual support documentation in the SDK and register the example launchers in ReaPack.
