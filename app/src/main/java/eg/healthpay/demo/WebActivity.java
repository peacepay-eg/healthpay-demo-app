package eg.healthpay.demo;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.speech.tts.TextToSpeech;
import android.view.WindowManager;
import android.webkit.CookieManager;
import android.webkit.JavascriptInterface;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import java.util.Locale;

/** Shows one page of the demo server (cashier, owner, soundbox or admin) inside the app. */
public class WebActivity extends Activity {
    static final String EXTRA_PAGE = "page";

    private WebView web;
    private TextToSpeech tts;
    private volatile boolean ttsReady = false;

    /** Lets the soundbox page speak with the phone's own Arabic voice. */
    public class Native {
        @JavascriptInterface public boolean canSpeak() { return ttsReady; }
        @JavascriptInterface public void speak(String text) {
            if (ttsReady && text != null) tts.speak(text, TextToSpeech.QUEUE_ADD, null, "hp" + System.nanoTime());
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        String page = getIntent().getStringExtra(EXTRA_PAGE);
        if (page == null) page = "pos";
        // The soundbox and the cashier screen (which can also speak) stay awake.
        if ("box".equals(page) || "pos".equals(page)) getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        tts = new TextToSpeech(this, status -> {
            if (status != TextToSpeech.SUCCESS) return;
            int r = tts.setLanguage(new Locale("ar", "EG"));
            if (r < 0) r = tts.setLanguage(new Locale("ar"));
            ttsReady = r >= 0;
        });

        web = new WebView(this);
        web.setBackgroundColor(Color.parseColor("#F4F2EC"));
        setContentView(web);

        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setMediaPlaybackRequiresUserGesture(false);
        s.setSupportMultipleWindows(false); // links that open a new tab open here instead
        s.setJavaScriptCanOpenWindowsAutomatically(true);
        CookieManager.getInstance().setAcceptCookie(true);
        CookieManager.getInstance().setAcceptThirdPartyCookies(web, true);

        web.addJavascriptInterface(new Native(), "HPNative");
        web.setWebViewClient(new WebViewClient() {
            @Override
            public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest req) {
                Uri u = req.getUrl();
                String host = u.getHost() == null ? "" : u.getHost();
                boolean google = host.endsWith("google.com") || host.endsWith("googleusercontent.com") || host.endsWith("gstatic.com");
                if ("https".equals(u.getScheme()) && google) return false; // stay in the app
                try { startActivity(new Intent(Intent.ACTION_VIEW, u)); } catch (Exception ignored) { }
                return true;
            }
        });

        String url = Prefs.base(this) + "?page=" + Uploader.enc(page) + "&shop=" + Uploader.enc(Prefs.shop(this)) + "&key=" + Uploader.enc(Prefs.key(this));
        web.loadUrl(url);
    }

    @Override
    public void onBackPressed() {
        if (web != null && web.canGoBack()) web.goBack(); else super.onBackPressed();
    }

    @Override
    protected void onDestroy() {
        if (tts != null) tts.shutdown();
        if (web != null) { web.removeJavascriptInterface("HPNative"); web.destroy(); }
        super.onDestroy();
    }
}
