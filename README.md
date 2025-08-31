# Moonlight Android

[Moonlight for Android](https://moonlight-stream.org) is an open source client for  [Sunshine](https://github.com/LizardByte/Sunshine).

This fork aims to build upon the original Android client with some new features and some existing features, fixes and optimizations from [Artemis](https://github.com/ClassicOldSong/moonlight-android) and other developers.


No release apks will be provided for the time being, so the target audience for this fork is other developers looking to incorporate added functionality into their own builds.

## New Features

### Per-app settings overrides

Each app can now have it's own resolution, FPS, bitrate and frame pacing settings:

[![IMAGE ALT TEXT HERE](https://img.youtube.com/vi/quPzQCCLMXA/0.jpg)](https://www.youtube.com/watch?v=quPzQCCLMXA)

### New gamepad button chord for quitting host app

To end the session AND quit the app on the host machine, use a new button chord (made with macros in mind):

```LB``` + ```RB``` + ```D-pad right``` + ```D-pad down```

## Features and Improvements Merged from Artemis

- Ultra low latency mode with Snapdragon (8 Gen 2+) latency improvements

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
