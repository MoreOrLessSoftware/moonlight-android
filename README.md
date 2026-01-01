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

### 5. New gamepad button chord for quitting host app

To end the session AND quit the app on the host machine, use a new button chord (made with macros in mind):

```LB``` + ```RB``` + ```D-pad right``` + ```D-pad down```

### 6. Ultra low latency flags for Exynos

Tried some [new flags for the Exynos decoder](https://github.com/MoreOrLessSoftware/moonlight-android/commit/77cd72c427dff7250b8e10b007d1ece2db9f7ddb):

```java
videoFormat.setInteger("vendor.rtc-ext-dec-output-queue-depth.value", 2); // Minimal queue depth for lower latency
videoFormat.setInteger("vendor.sec-dec-output.delay", 0); // Minimal output delay
```

These seem to reduce latency by a couple ms on my Pixel 7a and also actual rendering latency by 1 or 2 frames in my tests. To use, enable the "Ultra Low Latency" setting and I would suggest Balanced frame pacing.

### 7. Force HDR (10-bit SDR) streaming on devices that don't support HDR (Experimental)

Some non-HDR devices can still stream 10-bit SDR if Sunshine sends it. This simple hack bypasses the HEVC decoder check for HDR10 when you have the "Enable HDR" setting activated.

## Features and Improvements Merged from Artemis

- Ultra low latency mode with Snapdragon (8 Gen 2+) and MediaTek (MTK) latency improvements

## Downloads
[Download APK directly](https://github.com/MoreOrLessSoftware/moonlight-android/releases)

## Building
* Install Android Studio and the Android NDK
* Run ‘git submodule update --init --recursive’ from within moonlight-android/
* In moonlight-android/, create a file called ‘local.properties’. Add an ‘ndk.dir=’ property to the local.properties file and set it equal to your NDK directory.
* Build the APK using Android Studio or gradle

## Authors

* [Cameron Gutman](https://github.com/cgutman)  
* [Diego Waxemberg](https://github.com/dwaxemberg)  
* [Aaron Neyer](https://github.com/Aaronneyer)  
* [Andrew Hennessy](https://github.com/yetanothername)

Moonlight is the work of students at [Case Western](http://case.edu) and was
started as a project at [MHacks](http://mhacks.org).
