package com.impulser.engine

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.nio.ByteBuffer

enum class AppState {
    IDLE,
    CALIBRATING,
    ARMED,
    SWEEPING,
    PROCESSING,
    REVIEW,
    EXPORTING
}

object NativeEngine {
    private var nativeHandle: Long = 0
    
    private val _appState = MutableStateFlow(AppState.IDLE)
    val appState: StateFlow<AppState> = _appState.asStateFlow()
    
    private val _irBuffer = MutableStateFlow<ByteBuffer?>(null)
    val irBuffer: StateFlow<ByteBuffer?> = _irBuffer.asStateFlow()
    
    private val _statusMessage = MutableStateFlow("")
    val statusMessage: StateFlow<String> = _statusMessage.asStateFlow()
    
    private val _trimStart = MutableStateFlow(0)
    val trimStart: StateFlow<Int> = _trimStart.asStateFlow()
    
    private val _trimEnd = MutableStateFlow(0)
    val trimEnd: StateFlow<Int> = _trimEnd.asStateFlow()
    
    init {
        System.loadLibrary("impulser_capture")
    }
    
    fun create(): Boolean {
        if (nativeHandle != 0L) {
            destroy()
        }
        
        nativeHandle = nativeCreate()
        if (nativeHandle == 0L) {
            _statusMessage.value = "Failed to initialize audio engine"
            return false
        }
        
        updateState()
        _statusMessage.value = "Engine initialized"
        return true
    }
    
    fun destroy() {
        if (nativeHandle != 0L) {
            nativeDestroy()
            nativeHandle = 0L
            _appState.value = AppState.IDLE
            _irBuffer.value = null
            _statusMessage.value = ""
        }
    }
    
    fun arm(): Boolean {
        if (nativeHandle == 0L) {
            _statusMessage.value = "Engine not initialized"
            return false
        }
        
        _statusMessage.value = "Arming..."
        _appState.value = AppState.ARMED
        
        val success = nativeArm()
        
        if (success) {
            updateState()
            _statusMessage.value = "Armed and ready"
        } else {
            _statusMessage.value = "Arming failed"
            _appState.value = AppState.IDLE
        }
        
        return success
    }
    
    fun runCalibration(): Boolean {
        if (nativeHandle == 0L) {
            _statusMessage.value = "Engine not initialized"
            return false
        }
        
        _statusMessage.value = "Starting calibration..."
        _appState.value = AppState.CALIBRATING
        
        val success = nativeRunCalibration()
        
        if (success) {
            updateState()
            _statusMessage.value = "Calibration complete"
        } else {
            _statusMessage.value = "Calibration failed"
            _appState.value = AppState.IDLE
        }
        
        return success
    }
    
    fun startSweep(): Boolean {
        if (nativeHandle == 0L) {
            _statusMessage.value = "Engine not initialized"
            return false
        }
        
        _statusMessage.value = "Starting sweep..."
        _appState.value = AppState.SWEEPING
        
        val success = nativeStartSweep()
        
        if (!success) {
            _statusMessage.value = "Failed to start sweep"
            _appState.value = AppState.ARMED
        }
        
        return success
    }
    
    fun stopSweep() {
        if (nativeHandle == 0L) return
        
        _statusMessage.value = "Processing..."
        _appState.value = AppState.PROCESSING
        
        nativeStopSweep()
        
        updateState()
        
        val bufferSize = 48000 * 4 * 10
        val buffer = ByteBuffer.allocateDirect(bufferSize)
        val irLength = nativeGetIRBuffer(buffer)
        
        if (irLength > 0) {
            buffer.limit(irLength * 4)
            _irBuffer.value = buffer
            _trimEnd.value = irLength
            _statusMessage.value = "IR captured: $irLength samples"
        } else {
            _statusMessage.value = "No IR data"
        }
    }
    
    fun setTrimStart(samples: Int) {
        if (nativeHandle == 0L) return
        if (samples >= 0 && samples < _trimEnd.value) {
            nativeSetTrimStart(samples)
            _trimStart.value = samples
        }
    }
    
    fun setTrimEnd(samples: Int) {
        if (nativeHandle == 0L) return
        if (samples > _trimStart.value) {
            nativeSetTrimEnd(samples)
            _trimEnd.value = samples
        }
    }
    
