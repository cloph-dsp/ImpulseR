package com.impulser.capture

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.util.Log
import androidx.core.app.NotificationCompat

class AudioCaptureForegroundService : Service() {
    private val TAG = "AudioCaptureService"

    companion object {
        private const val CHANNEL_ID = "impulser_capture_channel"
        private const val NOTIFICATION_ID = 1
        const val ACTION_AUDIO_LOST = "com.impulser.capture.AUDIO_LOST"

        fun startService(context: Context) {
            val intent = Intent(context, AudioCaptureForegroundService::class.java)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                context.startForegroundService(intent)
            } else {
                context.startService(intent)
            }
        }

        fun stopService(context: Context) {
            val intent = Intent(context, AudioCaptureForegroundService::class.java)
            context.stopService(intent)
        }
    }

    private lateinit var audioManager: AudioManager
    private var focusRequest: AudioFocusRequest? = null

    private val focusChangeListener = AudioManager.OnAudioFocusChangeListener { focusChange ->
        when (focusChange) {
            AudioManager.AUDIOFOCUS_LOSS,
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT -> {
                Log.i(TAG, "Audio focus lost: $focusChange")
                sendAudioLostBroadcast()
            }
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK -> {
                // Ducks audio — no interrupt needed
            }
            AudioManager.AUDIOFOCUS_GAIN -> {
                // Resume if needed — not needed for capture pause/resume
            }
        }
    }

    private val routingChangedListener = AudioManager.OnRoutingChangedListener { _, _ ->
        Log.i(TAG, "Audio routing changed")
        sendAudioLostBroadcast()
    }

    private fun sendAudioLostBroadcast() {
        val intent = Intent(ACTION_AUDIO_LOST)
        sendBroadcast(intent)
    }

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
        audioManager = getSystemService(Context.AUDIO_SERVICE) as AudioManager

        val focusGain = AudioManager.AUDIOFOCUS_GAIN_TRANSIENT_EXCLUSIVE
        val usage = AudioManager.USAGE_MEDIA
        focusRequest = AudioFocusRequest.Builder(focusGain)
            .setUsage(usage)
            .build()

        audioManager.registerAudioRoutingCallback(routingChangedListener, Handler(android.os.Looper.getMainLooper()))
    }
    
    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        focusRequest?.let { audioManager.requestAudioFocus(it) }
        val notification = createNotification("Capturing audio...")
        startForeground(NOTIFICATION_ID, notification)
        return START_NOT_STICKY
    }
    
    override fun onBind(intent: Intent?): IBinder? = null
    
    override fun onDestroy() {
        super.onDestroy()
        focusRequest?.let { audioManager.abandonAudioFocusRequest(it) }
        audioManager.unregisterAudioRoutingCallback(routingChangedListener)
    }
    
    fun updateNotification(state: String) {
        val notification = createNotification(state)
        val notificationManager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        notificationManager.notify(NOTIFICATION_ID, notification)
    }
    
    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                "Audio Capture",
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = "Audio capture for impulse response measurement"
                setShowBadge(false)
            }
            
            val notificationManager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            notificationManager.createNotificationChannel(channel)
        }
    }
    
    private fun createNotification(text: String): Notification {
        val pendingIntent = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE
        )
        
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("ImpulseR")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.ic_btn_speak_now)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .setOngoing(true)
            .setContentIntent(pendingIntent)
            .build()
    }
}
