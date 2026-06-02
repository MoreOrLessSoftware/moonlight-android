# Moonlight Android

[Moonlight for Android](https://moonlight-stream.org) is an open source client for  [Sunshine](https://github.com/LizardByte/Sunshine).

This fork aims to build upon the original Android client with some new features and some existing features, fixes and optimizations from [Artemis](https://github.com/ClassicOldSong/moonlight-android) and other developers.

## New Features

### 1. Quick Launch Apps

You can create shortcuts to specific Sunshine apps and apply custom streaming settings (like resolution, FPS, bitrate, etc) to each shortcut. The same app can be configured to multiple shortcuts:

[![Quick Launch Apps](https://storage.googleapis.com/moreorlesscorrect/moonlight-x/screenshots/quick-launch.png)](https://youtu.be/3kc8VEu1x7k)

> [!NOTE]
> Quick Launch apps can also be added as shortcuts in Android launchers, etc.

### 2. Per-app settings overrides

Each app can now have it's own settings for resolution, FPS, bitrate, frame pacing, etc:

[![Per-app settings](https://storage.googleapis.com/moreorlesscorrect/moonlight-x/screenshots/per-app-settings.png)](https://youtu.be/AYPQj0LOsxk)

### 3. Brightness adjustment while streaming

Tap the left edge of the screen to show a brightness slider and set a custom brightness while streaming (or slide it to the bottom for "Auto" which will use the system brightness):

[![Brightness adjustment](https://storage.googleapis.com/moreorlesscorrect/moonlight-x/screenshots/brightness-slider.png)](https://youtu.be/CnanqtRz0FI)

### 4. Overrides for bitrate and performance stats

These overrides will supersede any per-app or Quick Launch settings when enabled:

![Quick toggle for performance overlay + bitrate override](https://storage.googleapis.com/moreorlesscorrect/moonlight-x/screenshots/overrides4.png)

### 5. Ultra low latency flags for Exynos and Amlogic decoders

Added some additional decoder flags/options to potentially reduce latency on devices with Exynos (tested on Google Pixel 7a), and Amlogic (tested on Chromecast w/Google TV) SoCs.

Recommend **Balanced** frame pacing and the **Ultra low latency** option enabled for these devices.

### 6. Force HDR (10-bit SDR) streaming on devices that don't support HDR (Experimental)

Some non-HDR devices can still stream 10-bit SDR if Sunshine sends it. This simple hack bypasses the HEVC decoder check for HDR10 when you have the "Enable HDR" setting activated.

### 7. In-Stream Overlay Menu

Access quick actions and custom commands during streaming with a customizable overlay menu:

**Features:**
- **Quick Actions**: Disconnect, quit session, toggle stats, mouse mode, and show keyboard (except Google TV)
- **Custom Commands**: Create custom keyboard shortcuts
- **Multiple Long-Press Trigger Options**:
  - Select button (default)
  - Start button
  - Guide button (not be supported on all devices)
  - LB + RB combination
  - Back button on remote (Shield, CCwGTV, etc - always available)
- **Customizable Hold Duration**: Choose how long to hold (0.5s to 3s)
- **Touch Gesture**: 3-finger tap also opens the menu

**Note:** Start button long-press for mouse emulation and 3-button keyboard shortcut have been removed in favor of the quick actions in the overlay menu.

### 8. Improved Stats Overlay

- Tweaked the formatting and simplified labels
- Added variance between incoming and rendered FPS (will show > 0% when the values differ by 1 or more FPS)

## Screenshots

<img width="2560" height="1600" alt="Screenshot_20260602-085353" src="https://github.com/user-attachments/assets/2da16b54-2685-4c06-a855-76a38de5255f" />

## Features and Improvements Merged from Artemis

- Ultra low latency mode with Snapdragon (8 Gen 2+) and MediaTek (MTK) latency improvements
- Revised decode latency calculation (typically a few ms less than Moonlight reports)

## Downloads
[Download APK from releases](https://github.com/MoreOrLessSoftware/moonlight-android/releases)

## Building
* Install Android Studio and the Android NDK
* Run ‘git submodule update --init --recursive’ from within moonlight-android/
* In moonlight-android/, create a file called ‘local.properties’. Add an ‘ndk.dir=’ property to the local.properties file and set it equal to your NDK directory.
* Build the APK using Android Studio or gradle

## Credits
Moonlight X is a fork of the original [Moonlight Android](https://github.com/moonlight-stream/moonlight-android) which is authored by [Cameron Gutman](https://github.com/cgutman) and others.
