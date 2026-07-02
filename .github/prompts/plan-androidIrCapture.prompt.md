## Plan: Android Impulse Response Capture Utility

**TL;DR** — A Kotlin + C++ Android app (API 29+) using Oboe for full-duplex audio, PFFFT for FFT-based ESS deconvolution, mandatory per-device hardware calibration, and a brutalist OpenGL ES 3.0 visualization. The C++ DSP core is entirely decoupled from the Kotlin UI via a thin JNI bridge. Eight phases, most within a phase parallelizable.

---

### Phase 1 — Audio Engine (Oboe Full-Duplex)

**Goal:** Reliable simultaneous play + capture with signal integrity guarantees.

1. Add `com.google.oboe:oboe:1.8.0` to `build.gradle`. Add `implementation 'com.google.prefab:oboe:1.8.0'` or integrate via CMake `FetchContent`.
2. Implement `OboeEngine.h/cpp`: creates two `AudioStream` objects — one output (`Direction::Output`, `PerformanceMode::LowLatency`, `AudioFormat::Float`), one input (`Direction::Input`, `InputPreset::Unprocessed`, same rate/format). Both at `48000 Hz`, mono.
3. Disable AGC/NS/AEC: `InputPreset::Unprocessed` handles this on API 29+. As a belt-and-suspenders measure, post-stream-open, enumerate `AudioEffect` descriptors via JNI and call `setEnabled(false)` on any AGC/NS/AEC effects found on the session ID.
4. Implement `RingBuffer<float>` (lock-free SPSC, power-of-2 size ≥ 4 × burst size) — written from the Oboe input callback, read by the DSP thread.
5. Frame-accurate latency compensation: after streams are opened, poll `getTimestamp()` on both. `delay_samples = round((t_input - t_output) * fs)`. Store as `mRoundTripDelaySamples`. This value is subtracted during deconvolution alignment.
6. Implement `OboeEngine::setCallback(IRCaptureCallback*)` so the DSP layer can register to receive complete captured buffers efficiently without coupling to Oboe internals.

**Files:** `cpp/engine/OboeEngine.{h,cpp}`, `cpp/engine/RingBuffer.h`

---

### Phase 2 — DSP Core: ESS + FFT Deconvolution *(depends on Phase 1)*

**Goal:** Produce a clean linear IR from the captured sweep recording.

1. Integrate **PFFFT** as a CMake subdirectory (`third_party/pffft/`). License: BSD-3. Configure CMake to build as a static library with NEON enabled (`-mfpu=neon -mfloat-abi=softfp` or ABI-auto on ARM64).
2. Implement `ESSGenerator.cpp`:
   - Parameters: `f1 = 20.0f`, `f2 = 20000.0f`, `T = 7.0f` seconds, `fs = 48000`.
   - `L = T / log(f2/f1)`
   - Sample loop: `x[n] = sinf(2π · f1 · L · (expf(n / (fs·L)) - 1.0f))`
   - Apply 20ms raised-cosine fade-in and 20ms raised-cosine fade-out to prevent DAC clicks.
   - Expose: `generate(float* out, int numSamples)`.
3. Implement `Deconvolver.cpp`:
   - Pre-compute analytical inverse filter: `h_inv[n] = x[N-1-n] · expf(-n · log(f2/f1) / (T·fs))` (time-reversed + amplitude-modulated copy of sweep).
   - Zero-pad both `y[]` (recorded) and `h_inv[]` to next power of 2 ≥ `len_y + len_h_inv − 1`.
   - Allocate PFFFT workspace. Execute `pffft_transform_ordered` on both → complex spectra `Y`, `H_inv`.
   - Complex multiply: `IR[k] = Y[k] · H_inv[k]` (element-wise).
   - `pffft_transform_ordered` inverse → `ir_full[]`.
   - Harmonic isolation: the linear IR sits at sample index `N_fft - 1` (end of output for causal convolution). The n-th harmonic is offset by `τ_n = round(T · log(n) / log(f2/f1) · fs)` samples earlier. Trim a window of `[N_fft - τ_2 + guard_samples ... N_fft + tail_samples]` to isolate the linear IR and discard all harmonic images.
   - Expose: `deconvolve(const float* recorded, int recLen, float** irOut, int* irLen)`.

