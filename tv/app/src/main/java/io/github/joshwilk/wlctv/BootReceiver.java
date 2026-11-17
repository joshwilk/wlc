/*
 * WLC TV - Copyright (C) 2026 Josh Wilkinson
 * GPL-2.0-or-later, like WLC and VLC.
 */
package io.github.joshwilk.wlctv;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** Starts the receiver when the TV boots, so it can be cast to straight away. */
public class BootReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context c, Intent i) {
        if (Intent.ACTION_BOOT_COMPLETED.equals(i.getAction()) && App.prefs().bootStart()) {
            try { App.startReceiver(c); } catch (RuntimeException ignored) { }
        }
    }
}
