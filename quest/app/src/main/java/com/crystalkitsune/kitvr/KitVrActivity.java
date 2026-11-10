package com.crystalkitsune.kitvr;

import android.app.NativeActivity;
import android.content.res.AssetFileDescriptor;
import android.media.AudioAttributes;
import android.media.MediaPlayer;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;

public final class KitVrActivity extends NativeActivity {
    private static final String WAIT_TRACK = "kitvr_waiting.mp3";
    private static final float WAIT_VOLUME = 0.18f;
    private static final long FADE_MS = 900;

    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private MediaPlayer waitPlayer;
    private boolean waitingForVideo;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        waitingForVideo = getIntent().getBooleanExtra("waiting_music", true);
        if (waitingForVideo) startWaitingMusic();
    }

    public void fadeWaitingMusic() {
        mainHandler.post(() -> {
            if (!waitingForVideo) return;
            waitingForVideo = false;
            MediaPlayer player = waitPlayer;
            if (player == null) return;

            long fadeStart = SystemClock.uptimeMillis();
            Runnable fade = new Runnable() {
                @Override
                public void run() {
                    if (waitPlayer != player) return;
                    float progress = Math.min(1.0f,
                            (float) (SystemClock.uptimeMillis() - fadeStart) / FADE_MS);
                    float volume = WAIT_VOLUME * (1.0f - progress);
                    player.setVolume(volume, volume);
                    if (progress >= 1.0f) {
                        releaseWaitingMusic();
                    } else {
                        mainHandler.postDelayed(this, 30);
                    }
                }
            };
            fade.run();
        });
    }

    private void startWaitingMusic() {
        try (AssetFileDescriptor asset = getAssets().openFd(WAIT_TRACK)) {
            MediaPlayer player = new MediaPlayer();
            waitPlayer = player;
            player.setAudioAttributes(new AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_GAME)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build());
            player.setLooping(true);
            player.setOnPreparedListener(prepared -> {
                if (waitingForVideo && waitPlayer == prepared) {
                    prepared.setVolume(WAIT_VOLUME, WAIT_VOLUME);
                    prepared.start();
                }
            });
            player.setOnErrorListener((failed, what, extra) -> {
                releaseWaitingMusic();
                return true;
            });
            player.setDataSource(asset.getFileDescriptor(), asset.getStartOffset(), asset.getLength());
            player.prepareAsync();
        } catch (Exception ignored) {
            releaseWaitingMusic();
        }
    }

    private void releaseWaitingMusic() {
        MediaPlayer player = waitPlayer;
        waitPlayer = null;
        if (player == null) return;
        try {
            if (player.isPlaying()) player.stop();
        } catch (IllegalStateException ignored) {
        }
        player.release();
    }

    @Override
    protected void onDestroy() {
        mainHandler.removeCallbacksAndMessages(null);
        releaseWaitingMusic();
        super.onDestroy();
    }
}