**Files:** `cpp/engine/ESSGenerator.{h,cpp}`, `cpp/engine/Deconvolver.{h,cpp}`, `cpp/third_party/pffft/`

---

### Phase 3 — Hardware Calibration Filter *(depends on Phase 2)*

**Goal:** Flat frequency response by inverting the device's speaker–microphone transfer function.

1. Implement `CalibrationFilter.cpp`:
   - `runCalibration()`: plays a short (3s) ESS — same parameters as Phase 2 — through the device speaker and records simultaneously. Uses `OboeEngine` from Phase 1.
   - Align recorded signal using the round-trip delay from Phase 1.
   - Compute device transfer function: `H_dev[k] = Y[k] / X[k]` where `X[k] = FFT(ess_reference)`.
   - Regularized inversion (Tikhonov): `H_corr[k] = conj(H_dev[k]) / (|H_dev[k]|² + ε)` where `ε = 1e-3` (−60 dBFS noise floor protection, prevents division by near-zero in low-energy bands).
   - Store as `h_corr_time[]` (IFFT of the above, real part only, windowed to 2048 taps).
   - Persist serialized coefficients to `SharedPreferences` keyed by `Build.MANUFACTURER + "_" + Build.MODEL + "_" + Build.FINGERPRINT` (hash). On next launch, calibration is skipped if a valid stored filter exists.
2. During capture in Phase 2, apply calibration as a pre-filter: convolve the recorded sweep with `h_corr_time[]` prior to deconvolution (implemented as a frequency-domain multiply inside `Deconvolver`).

**Files:** `cpp/engine/CalibrationFilter.{h,cpp}`

---

### Phase 4 — IR Post-Processing *(depends on Phase 2 + 3)*

**Goal:** Onset detection, precise trimming, half-Hann tail, strict zero endpoint.

1. Implement `IRProcessor.cpp`:
   - **Peak detection:** Find `n_peak = argmax(|ir[n]|)` in the linear IR window.
   - **Pre-pad:** Start output at `n_peak - round(0.05 * fs)` (50ms pre-delay, preserves pre-ringing without wasting file space).
   - **Noise floor search:** Scan backward from the tail. Find `n_noise` where `|ir[n]| < peakAmplitude · 10^(-60/20)` (−60 dBFS below peak). Use a 10ms RMS window to avoid triggering on isolated noise spikes.
   - **Half-Hann window:** Apply `w[n] = 0.5 · (1 + cos(π · (n - n_noise) / (n_end - n_noise)))` for `n ∈ [n_noise, n_end]`. This analytically reaches exactly 0.0f at `n_end`, guaranteeing a zero-crossing endpoint and preventing convolution pre-ringing artifacts in the host engine.
   - **Normalize:** Scale peak to −3 dBFS (`0.7079f`).
   - Expose trim handles: `setTrimStart(int samples)`, `setTrimEnd(int samples)` — used by UI drag interaction.

**Files:** `cpp/engine/IRProcessor.{h,cpp}`

---

### Phase 5 — OpenGL ES 3.0 Visualization *(parallel with Phases 2–4)*

**Goal:** Real-time scrolling spectrogram during capture + waveform display in REVIEW state.

