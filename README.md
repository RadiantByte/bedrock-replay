# BedrockReplay

## Introduction

BedrockReplay is an open-source native replay mod for Minecraft Bedrock on Android, made for [LeviLauncher](https://github.com/LiteLDev/LeviLauncher-Android). It allows players to record gameplay, watch replays, create cinematic camera paths, and export videos directly from Minecraft.

## Features

- Client-side gameplay recording and playback
- Replay management and importing
- Free camera and player POV switching
- Timeline controls and replay seeking
- Advanced replay editor with cinematic camera paths
- Customizable camera movement and settings
- Direct MP4 video export with landscape and portrait presets
- Native C++20 implementation built for LeviLauncher and Preloader

## System Requirements

- 64-bit ARM Android device (`arm64-v8a`)
- [LeviLauncher](https://github.com/LiteLDev/LeviLauncher-Android)
- A Minecraft Bedrock version supported by the BedrockReplay release you are using

## Installation

1. Install LeviLauncher.
2. Download the latest `BedrockReplay.levipack` release.
3. Import the package from LeviLauncher's mod manager and enable it.
4. Launch Minecraft through LeviLauncher.

## Commands

| Command | Description |
|---|---|
| `/record start` | Start or resume recording |
| `/record pause` | Pause recording |
| `/record stop` | Stop and save the recording |

## Development Setup

Requirements:

- Android NDK r28c
- xmake
- Python 3

Build for Android ARM64:

```sh
xmake f -y -p android -a arm64-v8a -m release --ndk=/path/to/android-ndk-r28c
xmake -y
```

The release build produces `libBedrockReplay.so` and `BedrockReplay.levipack` in the xmake target directory.

The replay implementation is located under `src/replay`, while native hooks, runtime code, signatures, and Minecraft SDK headers are maintained separately. Signatures and offsets are version-specific and may require updating for newer Minecraft versions.

## Contributing

Pull requests are highly appreciated. Keep changes focused and test them against the intended Minecraft version before submitting.

## Contact

Discord: [discord.gg/rMgdpTFFVg](https://discord.gg/rMgdpTFFVg)

**Report Issues:** Open an issue in this GitHub repository.