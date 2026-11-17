/*
 * WLC TV - Copyright (C) 2026 Josh Wilkinson
 * GPL-2.0-or-later, like WLC and VLC.
 */
package io.github.joshwilk.wlctv.core;

import java.util.List;
import java.util.Map;

/** What the cast receiver drives. The Android app implements it with libVLC. */
public interface Player {
    /** Start playing url from position seconds; queue holds {url,title} maps to play after it. */
    void load(String url, String title, double position, List<Map<String, Object>> queue, String from);

    /** play, resume, pause, toggle, stop, seek, seekrel, volume, next, prev. */
    void control(String cmd, double value);

    /** state, time, length, volume, title, index, count (WilkCast status fields). */
    Map<String, Object> status();
}
