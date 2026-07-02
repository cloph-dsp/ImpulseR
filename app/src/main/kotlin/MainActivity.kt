package com.impulser.capture

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
import android.util.Log
import android.view.WindowManager
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import com.impulser.engine.AppState
import com.impulser.engine.NativeEngine
import kotlinx.coroutines.*

enum class UIState {
    IDLE, CALIBRATING, ARMED, SWEEPING, PROCESSING, REVIEW, ERROR, EXPORTING
}

class MainActivity : ComponentActivity() {
    private val TAG = "ImpulseR"
    private val scope = CoroutineScope(Dispatchers.Main + SupervisorJob())
    private var exportedPath: String? = null
    private var irSampleData: FloatArray = FloatArray(0)
    
    private val requestPermissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { permissions ->
        val audioGranted = permissions[Manifest.permission.RECORD_AUDIO] ?: false
        
        if (audioGranted) {
            NativeEngine.create()
        } else {
            Toast.makeText(this, "Microphone permission required", Toast.LENGTH_LONG).show()
        }
    }
    
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Log.d(TAG, "onCreate started")
        
        // Request permissions first
        val permissionsToRequest = mutableListOf(Manifest.permission.RECORD_AUDIO)
        if (android.os.Build.VERSION.SDK_INT <= android.os.Build.VERSION_CODES.P) {
            permissionsToRequest.add(Manifest.permission.WRITE_EXTERNAL_STORAGE)
        }
        
        val notGranted = permissionsToRequest.filter { 
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED 
        }
        
        if (notGranted.isNotEmpty()) {
            requestPermissionLauncher.launch(notGranted.toTypedArray())
        } else {
            NativeEngine.create()
        }
        
