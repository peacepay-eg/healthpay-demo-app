# HealthPay Demo app (Android)

Demonstration only, for the HealthPay counter-payments demo (Google Apps Script server). Demo wallets only.

**What it does**
1. **Connect** once: paste the admin page's link (`…/exec?key=…`) and the shop (A).
2. **Open a screen** inside the app: Cashier, Owner and reports, Soundbox (speaks with the phone's Arabic voice, screen stays on), Admin and simulator.
3. **Listener**: on the phone holding the wallet SIM, passes SMS from the listed senders (default `OrangeCash`) to the server, exactly as the MacroDroid rule did (`POST …/exec?key=…&type=sms&device=box&sender=…`, the SMS text as the body). Messages wait on the phone until the server has them. A check-in every 5 minutes reports the battery (`type=alive`). Restarts after the phone restarts.

**Build**: GitHub Actions (`.github/workflows/build-apk.yml`) builds a debug APK on every push; download it from the run's artifacts. Locally: Android Studio, or `gradle assembleDebug` with Gradle 8.7 and JDK 17.

**Notes**
- Android 8 or later. Install by allowing "install unknown apps".
- Turn off the MacroDroid rule when the listener is on, or each payment is sent twice.
- The server needs v0.3.8 or later for the soundbox to use the phone's voice.
- Not for the Play Store: reading SMS needs Google's approval there.
