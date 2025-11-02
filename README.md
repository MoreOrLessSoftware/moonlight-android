# Moonlight Android

[Moonlight for Android](https://moonlight-stream.org) is an open source client for  [Sunshine](https://github.com/LizardByte/Sunshine).

This fork aims to build upon the original Android client with some new features and some existing features, fixes and optimizations from [Artemis](https://github.com/ClassicOldSong/moonlight-android) and other developers.

## New Features

### 1. Quick Launch Apps

You can create shortcuts to specific Sunshine apps and apply custom streaming settings (like resolution, FPS, bitrate, etc) to each shortcut. The same app can be configured to multiple shortcuts:

[![IMAGE ALT TEXT HERE](https://img.youtube.com/vi/3kc8VEu1x7k/0.jpg)](https://youtu.be/3kc8VEu1x7k)

> [!NOTE]
> Quick Launch apps is still a work in progress. On some devices the list of Quick Launch apps might not refresh after adding or removing one until you re-open the app.

### 2. Per-app settings overrides

Each app can now have it's own settings for resolution, FPS, bitrate, frame pacing, etc:

[![IMAGE ALT TEXT HERE](https://img.youtube.com/vi/AYPQj0LOsxk/0.jpg)](https://youtu.be/AYPQj0LOsxk)

### 3. New gamepad button chord for quitting host app

To end the session AND quit the app on the host machine, use a new button chord (made with macros in mind):

```LB``` + ```RB``` + ```D-pad right``` + ```D-pad down```

### 4. Ultra low latency flags for Exynos

Tried some [new flags for the Exynos decoder](https://github.com/MoreOrLessSoftware/moonlight-android/commit/77cd72c427dff7250b8e10b007d1ece2db9f7ddb):

```java
videoFormat.setInteger("vendor.rtc-ext-dec-output-queue-depth.value", 2); // Minimal queue depth for lower latency
videoFormat.setInteger("vendor.sec-dec-output.delay", 0); // Minimal output delay
```

These seem to reduce latency by a couple ms on my Pixel 7a and also actual rendering latency by 1 or 2 frames in my tests. To use, enable the "Ultra Low Latency" setting and I would suggest Balanced frame pacing.

## Features and Improvements Merged from Artemis

- Ultra low latency mode with Snapdragon (8 Gen 2+) and MediaTek (MTK) latency improvements

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
