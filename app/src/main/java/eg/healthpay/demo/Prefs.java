package eg.healthpay.demo;

import android.content.Context;
import android.content.SharedPreferences;
import android.net.Uri;

/** Everything the app remembers: the demo server's address and key, the shop, and the listener settings. */
final class Prefs {
    private Prefs() {}

    static SharedPreferences sp(Context c) {
        return c.getSharedPreferences("hp", Context.MODE_PRIVATE);
    }

    static String base(Context c) { return sp(c).getString("base", ""); }
    static String key(Context c) { return sp(c).getString("key", ""); }
    static String shop(Context c) { return sp(c).getString("shop", "A"); }
    static boolean listener(Context c) { return sp(c).getBoolean("listener", false); }
    static String senders(Context c) { return sp(c).getString("senders", "OrangeCash"); }
    static String status(Context c) { return sp(c).getString("status", "Nothing sent yet."); }

    static void setStatus(Context c, String s) { sp(c).edit().putString("status", s).apply(); }

    static boolean ready(Context c) { return !base(c).isEmpty() && !key(c).isEmpty(); }

    /**
     * Accepts any page link of the demo server, for example the admin page's address
     * ".../macros/s/AKfy.../exec?key=...". Returns null when it isn't a demo server link.
     */
    static String[] parseLink(String link) {
        if (link == null) return null;
        String t = link.trim();
        int at = t.indexOf("/exec");
        if (!t.startsWith("https://") || at < 0) return null;
        String base = t.substring(0, at + 5);
        String key = "";
        try {
            Uri u = Uri.parse(t);
            String k = u.getQueryParameter("key");
            if (k != null) key = k.trim();
        } catch (Exception ignored) { }
        return new String[] { base, key };
    }

    /** Masks the key for display: the first three characters, then "…". */
    static String masked(String key) {
        if (key == null || key.isEmpty()) return "(no key)";
        return (key.length() <= 3 ? key : key.substring(0, 3)) + "…";
    }
}
