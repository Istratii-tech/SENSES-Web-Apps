package org.arcwasm.mediademo;

import android.app.Activity;
import android.graphics.Color;
import android.media.MediaPlayer;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.util.Log;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.TextView;
import android.widget.VideoView;

/**
 * A VideoView playing the app's own clip (res/raw/demo.mp4, 640x360 H.264 +
 * AAC) in a loop: VideoView -> MediaPlayer (in process, MediaCodec on the
 * browser's WebCodecs, topic mediacodec) -> the SurfaceView's layer under the
 * window. A tap pauses or resumes. Prints, for the page and for checking it
 * without looking: "MediaDemo: prepared WxH duration=MS", "MediaDemo:
 * rendering", then "MediaDemo: position=MS playing=B" every second.
 */
public class MainActivity extends Activity {
    private static final String TAG = "MediaDemo";
    private VideoView mVideo;
    private TextView mStatus;
    private MediaPlayer mPlayer;
    private final Handler mHandler = new Handler();

    private final Runnable mTick = new Runnable() {
        public void run() {
            if (mPlayer != null) {
                int pos = mVideo.getCurrentPosition();
                boolean playing = mVideo.isPlaying();
                System.out.println("MediaDemo: position=" + pos + " playing=" + playing);
                mStatus.setText(String.format("MediaPlayer + MediaCodec (WebCodecs)  %d.%d s / %d.%d s%s",
                        pos / 1000, pos / 100 % 10, mVideo.getDuration() / 1000,
                        mVideo.getDuration() / 100 % 10, playing ? "" : "  (paused)"));
            }
            mHandler.postDelayed(this, 1000);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        mVideo = new VideoView(this);
        root.addView(mVideo, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.CENTER));
        mStatus = new TextView(this);
        mStatus.setTextColor(0xffe2e8f0);
        mStatus.setBackgroundColor(0x99000000);
        mStatus.setPadding(16, 8, 16, 8);
        mStatus.setTextSize(13);
        mStatus.setText("MediaPlayer: preparing...");
        root.addView(mStatus, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM));
        setContentView(root);

        mVideo.setOnPreparedListener(new MediaPlayer.OnPreparedListener() {
            public void onPrepared(MediaPlayer mp) {
                mPlayer = mp;
                mp.setLooping(true);
                System.out.println("MediaDemo: prepared " + mp.getVideoWidth() + "x"
                        + mp.getVideoHeight() + " duration=" + mp.getDuration());
                mp.setOnInfoListener(new MediaPlayer.OnInfoListener() {
                    public boolean onInfo(MediaPlayer m, int what, int extra) {
                        if (what == MediaPlayer.MEDIA_INFO_VIDEO_RENDERING_START)
                            System.out.println("MediaDemo: rendering");
                        return false;
                    }
                });
                mVideo.start();
            }
        });
        mVideo.setOnErrorListener(new MediaPlayer.OnErrorListener() {
            public boolean onError(MediaPlayer mp, int what, int extra) {
                System.out.println("MediaDemo: error " + what + "/" + extra);
                mStatus.setText("MediaPlayer error " + what + "/" + extra);
                return true;
            }
        });
        mVideo.setOnTouchListener(new View.OnTouchListener() {
            public boolean onTouch(View v, MotionEvent e) {
                if (e.getAction() == MotionEvent.ACTION_UP) {
                    if (mVideo.isPlaying()) mVideo.pause();
                    else mVideo.start();
                    Log.i(TAG, mVideo.isPlaying() ? "resumed" : "paused");
                }
                return true;
            }
        });
        mVideo.setVideoURI(Uri.parse("android.resource://" + getPackageName() + "/" + R.raw.demo));
        mHandler.postDelayed(mTick, 1000);
    }
}