**Spectrogram:**
1. Implement `SpectrogramRenderer.cpp`:
   - CPU-side STFT: 1024-point FFT, 50% overlap (512-sample hop), Hann window, computed from the `RingBuffer` on a dedicated low-priority thread. Output: log-magnitude row `float[512]` per hop.
   - Allocate a `GL_R32F`, `1024 × 512` texture (1024 time columns × 512 frequency bins). Wrap mode: `GL_REPEAT` for circular scrolling.
   - Each hop: `glTexSubImage2D` one column update (width=1, height=512), then advance `uScrollOffset` uniform by `1.0/1024.0`.
   - Fragment shader: sample `texture(uSpectrogram, vec2(mod(texCoord.x + uScrollOffset, 1.0), texCoord.y))`, map via a 256×1 `GL_RGBA8` colormap LUT texture (deep blue → cyan → yellow → white). Store colormap as a hardcoded `const uint8_t[]` in C++.
   - Log frequency axis: apply `texCoord.y = log(1 + k·y) / log(1 + k)` remap in fragment shader for perceptual frequency scaling.
2. Implement `WaveformRenderer.cpp` (*parallel with spectrogram*):
   - Dynamic VBO — upload `float[N]` IR samples as a `GL_LINES` strip.
   - Vertex shader: maps sample index → NDC X, amplitude → NDC Y. Adjusts based on visible zoom window.
   - Two vertical line markers (trim-start and trim-end) rendered as separate 2-vertex `GL_LINES` draw calls.
   - During REVIEW state: renders the final processed IR. During SWEEPING state: renders live circular buffer (live monitoring).

**Files:** `cpp/renderer/SpectrogramRenderer.{h,cpp}`, `cpp/renderer/WaveformRenderer.{h,cpp}`

---

### Phase 6 — Kotlin UI + JNI Bridge *(depends on Phase 1 stubs, parallel with Phase 2–4)*

**Goal:** Brutalist single-screen UI with zero nested menus.

**JNI Bridge:**
1. `JniBridge.cpp` — All `extern "C" JNIEXPORT` functions. Exposes:  
   `nativeCreate()`, `nativeDestroy()`, `nativeArm()`, `nativeStartSweep()`, `nativeGetState()`, `nativeGetIRBuffer(jobject byteBuffer)`, `nativeSetTrimStart(jint)`, `nativeSetTrimEnd(jint)`, `nativeExport(jstring path)`, `nativeGetSpectrogramTexId()`.
2. `NativeEngine.kt` — Kotlin singleton wrapping all JNI calls. Exposes `StateFlow<AppState>` updated from a polling coroutine (100ms interval) or JNI-side callback via `CallVoidMethod`.

**AppState FSM (Kotlin):**
```
IDLE → CALIBRATING → ARMED → SWEEPING → PROCESSING → REVIEW → EXPORTING → IDLE
                                                  ↑← DISCARD ──────────────|
```

**UI (`MainScreen.kt`, Jetpack Compose):**
- **Color palette:** Background `#0A0A0A`, foreground `#F0F0F0`, active accent `#FF4500`, warning `#FFB300`. Typeface: system monospace (`FontFamily.Monospace`).
- **Layout:** `Column` — top 65% `GLSurfaceViewWrapper` (AndroidView wrapping `GLSurfaceView`), remaining 35% control strip.
- **Control strip (4 elements only):**
  - **ARM** — large full-width toggle button. Label changes: `ARM` → `CALIBRATING…` → `ARMED` → `SWEEPING…` → `PROCESSING…` → `REVIEW`.
  - **STIMULUS** — currently disabled (ESS only); renders as static label `STIMULUS: ESS` in smaller monospaced type. Stub for future MLS toggle.
  - **TRIM** — enabled only in REVIEW state. Two `Slider` composables (start/end), directly linked to `nativeSetTrimStart/End`. Renders as inline two-handle range slider.
  - **EXPORT** — enabled only in REVIEW state. Single button. Triggers file picker via `ActivityResultContracts.CreateDocument`, then calls `nativeExport()`.
- **Status bar:** single-line `Text` at bottom showing e.g. `48kHz · 24bit · mono · 7.2s · peak -3.0dBFS`.
- No dialogs, no drawers, no settings screen.