        setContent {
            var uiState by remember { mutableStateOf(UIState.IDLE) }
            var spectrumData by remember { mutableStateOf(FloatArray(64) { 0f }) }
            var sweepProgress by remember { mutableStateOf(0f) }
            var showExportSuccess by remember { mutableStateOf<String?>(null) }
            var irSampleCount by remember { mutableIntStateOf(0) }
            var trimStart by remember { mutableFloatStateOf(0f) }
            var trimEnd by remember { mutableFloatStateOf(1f) }
            var statusMessage by remember { mutableStateOf("Tap ARM to start") }
            var isProcessing by remember { mutableStateOf(false) }
            
            LaunchedEffect(Unit) {
                NativeEngine.appState.collect { state ->
                    uiState = when (state) {
                        AppState.IDLE -> UIState.IDLE
                        AppState.CALIBRATING -> UIState.CALIBRATING
                        AppState.ARMED -> UIState.ARMED
                        AppState.SWEEPING -> UIState.SWEEPING
                        AppState.PROCESSING -> UIState.PROCESSING
                        AppState.REVIEW -> UIState.REVIEW
                        AppState.EXPORTING -> UIState.EXPORTING
                    }
                    isProcessing = state == AppState.PROCESSING || state == AppState.CALIBRATING || state == AppState.EXPORTING
                    
                    if (state == AppState.REVIEW) {
                        irSampleCount = NativeEngine.trimEnd.value - NativeEngine.trimStart.value
                        trimEnd = 1f
                        trimStart = 0f
                    }
                }
            }
            
            LaunchedEffect(Unit) {
                NativeEngine.statusMessage.collect { msg ->
                    statusMessage = msg
                }
            }
            
            LaunchedEffect(uiState, isProcessing) {
                while (isProcessing) {
                    delay(100)
                    when (uiState) {
                        UIState.CALIBRATING -> {
                            sweepProgress = (sweepProgress + 0.02f).coerceAtMost(1f)
                            spectrumData = generateSweepSpectrum(sweepProgress)
                        }
                        UIState.SWEEPING -> {
                            // Check if sweep completed
                            NativeEngine.checkSweepComplete()
                            sweepProgress = (sweepProgress + 0.02f).coerceAtMost(1f)
                            spectrumData = generateSweepSpectrum(sweepProgress)
                        }
                        UIState.PROCESSING -> {
                            sweepProgress = (sweepProgress + 0.02f).coerceAtMost(1f)
                            spectrumData = generateProcessingSpectrum(sweepProgress)
                        }
                        else -> {}
                    }
                }
            }
            
            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .background(Color(0xFF0A0A0A))
            ) {
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .weight(0.65f)
                        .background(Color(0xFF000000))
                ) {
                    SpectrumVisualizer(
                        data = spectrumData,
                        modifier = Modifier.fillMaxSize()
                    )
                    
                    Text(
                        text = when (uiState) {
                            UIState.IDLE -> "IDLE"
                            UIState.CALIBRATING -> "CALIBRATING..."
                            UIState.ARMED -> "ARMED"
                            UIState.SWEEPING -> "SWEEPING..."
                            UIState.PROCESSING -> "PROCESSING..."
                            UIState.REVIEW -> "REVIEW"
                            UIState.ERROR -> "ERROR"
                            UIState.EXPORTING -> "EXPORTING..."
                        },
                        color = Color(0xFFFF4500),
                        fontFamily = FontFamily.Monospace,
                        fontWeight = FontWeight.Bold,
                        fontSize = 24.sp,
                        modifier = Modifier
                            .align(Alignment.TopStart)
                            .padding(16.dp)
                    )
                    
                    Row(
                        modifier = Modifier
                            .align(Alignment.BottomCenter)
                            .fillMaxWidth()
                            .padding(horizontal = 16.dp, vertical = 4.dp),
                        horizontalArrangement = Arrangement.SpaceBetween
                    ) {
                        listOf("20", "100", "1k", "10k", "20k").forEach { freq ->
                            Text(
                                text = freq,
                                color = Color(0xFF404040),
                                fontFamily = FontFamily.Monospace,
                                fontSize = 10.sp
                            )
                        }
                    }
                }
                
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .weight(0.35f)
                        .background(Color(0xFF1A1A1A))
                        .padding(16.dp),
                    verticalArrangement = Arrangement.SpaceEvenly
                ) {
                    // Calibration button
                    Button(
                        onClick = {
                            NativeEngine.runCalibration()
                        },
                        modifier = Modifier
                            .fillMaxWidth()
                            .height(40.dp),
                        colors = ButtonDefaults.buttonColors(
                            containerColor = Color(0xFF333333)
                        ),
                        enabled = uiState == UIState.IDLE
                    ) {
                        Text(
                            text = "CALIBRATE",
                            fontFamily = FontFamily.Monospace,
                            fontSize = 14.sp,
                            color = Color(0xFFFFB300)
                        )
                    }
                    
                    Button(
                        onClick = {
                            when (uiState) {
                                UIState.IDLE -> {
                                    keepScreenOn = true
                                    NativeEngine.arm()
                                }
                                UIState.ARMED -> {
                                    NativeEngine.startSweep()
                                }
                                UIState.REVIEW -> {
                                    keepScreenOn = false
                                    NativeEngine.discard()
                                    spectrumData = FloatArray(64) { 0f }
                                    irSampleCount = 0
                                    exportedPath = null
                                    sweepProgress = 0f
                                }
                                else -> {}
                            }
                        },
                        modifier = Modifier
                            .fillMaxWidth()
                            .height(56.dp),
                        colors = ButtonDefaults.buttonColors(
                            containerColor = when (uiState) {
                                UIState.IDLE -> Color(0xFFFF4500)
                                UIState.CALIBRATING -> Color(0xFFFFB300)
                                UIState.ARMED -> Color(0xFF00FF00)
                                UIState.SWEEPING -> Color(0xFFFF4500)
                                UIState.PROCESSING -> Color(0xFFFFB300)
                                UIState.REVIEW -> Color(0xFF00FF00)
                                UIState.ERROR -> Color(0xFFFF0000)
                                UIState.EXPORTING -> Color(0xFFFFB300)
                            }
                        ),
                        enabled = uiState != UIState.SWEEPING && 
                                  uiState != UIState.PROCESSING && 
                                  uiState != UIState.CALIBRATING
                    ) {
                        Text(
                            text = when (uiState) {
                                UIState.IDLE -> "ARM"
                                UIState.CALIBRATING -> "CALIBRATING..."
                                UIState.ARMED -> "START SWEEP"
                                UIState.SWEEPING -> "SWEEPING..."
                                UIState.PROCESSING -> "PROCESSING..."
                                UIState.REVIEW -> "RESET"
                                UIState.ERROR -> "RETRY"
                                UIState.EXPORTING -> "EXPORTING..."
                            },
                            fontFamily = FontFamily.Monospace,
                            fontWeight = FontWeight.Bold,
                            fontSize = 18.sp,
                            color = Color(0xFF0A0A0A)
                        )
                    }
                    
if (uiState == UIState.REVIEW && irSampleCount > 0) {
                        Row(
                            modifier = Modifier
                                .fillMaxWidth()
                                .height(40.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Text(
                                text = "START",
                                color = Color(0xFF606060),
                                fontFamily = FontFamily.Monospace,
                                fontSize = 10.sp,
                                modifier = Modifier.width(48.dp)
                            )
                            Slider(
                                value = trimStart,
                                onValueChange = { 
                                    trimStart = it.coerceIn(0f, trimEnd - 0.01f)
                                    NativeEngine.setTrimStart((trimStart * irSampleCount).toInt())
                                },
                                modifier = Modifier.weight(1f),
                                colors = SliderDefaults.colors(
                                    thumbColor = Color(0xFFFF4500),
                                    activeTrackColor = Color(0xFFFF4500)
                                )
                            )
                            Text(
                                text = "%.1fs".format(trimStart * irSampleCount / 48000f),
                                color = Color(0xFF808080),
                                fontFamily = FontFamily.Monospace,
                                fontSize = 10.sp,
                                modifier = Modifier.width(48.dp)
                            )
                        }
                        
                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Text(
                                text = "END",
                                color = Color(0xFF606060),
                                fontFamily = FontFamily.Monospace,
                                fontSize = 10.sp,
                                modifier = Modifier.width(48.dp)
                            )
                            Slider(
                                value = trimEnd,
                                onValueChange = { 
                                    trimEnd = it.coerceIn(trimStart + 0.01f, 1f)
                                    NativeEngine.setTrimEnd((trimEnd * irSampleCount).toInt())
                                },
                                modifier = Modifier.weight(1f),
                                colors = SliderDefaults.colors(
                                    thumbColor = Color(0xFFFF4500),
                                    activeTrackColor = Color(0xFFFF4500)
                                )
                            )
                            Text(
                                text = "%.1fs".format(trimEnd * irSampleCount / 48000f),
                                color = Color(0xFF808080),
                                fontFamily = FontFamily.Monospace,
                                fontSize = 10.sp,
                                modifier = Modifier.width(48.dp)
                            )
                        }
                        
                        Button(
                            onClick = {
                                val fileName = "impulse_${System.currentTimeMillis()}.wav"
                                val exportPath = "Music/ImpulseResponses/$fileName"
                                val success = NativeEngine.export(exportPath)
                                showExportSuccess = if (success) "Saved: $fileName" else "Export failed"
                            },
                            modifier = Modifier
                                .fillMaxWidth()
                                .height(40.dp),
                            colors = ButtonDefaults.buttonColors(
                                containerColor = Color(0xFF2196F3)
                            )
                        ) {
                            Text(
                                text = "EXPORT WAV",
                                fontFamily = FontFamily.Monospace,
                                fontWeight = FontWeight.Bold,
                                fontSize = 14.sp,
                                color = Color.White
                            )
                        }
                    }
                    
                    showExportSuccess?.let { msg ->
                        Text(
                            text = msg,
                            color = Color(0xFF4CAF50),
                            fontFamily = FontFamily.Monospace,
                            fontSize = 12.sp
                        )
                    }
                    
                    Text(
                        text = "STIMULUS: ESS 20Hz-20kHz 7s",
                        color = Color(0xFF808080),
                        fontFamily = FontFamily.Monospace,
                        fontSize = 12.sp
                    )
                    
                    Text(
                        text = when (uiState) {
                            UIState.IDLE -> "48kHz · 24bit · mono"
                            UIState.CALIBRATING -> "Calibrating hardware..."
                            UIState.ARMED -> "Ready to capture"
                            UIState.SWEEPING -> "Playing ESS & recording..."
                            UIState.PROCESSING -> "Deconvolving IR..."
                            UIState.REVIEW -> if (irSampleCount > 0) "IR: $irSampleCount samples" else "IR captured"
                            UIState.ERROR -> "Error - check logs"
                            UIState.EXPORTING -> "Exporting..."
                        },
                        color = Color(0xFF808080),
                        fontFamily = FontFamily.Monospace,
                        fontSize = 12.sp
                    )
                    
                    Text(
                        text = statusMessage,
                        color = Color(0xFF606060),
                        fontFamily = FontFamily.Monospace,
                        fontSize = 10.sp
                    )
                }
            }
        }
        Log.d(TAG, "onCreate finished")
    }
    
    private var keepScreenOn: Boolean = false
        set(value) {
            field = value
            if (value) {
                window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            } else {
                window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            }
        }
    
    override fun onDestroy() {
        super.onDestroy()
        NativeEngine.destroy()
        scope.cancel()
        keepScreenOn = false
    }
}

