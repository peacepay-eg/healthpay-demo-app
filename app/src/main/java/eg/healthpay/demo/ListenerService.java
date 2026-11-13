package eg.healthpay.demo;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;

/**
 * Keeps the listener alive while it's switched on: a permanent notification (so Android doesn't stop it),
 * a check-in to the server every 5 minutes with the battery level, and a retry of anything still waiting.
 */
public class ListenerService extends Service {
    private static final String CHANNEL = "listener";
    private static final int NOTE_ID = 7;
    private static final long EVERY_MS = 5 * 60 * 1000L;

    private HandlerThread thread;
    private Handler handler;

    static void start(Context c) {
        Intent i = new Intent(c, ListenerService.class);
        if (Build.VERSION.SDK_INT >= 26) c.startForegroundService(i); else c.startService(i);
    }

    static void stop(Context c) {
        c.stopService(new Intent(c, ListenerService.class));
    }

    private final Runnable tick = new Runnable() {
        @Override public void run() {
            Context c = getApplicationContext();
            if (!Prefs.listener(c)) { stopSelf(); return; }
            Uploader.flush(c);
            boolean ok = Uploader.alive(c);
            updateNote(ok ? "Listening for " + Prefs.senders(c) + " messages" : "No connection to the server: retrying");
            handler.postDelayed(this, EVERY_MS);
        }
    };

    @Override
    public void onCreate() {
        super.onCreate();
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null && nm.getNotificationChannel(CHANNEL) == null) {
            NotificationChannel ch = new NotificationChannel(CHANNEL, "Payment listener", NotificationManager.IMPORTANCE_LOW);
            ch.setDescription("Shows that the phone is passing the wallet's payment SMS to HealthPay");
            nm.createNotificationChannel(ch);
        }
        thread = new HandlerThread("listener");
        thread.start();
        handler = new Handler(thread.getLooper());
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        Notification n = note("Starting…");
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(NOTE_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
        } else {
            startForeground(NOTE_ID, n);
        }
        handler.removeCallbacks(tick);
        handler.post(tick);
        return START_STICKY;
    }

    private Notification note(String text) {
        Intent open = new Intent(this, MainActivity.class);
        PendingIntent pi = PendingIntent.getActivity(this, 0, open, PendingIntent.FLAG_IMMUTABLE);
        return new Notification.Builder(this, CHANNEL)
                .setSmallIcon(R.drawable.ic_note)
                .setContentTitle("HealthPay listener is on")
                .setContentText(text)
                .setContentIntent(pi)
                .setOngoing(true)
                .build();
    }

    private void updateNote(String text) {
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null) {
            try { nm.notify(NOTE_ID, note(text)); } catch (Exception ignored) { }
        }
    }

    @Override
    public void onDestroy() {
        if (handler != null) handler.removeCallbacksAndMessages(null);
        if (thread != null) thread.quitSafely();
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }
}