**Files:** `cpp/jni/JniBridge.cpp`, `kotlin/engine/NativeEngine.kt`, `kotlin/engine/AppState.kt`, `kotlin/ui/MainScreen.kt`, `kotlin/ui/GLSurfaceViewWrapper.kt`

---

### Phase 7 — WAV Export *(depends on Phase 4)*

**Goal:** Standard 48kHz/24-bit mono RIFF/WAV with correct header.

1. Implement `WavExporter.cpp`:
   - Write standard RIFF/WAV header: `RIFF` → `WAVE` → `fmt ` chunk (PCM=1, channels=1, sampleRate=48000, bitsPerSample=24, blockAlign=3, byteRate=144000) → `data` chunk.
   - Convert `float` samples to 24-bit signed PCM: `int32_t s = clamp(round(x * 8388607.0f), -8388608, 8388607)`. Write as 3-byte little-endian (`s & 0xFF`, `(s>>8) & 0xFF`, `(s>>16) & 0xFF`).
   - No dynamic memory allocation during write — stream directly to `FILE*`.
2. `WavExportManager.kt`: uses `MediaStore.Audio.Media` (API 29+ scoped storage) to write to `Music/ImpulseResponses/` without `WRITE_EXTERNAL_STORAGE`. Passes the resolved `ContentResolver` fd to native.

**Files:** `cpp/engine/WavExporter.{h,cpp}`, `kotlin/export/WavExportManager.kt`

---

### Phase 8 — Integration, Permissions, Manifest *(depends on all phases)*

1. `AndroidManifest.xml`: `RECORD_AUDIO` (dangerous), `MODIFY_AUDIO_SETTINGS`. No `READ/WRITE_EXTERNAL_STORAGE` needed (scoped storage via MediaStore). Add `android:hardwareAccelerated="true"`.
2. Runtime permission flow in `MainActivity.kt`: Request `RECORD_AUDIO` before first `ARM`. If denied, show inline rationale text in the status bar (not a dialog).
3. Foreground service for long captures (>30s sweep + calibration): `AudioCaptureForegroundService.kt` with notification. Required by Android 10+ to maintain capture in the background.
4. `CMakeLists.txt`: Link `log`, `android`, `OpenSLES` (not needed — Oboe handles it), `GLESv3`, `EGL`, `aaudio`. Set `CMAKE_CXX_STANDARD 17`.
5. Keep-screen-on: `FLAG_KEEP_SCREEN_ON` on the Activity window during SWEEPING and CALIBRATING states.

---

### Relevant Files

| Path | Purpose |
|---|---|
| `app/src/main/cpp/CMakeLists.txt` | Build graph: PFFFT, Oboe, GLES3, EGL linkage |
| `app/src/main/cpp/engine/OboeEngine.{h,cpp}` | Full-duplex Oboe streams, latency measurement |
| `app/src/main/cpp/engine/ESSGenerator.{h,cpp}` | ESS signal with cosine fade-in/out |
| `app/src/main/cpp/engine/Deconvolver.{h,cpp}` | PFFFT deconvolution + harmonic window isolation |
| `app/src/main/cpp/engine/CalibrationFilter.{h,cpp}` | Tikhonov-regularized inverse filter, SharedPrefs persist |
| `app/src/main/cpp/engine/IRProcessor.{h,cpp}` | Peak detection, half-Hann tail window, normalize |
| `app/src/main/cpp/engine/WavExporter.{h,cpp}` | RIFF/WAV 48kHz/24-bit/mono write |
| `app/src/main/cpp/engine/RingBuffer.h` | SPSC lock-free ring buffer |
| `app/src/main/cpp/renderer/SpectrogramRenderer.{h,cpp}` | STFT, GL_R32F texture, scrolling fragment shader |
| `app/src/main/cpp/renderer/WaveformRenderer.{h,cpp}` | Dynamic VBO IR waveform + trim markers |
| `app/src/main/cpp/jni/JniBridge.cpp` | All JNI entry points |
| `app/src/main/kotlin/engine/NativeEngine.kt` | JNI wrapper + `StateFlow<AppState>` |
| `app/src/main/kotlin/ui/MainScreen.kt` | Single-screen Compose UI, brutalist layout |
| `app/src/main/kotlin/export/WavExportManager.kt` | MediaStore scoped storage |
| `app/src/main/AndroidManifest.xml` | Permissions, hardware acceleration flag |

