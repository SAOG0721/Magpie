## Duplicate-monitoring

When displaying performance monitor like RTSS (Rivatuner Statistics Server), there might be 2 OSD layers displayed with Magpie scaling. This is caused by d3d, the screen capture method since v0.7.0. It will be captured by RTSS as well. You can fix this issue by adding it to the blacklist.

## The hot keys don't work, but "Scale after `x`s" works.

1. Try changing the hot keys.
2. Try running Magpie as Administrator.

## Lagging/latency

Please check the [Performance optimization](https://github.com/Blinue/Magpie/wiki/Performance%20optimization) page.

## Will using Magpie in multi-player games be detected as cheating?

Magpie is designed to be non-intrusive, so it is unlikely to be detected as a cheating tool. There have been no reports of bans for using Magpie. Nevertheless, you should be aware of the risks of using the software.

## What is "3D game mode"?

Enabling this option can change the behavior of the cursor and in-game overlay to optimize for 3D games. It is recommended to turn on this option when scaling 3D games.

## What is "Disable DirectFlip"?

DirectFlip is a technology to reduce input lags, but it may cause trouble in some circumstances. Please turn on this option when you are in the following situations:

1. The game lags unexpectedly (game known with the issue: Days Gone).
2. Abnormal low frame rates.
3. The screen freezes when streaming.

## What is "Simulate Exclusive Fullscreen?"

A lot of software checks whether there are games running under dedicated fullscreen mode before popping up windows. Turn on this option if you'd like to play in fullscreen without being disturbed,

## What is the frame rate displayed in Magpie?

The frame rate displayed by Magpie is that of its own rather than that of the game. Due to the non-intrusive nature, Magpie has no way to detect the frame rates of games themselves. We recommend you to use tools like RTSS to display the games' frame rates. Their add-on layers can usually be captured by Magpie as well.

## Does Magpie support touch input?

Supported from v0.11, see [About touch support](About%20touch%20support.md) for details.

### Does Magpie support frame generation and AI effects?

Magpie Experimental supports **DLSS FG, XeSS FG/MFG, FSR3 FG, and NVIDIA VFG**; use one frame-generation implementation per effect group. Available backends, multipliers, and optical-flow options (AMDOF / NVOF) depend on your GPU, driver, and bundled runtimes.

This branch also provides **DLSS SR, FSR 2/3/4, and XeSS SR** temporal upscaling, **DLSSNR** AI image enhancement, **RTX Video** (VSR / Denoise / Deblur / TrueHDR), and **OKLab Image Adjustment** color grading. Because external window capture lacks native game motion vectors, depth buffers, and separated UI, results can differ from native in-game integration. See the [Built-in Effects Guide](Built-in%20effects.md#current-branch-setup) and [Frame Sync Guide](FRAME_SYNC_GUIDE.md) for setup details.

## What is the relationship between Magpie and Lossless Scaling?

[Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/) is a paid software on Steam that is similar to Magpie. When Magpie was released in February 2021, existing window scaling software (such as [IntegerScaler](https://tanalin.com/en/projects/integer-scaler/) and Lossless Scaling) only supported simple scaling algorithms like nearest-neighbor scaling and integer scaling. These programs relied on the [Magnification API](https://learn.microsoft.com/en-us/windows/win32/api/_magapi/) to function, which led to significant limitations.

To support advanced scaling algorithms, I developed and open-sourced Magpie, which implements scaling in a completely different way. Initially, Magpie only supported algorithms like Anime4K, Lanczos, and Adaptive Sharpen. However, based on user feedback, Magpie added support for FSR in [July 2021](https://github.com/Blinue/Magpie/commit/7f6c66f3b47ccd64da41d298faa7a8e185bd5299). Soon after, Lossless Scaling released version 1.4.0, which supported FSR in a similar way to Magpie.

Currently, Magpie and Lossless Scaling differ significantly. Magpie focuses on usability and supporting more scaling algorithms, while Lossless Scaling primarily aims to improve 3D game performance.
