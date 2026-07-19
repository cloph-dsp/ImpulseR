# AGENTS.md

## Learned User Preferences

- User communicates in European Portuguese; reply in the same register when the user writes in Portuguese
- Prefers concise, code-first responses; no essays or unrequested explanations
- Approves work via question dialogs or direct "do X" — don't ask for sign-off on trivial steps
- Expects diff-first verification before commit: read every changed file, confirm wiring
- Respects Ponytail philosophy: minimum that works, lazy solutions, YAGNI, deletion over addition
- Likes parallel-batch dispatch over sequential edits (4 batches done this session)
- Catches agent bugs after delivery — agent claims must be re-verified
- Prefers pragmatic shortcuts marked with `// ponytail:` comments when complexity isn't justified
- Wants explicit commit messages that describe WHY, not just WHAT
- Bugfixes should be minimal — don't refactor while fixing

## Learned Workspace Facts

- Project: ImpulseR — Android IR-capture app, ESS chirps, Oboe + C++ DSP + JNI + Compose
- C++ DSP lives at `app/src/main/cpp/engine/` (NOT `app/src/main/cpp/`)
- JNI bridge: `app/src/main/cpp/jni/JniBridge.cpp`
- Kotlin: `app/src/main/kotlin/` (MainActivity, AudioCaptureForegroundService, engine/NativeEngine)
- minSdk 29, targetSdk 34
- ADB path: `C:\Users\Razer\adb\platform-tools\adb.exe` (not in PATH)
- Nothing phone connects via WiFi ADB: `192.168.1.68:38751` (see `C:\Users\Razer\adb\connect-nothing.bat`)
- NDK not installed locally — DSP code validated via `g++ -std=c++17 -fsyntax-only` with stubs
- No Kotlin LSP installed — can't run diagnostics on `.kt` files
- Build: `./gradlew installDebug` (or `gradlew.bat installDebug` on Windows)
- CalibrationFilter API: `getFilter()` returns `const float*` + separate `getFilterLength()` (NOT `vector<float>&`)
- Device ID for persistence: `ro.product.manufacturer + ":" + ro.product.model` (stable across OS updates)
- Calibration persists to `/data/data/com.impulser.capture/files/calibration_<deviceId>.bin` (auto-loaded in `nativeCreate()` since batch D)
- Sweep duration: 7s (unified across calibration and measurement)
- Filter length: 8192 taps
- ESS anti-aliasing: f2 clamped to `0.45 * sampleRate`
- Verification pattern after agent work: read every changed file, check wiring, confirm scope (no creep)
