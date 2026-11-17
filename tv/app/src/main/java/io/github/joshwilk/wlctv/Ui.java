/*
 * WLC TV - Copyright (C) 2026 Josh Wilkinson
 * GPL-2.0-or-later, like WLC and VLC.
 */
package io.github.joshwilk.wlctv;

import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.TextView;

/** The WLC look on a TV: dark steel, construction orange, big focusable targets. */
final class Ui {
    static final int BG = 0xFF15171B, CARD = 0xFF22252B, ORANGE = 0xFFFF6B00, ORANGE2 = 0xFFFF9440,
            TEXT = 0xFFEEF0F3, MUTE = 0xFF9AA3B2, LINE = 0xFF363B45, OK = 0xFF4CD08A, BAD = 0xFFFF5A5C;

    private Ui() { }

    static int dp(Context c, float v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, c.getResources().getDisplayMetrics());
    }

    static TextView text(Context c, String s, float sp, int color, boolean bold) {
        TextView t = new TextView(c);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    static GradientDrawable round(Context c, int fill, int stroke, float radiusDp) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(fill);
        g.setCornerRadius(dp(c, radiusDp));
        if (stroke != 0) g.setStroke(dp(c, 3), stroke);
        return g;
    }

    /** A remote-friendly button: grey at rest, orange outline + fill when focused. */
    static TextView button(Context c, String label, View.OnClickListener click) {
        TextView b = text(c, label, 20, TEXT, true);
        b.setFocusable(true);
        b.setClickable(true);
        b.setGravity(Gravity.CENTER_VERTICAL);
        b.setPadding(dp(c, 22), dp(c, 14), dp(c, 22), dp(c, 14));
        StateListDrawable s = new StateListDrawable();
        s.addState(new int[]{android.R.attr.state_focused}, round(c, 0xFF3A2414, ORANGE, 12));
        s.addState(new int[]{android.R.attr.state_pressed}, round(c, 0xFF3A2414, ORANGE, 12));
        s.addState(new int[]{}, round(c, CARD, LINE, 12));
        b.setBackground(s);
        b.setOnClickListener(click);
        b.setOnFocusChangeListener((v, f) -> ((TextView) v).setTextColor(f ? Color.WHITE : TEXT));
        return b;
    }

    static LinearLayout.LayoutParams wrap(Context c, int marginDp) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        p.setMargins(0, dp(c, marginDp), dp(c, marginDp), 0);
        return p;
    }

    static LinearLayout.LayoutParams fill(Context c, int marginDp) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        p.setMargins(0, dp(c, marginDp), 0, 0);
        return p;
    }

    static String clock(double secs) {
        long s = Math.max(0, (long) secs);
        long h = s / 3600, m = (s / 60) % 60, x = s % 60;
        return h > 0 ? String.format(java.util.Locale.ROOT, "%d:%02d:%02d", h, m, x)
                : String.format(java.util.Locale.ROOT, "%d:%02d", m, x);
    }
}