@Composable
fun SpectrumVisualizer(
    data: FloatArray,
    modifier: Modifier = Modifier
) {
    Canvas(modifier = modifier) {
        val barCount = data.size
        val barWidth = size.width / barCount
        val maxHeight = size.height * 0.9f
        
        data.forEachIndexed { index, value ->
            val barHeight = (value * maxHeight).coerceAtLeast(0f)
            val x = index * barWidth
            
            val hue = (value * 30f).coerceIn(0f, 30f)
            val color = Color.hsv(30f - hue, 1f, 1f)
            
            drawRect(
                color = color,
                topLeft = Offset(x + 2, size.height - barHeight),
                size = androidx.compose.ui.geometry.Size(barWidth - 4, barHeight)
            )
            
            if (barHeight > 20) {
                drawRect(
                    color = color.copy(alpha = 0.3f),
                    topLeft = Offset(x + 2, size.height),
                    size = androidx.compose.ui.geometry.Size(barWidth - 4, barHeight * 0.3f)
                )
            }
        }
        
        for (i in 1..4) {
            val y = size.height * i / 5
            drawLine(
                color = Color(0xFF202020),
                start = Offset(0f, y),
                end = Offset(size.width, y),
                strokeWidth = 1f
            )
        }
    }
}

fun generateSweepSpectrum(progress: Float): FloatArray {
    val data = FloatArray(64)
    val freq = 20f * Math.pow(1000.0, progress.toDouble()).toFloat()
    
    for (i in data.indices) {
        val binFreq = 20f * Math.pow(1000.0, (i.toFloat() / 63).toDouble()).toFloat()
        val diff = kotlin.math.abs(binFreq - freq)
        val response = if (diff < freq * 0.3f) 0.8f + kotlin.random.Random.nextFloat() * 0.2f else 0.1f + kotlin.random.Random.nextFloat() * 0.1f
        data[i] = response
    }
    return data
}

fun generateProcessingSpectrum(progress: Float): FloatArray {
    val data = FloatArray(64)
    val peakPos = (progress * 63).toInt()
    
    for (i in data.indices) {
        val dist = kotlin.math.abs(i - peakPos)
        data[i] = if (dist < 3) (1f - dist / 3f) * progress else 0f
    }
    return data
}