    fun export(path: String): Boolean {
        if (nativeHandle == 0L) {
            _statusMessage.value = "Engine not initialized"
            return false
        }
        
        if (_appState.value != AppState.REVIEW) {
            _statusMessage.value = "Not in REVIEW state"
            return false
        }
        
        _appState.value = AppState.EXPORTING
        _statusMessage.value = "Exporting..."
        
        val success = nativeExport(path)
        
        _appState.value = AppState.REVIEW
        
        if (success) {
            _statusMessage.value = "Exported to $path"
        } else {
            _statusMessage.value = "Export failed"
        }
        
        return success
    }
    
    fun discard() {
        if (nativeHandle == 0L) return
        
        nativeDiscard()
        _appState.value = AppState.IDLE
        _irBuffer.value = null
        _trimStart.value = 0
        _trimEnd.value = 0
        _statusMessage.value = "Capture discarded"
    }
    
    fun getSpectrogramTexId(): Int {
        if (nativeHandle == 0L) return 0
        return nativeGetSpectrogramTexId()
    }

    fun getProcessingProgress(): Float {
        if (nativeHandle == 0L) return 0f
        return nativeGetProcessingProgress()
    }

    fun getPlaybackProgress(): Float {
        if (nativeHandle == 0L) return 0f
        return nativeGetPlaybackProgress()
    }

    fun getInputLevel(): Float {
        if (nativeHandle == 0L) return 0f
        return nativeGetInputLevel()
    }

    fun getCurrentSpectrum(bins: FloatArray): Boolean {
        if (nativeHandle == 0L) return false
        return try {
            nativeGetCurrentSpectrum(bins)
            true
        } catch (e: Exception) {
            false
        }
    }

    fun getIRSpectrum(bins: FloatArray): Boolean {
        if (nativeHandle == 0L) return false
        return try {
            nativeGetIRSpectrum(bins)
            true
        } catch (e: Exception) {
            false
        }
    }

    fun getInputSpectrum(bins: FloatArray): Boolean {
        if (nativeHandle == 0L) return false
        return try {
            nativeGetInputSpectrum(bins, bins.size)
            true
        } catch (e: Exception) {
            false
        }
    }

    fun setSpectrogramSurface(surfacePtr: Long) {
        if (nativeHandle == 0L) return
        nativeSetSpectrogramSurface(surfacePtr)
    }
    
    fun setWaveformSurface(surfacePtr: Long) {
        if (nativeHandle == 0L) return
        nativeSetWaveformSurface(surfacePtr)
    }
    
    fun updateSpectrogram(data: FloatArray) {
        if (nativeHandle == 0L) return
        nativeUpdateSpectrogram(data)
    }
    
    fun updateWaveform(data: FloatArray, trimStart: Int, trimEnd: Int) {
        if (nativeHandle == 0L) return
        nativeUpdateWaveform(data, trimStart, trimEnd)
    }
    
    fun checkSweepComplete(): Boolean {
        if (nativeHandle == 0L) return false
        val completed = nativeCheckSweepComplete()
        if (completed) updateState()  // sync UI with native state change
        return completed
    }

    private fun updateState() {
        val stateCode = nativeGetState()
        _appState.value = AppState.entries.getOrElse(stateCode) { AppState.IDLE }
    }
    
    private external fun nativeCreate(): Long
    private external fun nativeDestroy()
    private external fun nativeGetState(): Int
    private external fun nativeGetProcessingProgress(): Float
    private external fun nativeGetPlaybackProgress(): Float
    private external fun nativeGetInputLevel(): Float
    private external fun nativeGetCurrentSpectrum(bins: FloatArray)
    private external fun nativeGetIRSpectrum(bins: FloatArray)
    private external fun nativeGetInputSpectrum(bins: FloatArray, nBins: Int): Boolean
    private external fun nativeArm(): Boolean
    private external fun nativeStartSweep(): Boolean
    private external fun nativeStopSweep()
    private external fun nativeSetTrimStart(samples: Int)
    private external fun nativeSetTrimEnd(samples: Int)
    private external fun nativeGetIRBuffer(buffer: ByteBuffer): Int
    private external fun nativeExport(path: String): Boolean
    private external fun nativeGetSpectrogramTexId(): Int
    private external fun nativeDiscard()
    private external fun nativeSetSpectrogramSurface(surfacePtr: Long)
    private external fun nativeSetWaveformSurface(surfacePtr: Long)
    private external fun nativeUpdateSpectrogram(data: FloatArray)
    private external fun nativeUpdateWaveform(data: FloatArray, trimStart: Int, trimEnd: Int)
    private external fun nativeCheckSweepComplete(): Boolean
    private external fun nativeRunCalibration(): Boolean
}
