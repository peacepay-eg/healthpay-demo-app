package eg.healthpay.demo;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.PowerManager;
import android.provider.Settings;
import android.text.InputType;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.util.ArrayList;
import java.util.List;

/**
 * The start screen: connect the app to the demo server once, open the cashier, owner or soundbox
 * screen, and switch this phone into the listener that replaces MacroDroid.
 */
public class MainActivity extends Activity {
    private static final int REQ_LISTENER = 1;
    private static final int GREEN = Color.parseColor("#0E6B52");
    private static final int INK = Color.parseColor("#17201E");
    private static final int MUTED = Color.parseColor("#5A6460");

    private EditText link, shop, senders;
    private TextView server, status;
    private CheckBox listener;
    private final List<Button> roleButtons = new ArrayList<>();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().setStatusBarColor(INK);

        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(20);
        col.setPadding(pad, pad, pad, pad);
        col.setBackgroundColor(Color.parseColor("#F4F2EC"));

        TextView brand = text("HEALTHPAY · DEMO", 12, MUTED, true);
        brand.setLetterSpacing(0.08f);
        col.addView(brand);
        col.addView(text("Counter payments", 24, INK, true));
        col.addView(space(16));

        // 1. Connect
        LinearLayout c1 = card(col);
        c1.addView(text("1. Connect to the demo server", 16, INK, true));
        c1.addView(text("Paste the admin page's link (it ends with /exec?key=…). You do this once.", 13, MUTED, false));
        link = field(c1, "https://script.google.com/macros/s/…/exec?key=…", false);
        link.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI | InputType.TYPE_TEXT_FLAG_MULTI_LINE);
        c1.addView(text("Shop", 13, MUTED, true));
        shop = field(c1, "A", true);
        shop.setText(Prefs.shop(this));
        Button save = button(c1, "Save", true);
        save.setOnClickListener(v -> saveLink());
        server = text("", 13, MUTED, false);
        c1.addView(server);

        // 2. Open a screen
        LinearLayout c2 = card(col);
        c2.addView(text("2. Open a screen", 16, INK, true));
        String[][] roles = { { "Cashier", "pos" }, { "Owner and reports", "owner" }, { "Soundbox", "box" }, { "Admin and simulator", "admin" } };
        for (String[] r : roles) {
            Button b = button(c2, r[0], r[1].equals("pos"));
            b.setOnClickListener(v -> openPage(r[1]));
            roleButtons.add(b);
        }

        // 3. Listener
        LinearLayout c3 = card(col);
        c3.addView(text("3. This phone as the listener", 16, INK, true));
        c3.addView(text("Use this on the phone that holds the wallet SIM. It passes the wallet's \"money received\" SMS to the server, instead of MacroDroid. Turn off the MacroDroid rule so payments aren't sent twice. Other messages are never read further or sent.", 13, MUTED, false));
        listener = new CheckBox(this);
        listener.setText("Pass the wallet's payment SMS to HealthPay");
        listener.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        listener.setTextColor(INK);
        listener.setChecked(Prefs.listener(this));
        listener.setOnClickListener(v -> toggleListener(listener.isChecked()));
        c3.addView(listener);
        c3.addView(text("Senders to pass on (comma-separated)", 13, MUTED, true));
        senders = field(c3, "OrangeCash", true);
        senders.setText(Prefs.senders(this));
        senders.setOnFocusChangeListener((v, has) -> { if (!has) saveSenders(); });
        status = text("", 13, MUTED, false);
        c3.addView(status);
        Button test = button(c3, "Check in with the server now", false);
        test.setOnClickListener(v -> checkIn());
        Button battery = button(c3, "Let it run in the background", false);
        battery.setOnClickListener(v -> askBattery());

        col.addView(text("Demonstration app · v0.1 · demo wallets only", 11, MUTED, false));

        ScrollView sv = new ScrollView(this);
        sv.addView(col);
        setContentView(sv);
        refresh();
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh();
    }

    private void refresh() {
        boolean ready = Prefs.ready(this);
        server.setText(ready ? "Connected: " + shortBase(Prefs.base(this)) + " · key " + Prefs.masked(Prefs.key(this)) + " · shop " + Prefs.shop(this)
                : "Not connected yet.");
        server.setTextColor(ready ? GREEN : Color.parseColor("#8F1F15"));
        for (Button b : roleButtons) b.setEnabled(ready);
        int q = Uploader.queued(this);
        status.setText((Prefs.listener(this) ? "On. " : "Off. ") + Prefs.status(this) + (q > 0 ? " " + q + " waiting to send." : ""));
    }

    private void saveLink() {
        String s = link.getText().toString();
        String sh = shop.getText().toString().trim();
        if (sh.isEmpty()) sh = "A";
        if (!s.trim().isEmpty()) {
            String[] p = Prefs.parseLink(s);
            if (p == null) { toast("That isn't a demo server link. Copy the admin page's address from the browser."); return; }
            if (p[1].isEmpty()) { toast("The link has no key in it. Copy the whole address, including ?key=…"); return; }
            Prefs.sp(this).edit().putString("base", p[0]).putString("key", p[1]).putString("shop", sh).apply();
            link.setText("");
        } else {
            Prefs.sp(this).edit().putString("shop", sh).apply();
        }
        toast(Prefs.ready(this) ? "Saved." : "Paste the link first.");
        refresh();
    }

    private void saveSenders() {
        String s = senders.getText().toString().trim();
        Prefs.sp(this).edit().putString("senders", s.isEmpty() ? "OrangeCash" : s).apply();
    }

    private void openPage(String page) {
        if (!Prefs.ready(this)) { toast("Connect to the demo server first."); return; }
        Intent i = new Intent(this, WebActivity.class);
        i.putExtra(WebActivity.EXTRA_PAGE, page);
        startActivity(i);
    }

    private void toggleListener(boolean on) {
        saveSenders();
        if (on) {
            if (!Prefs.ready(this)) { listener.setChecked(false); toast("Connect to the demo server first."); return; }
            List<String> need = new ArrayList<>();
            if (checkSelfPermission(Manifest.permission.RECEIVE_SMS) != PackageManager.PERMISSION_GRANTED) need.add(Manifest.permission.RECEIVE_SMS);
            if (Build.VERSION.SDK_INT >= 33 && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) need.add(Manifest.permission.POST_NOTIFICATIONS);
            if (!need.isEmpty()) { requestPermissions(need.toArray(new String[0]), REQ_LISTENER); return; }
            startListener();
        } else {
            Prefs.sp(this).edit().putBoolean("listener", false).apply();
            ListenerService.stop(this);
            refresh();
        }
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] perms, int[] results) {
        super.onRequestPermissionsResult(code, perms, results);
        if (code != REQ_LISTENER) return;
        if (checkSelfPermission(Manifest.permission.RECEIVE_SMS) == PackageManager.PERMISSION_GRANTED) startListener();
        else { listener.setChecked(false); toast("Without SMS permission the phone can't pass payments on."); refresh(); }
    }

    private void startListener() {
        Prefs.sp(this).edit().putBoolean("listener", true).apply();
        ListenerService.start(this);
        toast("Listener on. Send 1 EGP to the wallet to test it.");
        refresh();
    }

    private void checkIn() {
        if (!Prefs.ready(this)) { toast("Connect to the demo server first."); return; }
        toast("Checking…");
        new Thread(() -> {
            Uploader.flush(this);
            boolean ok = Uploader.alive(this);
            runOnUiThread(() -> { toast(ok ? "The server heard this phone." : "No answer from the server. Check the link and the internet."); refresh(); });
        }).start();
    }

    private void askBattery() {
        PowerManager pm = getSystemService(PowerManager.class);
        if (pm != null && pm.isIgnoringBatteryOptimizations(getPackageName())) {
            toast("Already allowed. On Xiaomi, Oppo or Tecno phones, also allow \"Autostart\" for this app in the phone's settings.");
            return;
        }
        try {
            Intent i = new Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS);
            i.setData(Uri.parse("package:" + getPackageName()));
            startActivity(i);
        } catch (Exception e) {
            startActivity(new Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS));
        }
    }

    // ---------- small helpers for the hand-built layout ----------

    private static String shortBase(String b) {
        int i = b.indexOf("/s/");
        if (i < 0) return b;
        String id = b.substring(i + 3);
        return "…/s/" + (id.length() > 6 ? id.substring(0, 6) + "…" : id);
    }

    private int dp(int v) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics()));
    }

    private View space(int h) {
        View v = new View(this);
        v.setLayoutParams(new LinearLayout.LayoutParams(1, dp(h)));
        return v;
    }

    private TextView text(String s, int sp, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        t.setPadding(0, dp(3), 0, dp(3));
        t.setLineSpacing(0, 1.15f);
        return t;
    }

    private LinearLayout card(LinearLayout parent) {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        c.setPadding(dp(16), dp(14), dp(16), dp(14));
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(Color.WHITE);
        bg.setCornerRadius(dp(16));
        c.setBackground(bg);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = dp(14);
        parent.addView(c, lp);
        return c;
    }

    private EditText field(LinearLayout parent, String hint, boolean single) {
        EditText e = new EditText(this);
        e.setHint(hint);
        e.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        e.setSingleLine(single);
        e.setMinHeight(dp(48));
        parent.addView(e, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        return e;
    }

    private Button button(LinearLayout parent, String label, boolean primary) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        b.setGravity(Gravity.CENTER);
        b.setMinHeight(dp(52));
        GradientDrawable bg = new GradientDrawable();
        bg.setCornerRadius(dp(12));
        if (primary) { bg.setColor(GREEN); b.setTextColor(Color.WHITE); }
        else { bg.setColor(Color.WHITE); bg.setStroke(dp(1), Color.parseColor("#C9C3B6")); b.setTextColor(INK); }
        b.setBackground(bg);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(8);
        parent.addView(b, lp);
        return b;
    }

    private void toast(String s) {
        Toast.makeText(this, s, Toast.LENGTH_LONG).show();
    }
}