---

### Verification

1. **Unit: ESS math** — Generate 7s ESS, auto-deconvolve against itself; resulting IR should be a near-perfect Dirac delta (peak sample ≥ 0.99, all others < 0.001).
2. **Unit: Harmonic isolation** — Feed a synthetically distorted response; confirm the 2nd harmonic image does not appear in the trimmed IR output window.
3. **Unit: Half-Hann terminal value** — Assert `ir[n_end] == 0.0f` (exact) after windowing.
4. **Unit: WAV header** — Load exported `.wav` via libsndfile or any reference WAV reader; verify `sampleRate=48000`, `bitDepth=24`, `channels=1`, `format=PCM`.
5. **Integration: `UNPROCESSED` on physical device** — Record a 1kHz tone with AGC on vs. `InputPreset::Unprocessed`; amplitude should remain constant over 10 seconds in the latter case (no AGC pumping).
6. **Integration: Calibration persistence** — Force-quit, relaunch app; confirm calibration phase is skipped and stored filter coefficients are loaded from `SharedPreferences`.
7. **Integration: Round-trip latency alignment** — Play a known click, record it; verify `delay_samples` estimate is within ±1 sample of the measured alignment.
8. **Visual: Spectrogram during sweep** — ESS should produce a visually obvious diagonal line sweep from low to high frequency on the display.
9. **Load test in convolution engine** — Import the exported IR into a DAW (Reaper + ReaVerb, or Ableton Convolution Reverb). Verify no pre-ring, no DC offset, clean tail.

---

### Decisions

- **minSdk = 29** — `InputPreset::Unprocessed` is reliable only from API 29; no fallback path needed.
- **ESS only** — MLS/FHT is deferred. `STIMULUS` button renders as a disabled static label; it becomes an active toggle in a future phase.
- **Calibration is mandatory** — Runs once per device fingerprint, persists indefinitely. User sees a status message but has no skip option.
- **Mono WAV** — All standard convolution engines (Reaper, Ableton, Logic) accept mono IRs. No stereo duplication overhead.
- **OpenGL ES 3.0** — CPU STFT + R32F texture upload is sufficient at 48kHz with 50% overlap; no compute shader complexity required.
- **Out of scope:** network sync, multi-microphone, frequency weighting curves (e.g. A-weighting), IR database/cloud sync, MLS path.

---

### Further Considerations

1. **OEM HAL override risk** — Some Samsung and Xiaomi devices have been documented to silently re-enable noise suppression at the HAL layer regardless of `InputPreset::Unprocessed`. A runtime self-test (record 5s silence post-calibration, check for telltale NS spectral artifacts) could detect this and present a warning in the status bar. Worth adding as a post-MVP hardening task.

2. **Calibration filter length** — 2048 taps at 48kHz = 42.7ms. For very reverberant phone acoustics (e.g., metallic unibody devices with strong speaker-to-mic resonance), this may be insufficient. A runtime check: if the measured device tail exceeds 2048 taps, automatically double to 4096. This adds marginal convolution cost but stays deterministic.

3. **GLSurfaceView vs. SurfaceView+EGL on Qualcomm** — A small percentage of Snapdragon 8xx devices show GLSurfaceView renderer thread priority drops during concurrent intense audio activity. If frame drops are observed in integration testing, swapping to a manual `SurfaceView` + `EGL14` + dedicated render thread resolves it. Flag as a known risk; address only if observed empirically.
