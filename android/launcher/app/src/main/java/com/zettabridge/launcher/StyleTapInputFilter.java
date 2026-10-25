package com.zettabridge.launcher;

import android.app.Activity;
import android.content.Context;
import android.text.InputFilter;
import android.text.Spanned;
import android.view.View;
import android.widget.TextView;

import java.util.LinkedHashMap;
import java.util.Map;

/**
 * StyleTap 0.8.0034 only: keeps characters its single-byte Palm text code cannot take away from
 * its hidden input field.
 *
 * StyleTap passes every character typed into DummyEditText straight to native enqueueKey. Its
 * SingleByteCodePage::TxtCharIsValid rejects 0x0100-0xFF7F, but SingleByteCodePage::TxtSetNextChar
 * stores the low byte of any character without checking. A Hangul syllable such as U+AC00 becomes
 * a NUL byte inside the field text, FrmDispatchEvent never returns, and the Palm screen stops
 * responding (Hangul jamo U+3131 becomes the digit "1"). This filter drops exactly that range,
 * and every code point above U+FFFF, before StyleTap sees them.
 *
 * It is a compatibility guard for the path verified above, not a statement of every character
 * StyleTap supports. Deletions are never blocked. On by default; the library screen can turn it
 * off to reproduce the original behaviour.
 */
final class StyleTapInputFilter implements InputFilter {
    static final String PACKAGE = "com.StyleTap.StyleTap";
    static final String INPUT_FIELD = "dummyEditField";
    private static final int MAX_NOTED = 16;

    private final Context context;
    private final Map<Integer, Integer> blocked = new LinkedHashMap<>();
    private int total;

    StyleTapInputFilter(Context context) {
        this.context = context;
    }

    /** True for code points StyleTap would store as a wrong single byte. */
    static boolean isBlocked(int codePoint) {
        return (codePoint >= 0x0100 && codePoint <= 0xFF7F) || codePoint > 0xFFFF;
    }

    /** The text with blocked code points removed, or null when nothing in it is blocked. */
    static String strip(CharSequence source, int start, int end, Map<Integer, Integer> counts) {
        StringBuilder kept = null;
        for (int i = start; i < end; ) {
            final int codePoint = Character.codePointAt(source, i);
            final int length = Character.charCount(codePoint);
            if (isBlocked(codePoint)) {
                if (kept == null) kept = new StringBuilder(source.subSequence(start, i));
                if (counts != null) counts.merge(codePoint, 1, Integer::sum);
            } else if (kept != null) {
                kept.append(source, i, Math.min(i + length, end));
            }
            i += length;
        }
        return kept == null ? null : kept.toString();
    }

    @Override
    public CharSequence filter(CharSequence source, int start, int end, Spanned dest, int dstart, int dend) {
        if (start >= end) return null;  // a deletion: never blocked
        final Map<Integer, Integer> counts = new LinkedHashMap<>();
        final String kept = strip(source, start, end, counts);
        if (kept == null) return null;
        for (Map.Entry<Integer, Integer> e : counts.entrySet()) {
            total += e.getValue();
            final boolean first = !blocked.containsKey(e.getKey());
            blocked.merge(e.getKey(), e.getValue(), Integer::sum);
            // One line per new code point, not per keystroke; only code points, never the text.
            if (first && blocked.size() <= MAX_NOTED) {
                Diagnostics.note(context, String.format(java.util.Locale.ROOT,
                        "input-filtered: U+%04X (first time) total=%d", e.getKey(), total));
            }
        }
        return kept;
    }

    /**
     * Called after the plugin activity's onCreate: attaches the filter to StyleTap's input field.
     * `host` is the launcher Application: notes must land in its files dir, which a plugin
     * context redirects.
     */
    static void attach(Activity activity, LoadedPlugin plugin, Context host) {
        if (plugin == null || !PACKAGE.equals(plugin.packageName)) return;
        if (plugin.record != null && !plugin.record.styletapInputFilter) {
            Diagnostics.note(host, "input-filter: off (disabled in the library screen)");
            return;
        }
        final int id = activity.getResources().getIdentifier(INPUT_FIELD, "id", plugin.packageName);
        final View view = id == 0 ? null : activity.findViewById(id);
        if (!(view instanceof TextView)) {
            Diagnostics.note(host, "input-filter: not attached (" + INPUT_FIELD + " not found)");
            return;
        }
        final TextView field = (TextView) view;
        final InputFilter[] old = field.getFilters();
        for (InputFilter f : old) {
            if (f instanceof StyleTapInputFilter) return;  // recreated activity, already attached
        }
        final InputFilter[] filters = new InputFilter[old.length + 1];
        System.arraycopy(old, 0, filters, 0, old.length);
        filters[old.length] = new StyleTapInputFilter(host);
        field.setFilters(filters);
        Diagnostics.note(host, "input-filter: on (blocks U+0100-U+FF7F and above U+FFFF)");
    }
}
