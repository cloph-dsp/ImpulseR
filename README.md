# ImpulseR

Android app for acoustic impulse response (IR) capture using exponential sine sweeps (ESS).

Captures room acoustics, speaker/headphone response, or any linear system response via the device microphone.

## Build

**Prerequisites**: JDK 17+, Android SDK 34, NDK 25.2.9519653, CMake 3.22.1

```bash
# Local build (uses android-sdk/ and java/ from repo root)
./gradlew assembleDebug
```

**GitHub Actions**: push a `v*` tag to trigger release build:

```bash
git tag v1.0.0
git push origin v1.0.0
```

## How it works

1. **Arm** — prepares microphone and speaker for measurement
2. **Calibrate** — measures the phone's own speaker→mic transfer function (optional, improves accuracy)
3. **Sweep** — plays an ESS chirp through the speaker, records via microphone
4. **Deconvolve** — extracts the impulse response from the recording
5. **Review & Export** — trim the IR and export as .wav

## Calibration

The calibration filter compensates for the phone's built-in speaker and microphone coloration.
Run **CALIBRATE** before your first measurement to precompute an inverse filter.

## License

MIT
