package com.impulser.capture

import android.Manifest
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.view.WindowManager
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
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
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import androidx.documentfile.provider.DocumentFile
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
    private var audioLostReceiver: BroadcastReceiver? = null
    
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
        
        audioLostReceiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context?, intent: Intent?) {
                val currentState = NativeEngine.appState.value
                when (currentState) {
                    AppState.SWEEPING -> {
                        NativeEngine.stopSweep()
                    }
                    AppState.CALIBRATING -> {
                        // ponytail: no native calibration cancel — discard resets to IDLE cleanly
                        NativeEngine.discard()
                    }
                    else -> {}
                }
            }
        }
        val filter = IntentFilter(AudioCaptureForegroundService.ACTION_AUDIO_LOST)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(audioLostReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            registerReceiver(audioLostReceiver, filter)
        }

        setContent {
            var uiState by remember { mutableStateOf(UIState.IDLE) }
            var spectrumData by remember { mutableStateOf(FloatArray(64) { 0f }) }
            var cachedIR by remember { mutableStateOf(FloatArray(64) { 0f }) }
            var sweepProgress by remember { mutableStateOf(0f) }
            var inputLevel by remember { mutableStateOf(0f) }
            var showExportSuccess by remember { mutableStateOf<String?>(null) }
            var irSampleCount by remember { mutableIntStateOf(0) }
            var trimStart by remember { mutableFloatStateOf(0f) }
            var trimEnd by remember { mutableFloatStateOf(1f) }
            var statusMessage by remember { mutableStateOf("Tap ARM to start") }
            var isProcessing by remember { mutableStateOf(false) }
            var irFilename by remember { mutableStateOf("") }
            var chosenFolderUri by remember { mutableStateOf<Uri?>(null) }
            // Spectrogram history: last N frames of FFT magnitude (log-mapped to 64 bins)
            val spectrogramRows = 32
            var spectrogramHistory by remember { mutableStateOf(Array(spectrogramRows) { FloatArray(64) { 0f } }) }
            val scope = rememberCoroutineScope()
            val ctx = this@MainActivity

            val folderPickerLauncher = rememberLauncherForActivityResult(
                contract = ActivityResultContracts.OpenDocumentTree()
            ) { uri ->
            uri?.let {
                runCatching {
                    contentResolver.takePersistableUriPermission(
                        it,
                        Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                    )
                }
                chosenFolderUri = it
            }
            }
            
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
                    isProcessing = state == AppState.SWEEPING || state == AppState.PROCESSING || state == AppState.CALIBRATING
                    
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
            
            LaunchedEffect(uiState) {
                when (uiState) {
                    UIState.IDLE, UIState.EXPORTING, UIState.ERROR -> {
                        spectrumData = FloatArray(64) { 0f }
                    }
                    UIState.REVIEW -> {
                        if (cachedIR.isEmpty()) {
                            NativeEngine.getIRSpectrum(cachedIR)
                        }
                        spectrumData = cachedIR
                    }
                    else -> {}
                }
            }

            LaunchedEffect(uiState) {
                if (uiState == UIState.REVIEW && cachedIR.isNotEmpty() && irFilename.isBlank()) {
                    irFilename = "impulse_${System.currentTimeMillis()}.wav"
                }
            }

            LaunchedEffect(uiState) {
                while (isProcessing) {
                    when (uiState) {
                    UIState.CALIBRATING -> {
                        inputLevel = NativeEngine.getInputLevel()
                        sweepProgress = NativeEngine.getPlaybackProgress()
                        NativeEngine.getCurrentSpectrum(spectrumData)
                    }
                        UIState.SWEEPING -> {
                            NativeEngine.checkSweepComplete()
                            inputLevel = NativeEngine.getInputLevel()
                            sweepProgress = NativeEngine.getPlaybackProgress()
                            NativeEngine.getCurrentSpectrum(spectrumData)
                            // Shift history down and insert new row at top
                            for (i in spectrogramRows - 1 downTo 1) {
                                spectrogramHistory[i] = spectrogramHistory[i - 1]
                            }
                            spectrogramHistory[0] = spectrumData.copyOf()
                        }
                        UIState.PROCESSING -> {
                            sweepProgress = NativeEngine.getProcessingProgress()
                            inputLevel = 0f
                            NativeEngine.getIRSpectrum(spectrumData)
                        }
                        else -> {}
                    }
                    delay(50)
                }
            }

            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .background(Color(0xFF0A0A0A))
            ) {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .weight(0.65f)
                        .background(Color(0xFF000000))
                ) {
                    Box(modifier = Modifier.weight(1f)) {
                        SpectrumVisualizer(
                            data = spectrumData,
                            modifier = Modifier.fillMaxSize()
                        )
                    }
                    if (uiState == UIState.SWEEPING) {
                        Box(modifier = Modifier.weight(1f)) {
                            SpectrogramVisualizer(
                                history = spectrogramHistory,
                                modifier = Modifier.fillMaxSize()
                            )
                        }
                    }
                }
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .weight(0.1f)
                ) {
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
                        color = when (uiState) {
                            UIState.SWEEPING -> Color(0xFFFF1744)
                            UIState.PROCESSING -> Color(0xFFFFB300)
                            UIState.CALIBRATING -> Color(0xFFFFB300)
                            else -> Color(0xFFFF4500)
                        },
                        fontFamily = FontFamily.Monospace,
                        fontWeight = FontWeight.Bold,
                        fontSize = 24.sp,
                        modifier = Modifier
                            .align(Alignment.TopStart)
                            .padding(16.dp)
                    )

                    if (uiState == UIState.CALIBRATING || uiState == UIState.PROCESSING) {
                        LinearProgressIndicator(
                            progress = { sweepProgress.coerceIn(0f, 1f) },
                            modifier = Modifier
                                .align(Alignment.BottomCenter)
                                .fillMaxWidth()
                                .padding(horizontal = 16.dp, vertical = 32.dp),
                            color = Color(0xFFFFB300),
                            trackColor = Color(0xFF333333)
                        )
                    }

                    if (uiState == UIState.SWEEPING) {
                        Text(
                            text = "● REC",
                            color = Color(0xFFFF1744),
                            fontFamily = FontFamily.Monospace,
                            fontWeight = FontWeight.Bold,
                            fontSize = 14.sp,
                            modifier = Modifier
                                .align(Alignment.TopEnd)
                                .padding(16.dp)
                        )
                    }

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
                            scope.launch(Dispatchers.IO) {
                                NativeEngine.runCalibration()
                            }
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
                                    scope.launch(Dispatchers.IO) {
                                        NativeEngine.startSweep()
                                    }
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
                    }

                    val showSaveUI = uiState != UIState.IDLE && uiState != UIState.EXPORTING && uiState != UIState.ERROR

                    if (showSaveUI) {
                        OutlinedTextField(
                            value = irFilename,
                            onValueChange = { irFilename = it },
                            label = { Text("Filename") },
                            singleLine = true,
                            modifier = Modifier.fillMaxWidth(),
                            colors = OutlinedTextFieldDefaults.colors(
                                focusedBorderColor = Color(0xFFFF4500),
                                unfocusedBorderColor = Color(0xFF606060),
                                focusedLabelColor = Color(0xFFFF4500)
                            )
                        )

                        Row(
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(vertical = 8.dp),
                            horizontalArrangement = Arrangement.spacedBy(8.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Button(
                                onClick = { folderPickerLauncher.launch(null) },
                                modifier = Modifier.weight(1f),
                                colors = ButtonDefaults.buttonColors(
                                    containerColor = Color(0xFF424242)
                                )
                            ) {
                                Text(
                                    text = chosenFolderUri?.let {
                                        DocumentFile.fromTreeUri(ctx, it)?.name ?: it.lastPathSegment
                                    } ?: "Choose folder…",
                                    fontFamily = FontFamily.Monospace,
                                    fontSize = 12.sp,
                                    maxLines = 1
                                )
                            }
                        }
                    } else if (irFilename.isBlank()) {
                        Text(
                            text = "Available after capture",
                            color = Color(0xFF505050),
                            fontFamily = FontFamily.Monospace,
                            fontSize = 12.sp
                        )
                    }

                    if (uiState == UIState.REVIEW && irSampleCount > 0) {
                        val sanitizedFilename = irFilename.filter { c -> c.isLetterOrDigit() || c == '-' || c == '_' }.take(63).let { if (it.isBlank()) "impulse" else it } + ".wav"

                        Button(
                            onClick = {
                                val cacheDir = ctx.cacheDir
                                val tempFile = java.io.File(cacheDir, sanitizedFilename)
                                val tempPath = tempFile.absolutePath
                                val exportSuccess = NativeEngine.export(tempPath)

                                if (!exportSuccess) {
                                    showExportSuccess = "Export failed"
                                    return@Button
                                }

                                if (chosenFolderUri != null) {
                                    val tree = DocumentFile.fromTreeUri(ctx, chosenFolderUri!!)
                                    val existing = tree?.findFile(sanitizedFilename)
                                    existing?.delete()
                                    val newFile = tree?.createFile("audio/wav", sanitizedFilename)
                                    if (newFile != null) {
                                        ctx.contentResolver.openOutputStream(newFile.uri)?.use { out ->
                                            tempFile.inputStream().use { input -> input.copyTo(out) }
                                        }
                                        tempFile.delete()
                                        val folderName = DocumentFile.fromTreeUri(ctx, chosenFolderUri!!)?.name ?: chosenFolderUri!!.lastPathSegment
                                        showExportSuccess = "Saved: $folderName/$sanitizedFilename"
                                    } else {
                                        showExportSuccess = "Failed to create file in folder"
                                    }
                                } else {
                                    showExportSuccess = "Saved: ${tempFile.absolutePath}"
                                }
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
                            UIState.SWEEPING -> "Recording sweep response..."
                            UIState.PROCESSING -> "Deconvolving IR..."
                            UIState.REVIEW -> if (irSampleCount > 0) "IR: $irSampleCount samples" else "IR captured"
                            UIState.ERROR -> "Error - check logs"
                            UIState.EXPORTING -> "Exporting..."
                        },
                        color = when (uiState) {
                            UIState.SWEEPING -> Color(0xFFFF1744)
                            UIState.PROCESSING, UIState.CALIBRATING -> Color(0xFFFFB300)
                            else -> Color(0xFF808080)
                        },
                        fontFamily = FontFamily.Monospace,
                        fontSize = 12.sp,
                        fontWeight = if (uiState == UIState.SWEEPING || uiState == UIState.PROCESSING || uiState == UIState.CALIBRATING) FontWeight.Bold else FontWeight.Normal
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
        audioLostReceiver?.let { unregisterReceiver(it) }
        audioLostReceiver = null
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
    // Peak hold state with slow decay
    val peakData = remember { FloatArray(64) { 0f } }

    // Update peaks with decay
    LaunchedEffect(data) {
        if (data.isNotEmpty()) {
            for (i in data.indices) {
                peakData[i] = maxOf(peakData[i] * 0.95f, data[i])
            }
        }
    }

    Canvas(modifier = modifier) {
        val barCount = 64
        val barWidth = size.width / barCount
        val maxHeight = size.height * 0.9f
        val sampleRate = 48000

        // Log-spaced frequency interpolation
        val logData = FloatArray(barCount)
        if (data.isNotEmpty()) {
            val fftSize = data.size
            for (i in 0 until barCount) {
                // Log-spaced center frequency: 20Hz to 20kHz
                val freq = (20.0 * Math.pow(10.0, i.toDouble() / 10.0)).toFloat()
                // Map frequency to FFT bin index
                val binIndex = (freq * fftSize * 2f / sampleRate).toInt()
                // Interpolate from nearest bins
                logData[i] = when {
                    binIndex < 0 -> data[0]
                    binIndex >= fftSize - 1 -> data[fftSize - 1]
                    else -> {
                        val frac: Float = (freq * fftSize * 2f / sampleRate) - binIndex.toFloat()
                        data[binIndex] * (1f - frac) + data[binIndex + 1] * frac
                    }
                }
            }
        }

        // Draw smooth filled path
        val path = Path()
        if (logData.isNotEmpty()) {
            path.moveTo(0f, size.height)
            logData.forEachIndexed { index, value ->
                val x = index * barWidth
                val y = size.height - (value * maxHeight)
                path.lineTo(x, y)
            }
            path.lineTo(size.width, size.height)
            path.close()
        }

        // Main filled area
        drawPath(
            path = path,
            color = Color(0xFF00BFFF)
        )

        // Peak hold line
        if (logData.isNotEmpty()) {
            val peakPath = Path()
            peakPath.moveTo(0f, size.height - peakData[0] * maxHeight)
            logData.indices.forEach { i ->
                val x = i * barWidth
                val y = size.height - (peakData[i] * maxHeight)
                peakPath.lineTo(x, y)
            }
            drawPath(
                path = peakPath,
                color = Color(0xFFFFD700)
            )
        }

        // Grid lines
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

@Composable
fun SpectrogramVisualizer(
    history: Array<FloatArray>,
    modifier: Modifier = Modifier,
    rows: Int = 32
) {
    Canvas(modifier = modifier) {
        val barWidth = size.width / 64
        val rowHeight = size.height / rows

        history.forEachIndexed { rowIdx, row ->
            row.forEachIndexed { binIdx, value ->
                val x = binIdx * barWidth
                val y = rowIdx * rowHeight

                val intensity = value.coerceIn(0f, 1f)
                val color = when {
                    intensity < 0.2f -> Color(0xFF001F3F)
                    intensity < 0.4f -> Color(0xFF0074D9)
                    intensity < 0.6f -> Color(0xFF00FF00)
                    intensity < 0.8f -> Color(0xFFFFEB00)
                    else -> Color(0xFFFF4500)
                }

                drawRect(
                    color = color,
                    topLeft = Offset(x, y),
                    size = Size(barWidth - 1, rowHeight)
                )
            }
        }
    }
}
