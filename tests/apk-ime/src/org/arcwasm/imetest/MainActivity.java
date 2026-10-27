// arc-wasm: text fields for the page's keyboard (topic ios-audio-keyboard).
// A search field (single line, capital sentences, "search" action), a
// multi-line note and a number field, top to bottom; the bottom of the
// window is a label, so that adjustResize shows as a shorter content view.
// Every change is logged (tag ImeTest), for tests that read the page's log.
package org.arcwasm.imetest;

import android.app.Activity;
import android.graphics.Color;
import android.os.Bundle;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.view.ViewTreeObserver;
import android.view.inputmethod.EditorInfo;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;

public class MainActivity extends Activity {
    private static final String TAG = "ImeTest";
    private int mLastHeight;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        final LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.WHITE);
        root.addView(field("search", InputType.TYPE_CLASS_TEXT
                | InputType.TYPE_TEXT_FLAG_CAP_SENTENCES, EditorInfo.IME_ACTION_SEARCH, true));
        root.addView(field("note", InputType.TYPE_CLASS_TEXT
                | InputType.TYPE_TEXT_FLAG_MULTI_LINE | InputType.TYPE_TEXT_FLAG_CAP_SENTENCES,
                EditorInfo.IME_ACTION_UNSPECIFIED, false));
        root.addView(field("number", InputType.TYPE_CLASS_NUMBER, EditorInfo.IME_ACTION_DONE,
                true));
        View space = new View(this);
        root.addView(space, new LinearLayout.LayoutParams(-1, 0, 1f));
        TextView bottom = new TextView(this);
        bottom.setText("bottom");
        bottom.setGravity(Gravity.CENTER);
        bottom.setBackgroundColor(0xff3060c0);
        bottom.setTextColor(Color.WHITE);
        root.addView(bottom, new LinearLayout.LayoutParams(-1, 80));
        setContentView(root);
        root.getViewTreeObserver().addOnGlobalLayoutListener(
                new ViewTreeObserver.OnGlobalLayoutListener() {
                    @Override public void onGlobalLayout() {
                        int h = root.getHeight();
                        if (h != mLastHeight) {
                            mLastHeight = h;
                            Log.i(TAG, "layout height=" + h);
                        }
                    }
                });
        Log.i(TAG, "ready");
    }

    private EditText field(final String name, int inputType, int action, boolean singleLine) {
        final EditText e = new EditText(this);
        e.setHint(name);
        e.setTextSize(20);
        e.setInputType(inputType);
        e.setImeOptions(action);
        if (singleLine) {
            e.setSingleLine(true);
        } else {
            e.setMinLines(3);
        }
        e.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
            @Override public void onTextChanged(CharSequence s, int a, int b, int c) {}
            @Override public void afterTextChanged(Editable s) {
                Log.i(TAG, "text " + name + "=[" + s.toString().replace("\n", "\\n") + "]");
            }
        });
        e.setOnEditorActionListener(new TextView.OnEditorActionListener() {
            @Override public boolean onEditorAction(TextView v, int actionId,
                    android.view.KeyEvent event) {
                Log.i(TAG, "action " + name + " " + actionId + " [" + v.getText() + "]");
                return false;
            }
        });
        e.setOnFocusChangeListener(new View.OnFocusChangeListener() {
            @Override public void onFocusChange(View v, boolean hasFocus) {
                if (hasFocus) {
                    Log.i(TAG, "focus " + name);
                }
            }
        });
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
        lp.setMargins(8, 8, 8, 8);
        e.setLayoutParams(lp);
        return e;
    }
}
