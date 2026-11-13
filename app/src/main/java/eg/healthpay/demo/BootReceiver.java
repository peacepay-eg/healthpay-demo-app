package eg.healthpay.demo;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** Restarts the listener after the phone restarts. */
public class BootReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        if (Intent.ACTION_BOOT_COMPLETED.equals(intent.getAction()) && Prefs.listener(context)) {
            ListenerService.start(context);
        }
    }
}
