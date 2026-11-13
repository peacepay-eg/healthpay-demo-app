package eg.healthpay.demo;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.provider.Telephony;
import android.telephony.SmsMessage;

import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;

/** Wakes on every incoming SMS; keeps only the wallet's "money received" messages and sends them on. */
public class SmsReceiver extends BroadcastReceiver {

    @Override
    public void onReceive(Context context, Intent intent) {
        if (!Telephony.Sms.Intents.SMS_RECEIVED_ACTION.equals(intent.getAction())) return;
        final Context c = context.getApplicationContext();
        if (!Prefs.listener(c)) return;

        SmsMessage[] parts = Telephony.Sms.Intents.getMessagesFromIntent(intent);
        if (parts == null || parts.length == 0) return;

        // A long SMS arrives in several parts: join them per sender.
        Map<String, StringBuilder> bySender = new LinkedHashMap<>();
        for (SmsMessage m : parts) {
            if (m == null) continue;
            String from = m.getDisplayOriginatingAddress();
            if (from == null) from = "";
            StringBuilder sb = bySender.get(from);
            if (sb == null) { sb = new StringBuilder(); bySender.put(from, sb); }
            String body = m.getDisplayMessageBody();
            if (body != null) sb.append(body);
        }

        int kept = 0;
        for (Map.Entry<String, StringBuilder> e : bySender.entrySet()) {
            if (wanted(c, e.getKey())) {
                Uploader.enqueue(c, e.getKey(), e.getValue().toString());
                kept++;
            }
        }
        if (kept == 0) return; // personal messages are never read further, stored or sent

        final PendingResult pr = goAsync();
        new Thread(() -> {
            try { Uploader.flush(c); } finally { pr.finish(); }
        }).start();
    }

    /** Only senders in the list (for example "OrangeCash"); an empty list would send nothing. */
    static boolean wanted(Context c, String sender) {
        String s = sender == null ? "" : sender.toLowerCase(Locale.ROOT).replace(" ", "");
        for (String w : Prefs.senders(c).split(",")) {
            String t = w.trim().toLowerCase(Locale.ROOT).replace(" ", "");
            if (!t.isEmpty() && s.contains(t)) return true;
        }
        return false;
    }
}
