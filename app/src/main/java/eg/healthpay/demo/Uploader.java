package eg.healthpay.demo;

import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.BatteryManager;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * Sends the wallet's SMS to the demo server, exactly as the MacroDroid rule did:
 * POST {server}?key=…&type=sms&device=box&sender=… with the SMS text as the body.
 * Messages wait in a queue on the phone until the server has them, so nothing is lost offline.
 */
final class Uploader {
    private Uploader() {}

    private static final Object LOCK = new Object();
    private static final int MAX_QUEUE = 300;

    static void enqueue(Context c, String sender, String body) {
        synchronized (LOCK) {
            try {
                JSONArray q = queue(c);
                JSONObject o = new JSONObject();
                o.put("s", sender);
                o.put("b", body);
                o.put("t", System.currentTimeMillis());
                q.put(o);
                while (q.length() > MAX_QUEUE) q.remove(0);
                save(c, q);
            } catch (Exception ignored) { }
        }
    }

    static int queued(Context c) {
        synchronized (LOCK) { return queue(c).length(); }
    }

    /** Sends everything waiting, oldest first; stops at the first failure and keeps the rest. */
    static int flush(Context c) {
        synchronized (LOCK) {
            if (!Prefs.ready(c)) return 0;
            JSONArray q = queue(c);
            int sent = 0;
            while (q.length() > 0) {
                JSONObject o = q.optJSONObject(0);
                if (o == null) { q.remove(0); continue; }
                String url = Prefs.base(c) + "?key=" + enc(Prefs.key(c)) + "&type=sms&device=box&sender=" + enc(o.optString("s"));
                int code = post(url, o.optString("b"));
                if (code >= 200 && code < 400) {
                    q.remove(0);
                    sent++;
                    Prefs.setStatus(c, "Last SMS sent to the server at " + now() + ".");
                } else {
                    Prefs.setStatus(c, "Couldn't reach the server at " + now() + " (" + (code < 0 ? "no connection" : "error " + code) + "). " + q.length() + " waiting; retrying.");
                    break;
                }
            }
            save(c, q);
            return sent;
        }
    }

    /** Tells the server the listener is alive, with the battery level, as MacroDroid's "Alive" rule did. */
    static boolean alive(Context c) {
        if (!Prefs.ready(c)) return false;
        String url = Prefs.base(c) + "?key=" + enc(Prefs.key(c)) + "&type=alive&device=box&battery=" + battery(c);
        int code = get(url);
        boolean ok = code >= 200 && code < 400;
        if (ok && queued(c) == 0) Prefs.setStatus(c, "Online. Last check-in at " + now() + ".");
        return ok;
    }

    private static int battery(Context c) {
        try {
            Intent b = c.registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
            if (b == null) return -1;
            int level = b.getIntExtra(BatteryManager.EXTRA_LEVEL, -1), scale = b.getIntExtra(BatteryManager.EXTRA_SCALE, 100);
            return level < 0 ? -1 : Math.round(level * 100f / scale);
        } catch (Exception e) { return -1; }
    }

    // Apps Script runs the request, then answers with a redirect: a 3xx means it arrived.
    private static int post(String url, String body) {
        HttpURLConnection h = null;
        try {
            h = (HttpURLConnection) new URL(url).openConnection();
            h.setInstanceFollowRedirects(false);
            h.setConnectTimeout(15000);
            h.setReadTimeout(30000);
            h.setRequestMethod("POST");
            h.setDoOutput(true);
            h.setRequestProperty("Content-Type", "text/plain; charset=utf-8");
            byte[] b = body.getBytes(StandardCharsets.UTF_8);
            h.setFixedLengthStreamingMode(b.length);
            try (OutputStream os = h.getOutputStream()) { os.write(b); }
            return h.getResponseCode();
        } catch (Exception e) {
            return -1;
        } finally {
            if (h != null) h.disconnect();
        }
    }

    private static int get(String url) {
        HttpURLConnection h = null;
        try {
            h = (HttpURLConnection) new URL(url).openConnection();
            h.setInstanceFollowRedirects(false);
            h.setConnectTimeout(15000);
            h.setReadTimeout(30000);
            return h.getResponseCode();
        } catch (Exception e) {
            return -1;
        } finally {
            if (h != null) h.disconnect();
        }
    }

    private static JSONArray queue(Context c) {
        try { return new JSONArray(Prefs.sp(c).getString("queue", "[]")); }
        catch (Exception e) { return new JSONArray(); }
    }

    private static void save(Context c, JSONArray q) {
        Prefs.sp(c).edit().putString("queue", q.toString()).commit();
    }

    static String enc(String s) {
        try { return URLEncoder.encode(s == null ? "" : s, "UTF-8"); }
        catch (Exception e) { return ""; }
    }

    private static String now() {
        return new SimpleDateFormat("HH:mm", Locale.US).format(new Date());
    }
}
