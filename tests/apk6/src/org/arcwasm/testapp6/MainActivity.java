package org.arcwasm.testapp6;

import android.Manifest;
import android.app.Activity;
import android.app.ActivityManager;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.content.res.ColorStateList;
import android.graphics.drawable.RippleDrawable;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.view.ViewOutlineProvider;
import android.view.ViewTreeObserver;
import android.widget.Button;
import android.widget.TextView;

/**
 * The launcher activity of the API 23 test app: Theme.Material, elevation
 * and outlines, a RippleDrawable button that counts taps, a runtime
 * permission request, a result from another activity, API 23 calls
 * (Context.getColor, getSystemService(Class), ConnectivityManager.getActiveNetwork).
 * Prints "MainActivity: ready button=X,Y" (the button's centre on the
 * display) once drawn and "MainActivity: clicks=N" per tap, for the page's
 * self-test (fw6-graphics-web/app.html).
 */
public class MainActivity extends Activity {
    public static final int REQUEST_PERMISSIONS = 7;
    public static final int REQUEST_RESULT = 42;

    private int mClicks;
    private TextView mCounter;
    private TextView mPermission;
    private Button mButton;
    private boolean mReadyReported;
    public String network;
    public int statusBarColor;

    private final BroadcastReceiver mReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            App6.event("MainActivity.onReceive " + intent.getAction() + " "
                    + intent.getStringExtra("text"));
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        App6.event("MainActivity.onCreate");
        App6.LIVE.put("MainActivity", this);
        setContentView(R.layout.main);
        ((TextView) findViewById(R.id.hello)).setText(
                getString(R.string.hello, Build.VERSION.SDK_INT) + " (" + getString(R.string.sdk_note) + ")");
        mCounter = (TextView) findViewById(R.id.counter);
        mPermission = (TextView) findViewById(R.id.permission);
        mButton = (Button) findViewById(R.id.button);
        updateCounter();
        updatePermission();

        // API 21: outlines, translationZ, status bar colour, a RippleDrawable in code.
        View card = findViewById(R.id.card);
        card.setOutlineProvider(ViewOutlineProvider.BOUNDS);
        card.setClipToOutline(true);
        card.setTranslationZ(2);
        Button result = (Button) findViewById(R.id.material_button);
        result.setBackground(new RippleDrawable(ColorStateList.valueOf(getColor(R.color.accent)),
                getDrawable(android.R.drawable.btn_default), null));
        getWindow().setStatusBarColor(getColor(R.color.primary_dark));
        statusBarColor = getWindow().getStatusBarColor();
        setTaskDescription(new ActivityManager.TaskDescription(getString(R.string.app_name),
                null, getColor(R.color.primary)));

        // API 23: getSystemService(Class), the active Network.
        ConnectivityManager cm = getSystemService(ConnectivityManager.class);
        Network n = cm.getActiveNetwork();
        NetworkCapabilities nc = n == null ? null : cm.getNetworkCapabilities(n);
        network = n == null ? "none" : nc != null
                && nc.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) ? "wifi" : "other";

        mButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                mClicks++;
                updateCounter();
                App6.event("MainActivity.onClick " + mClicks);
                System.out.println("MainActivity: clicks=" + mClicks);
            }
        });
        result.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startActivityForResult(new Intent(MainActivity.this, ResultActivity.class)
                        .putExtra("question", 6 * 7), REQUEST_RESULT);
            }
        });
        // Once laid out and drawn: where the button is, for the page's taps.
        final View decor = getWindow().getDecorView();
        decor.getViewTreeObserver().addOnPreDrawListener(new ViewTreeObserver.OnPreDrawListener() {
            @Override
            public boolean onPreDraw() {
                if (!mReadyReported && mButton.getWidth() > 0) {
                    mReadyReported = true;
                    decor.post(new Runnable() {
                        public void run() {
                            int[] xy = new int[2];
                            mButton.getLocationOnScreen(xy);
                            System.out.println("MainActivity: ready button="
                                    + (xy[0] + mButton.getWidth() / 2) + ","
                                    + (xy[1] + mButton.getHeight() / 2)
                                    + " network=" + network);
                        }
                    });
                }
                return true;
            }
        });
        registerReceiver(mReceiver, new IntentFilter("org.arcwasm.testapp6.PING"));
        if (savedInstanceState == null) {
            // Runtime permissions (API 23), as an app asks on first start.
            requestPermissions(new String[] {Manifest.permission.CAMERA,
                    Manifest.permission.WRITE_EXTERNAL_STORAGE}, REQUEST_PERMISSIONS);
        }
    }

    private void updateCounter() {
        mCounter.setText(getString(R.string.clicks, mClicks));
    }

    private void updatePermission() {
        mPermission.setText(getString(R.string.permission,
                checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
                        ? "granted" : "denied"));
    }

    public int getClicks() {
        return mClicks;
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions,
            int[] grantResults) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < permissions.length; i++) {
            String p = permissions[i];
            sb.append(' ').append(p.substring(p.lastIndexOf('.') + 1)).append('=')
                    .append(grantResults[i]);
        }
        App6.event("MainActivity.onRequestPermissionsResult " + requestCode + sb);
        updatePermission();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        App6.event("MainActivity.onActivityResult " + requestCode + " " + resultCode + " "
                + (data == null ? null : data.getIntExtra("answer", -1)));
    }

    @Override
    protected void onStart() {
        super.onStart();
        App6.event("MainActivity.onStart");
    }

    @Override
    protected void onResume() {
        super.onResume();
        App6.event("MainActivity.onResume");
    }

    @Override
    protected void onPause() {
        App6.event("MainActivity.onPause");
        super.onPause();
    }

    @Override
    protected void onStop() {
        App6.event("MainActivity.onStop");
        super.onStop();
    }

    @Override
    protected void onRestart() {
        super.onRestart();
        App6.event("MainActivity.onRestart");
    }

    @Override
    protected void onDestroy() {
        App6.event("MainActivity.onDestroy");
        unregisterReceiver(mReceiver);
        App6.LIVE.remove("MainActivity");
        super.onDestroy();
    }
}
