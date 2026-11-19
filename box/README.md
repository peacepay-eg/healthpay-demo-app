# HealthPay counter box (demo firmware)

Firmware for the ESP32 2.8" touch-screen boards ("Cheap Yellow Display" family).
The box shows a Meeza QR for each sale, registers the sale with the HealthPay
demo server, and says "received ... pounds" in Arabic when it is paid.

GitHub builds one file per board on every change under `box/` and puts them on
the `firmware` branch:

One file per board serves every merchant (v0.8.1). When the logged-in person's
shop is a restaurant or cafe (business type "Restaurant or cafe" on the server),
the box asks who served the table and offers a tip before the QR; for a shop it
goes straight from the amount to the QR.

Bills and recharges (v0.9.0, needs server v0.10.0): the **Menu** key (where
Today used to be) opens Bills and recharges, and Today's sales. The cashier
picks a service, types the account or mobile number on the keypad, sees the
amount due, and takes **Cash** (the bill is paid at once from the shop's wallet)
or **QR / card** (the usual payment screen; the bill is paid when the customer
pays). Demo services only: no real bill is paid.

| File | Board |
|---|---|
| HealthPayBox_2432S028R.bin | ESP32-2432S028R (ILI9341 screen) - try this first |
| HealthPayBox_2432S028_ST7789.bin | the same board with an ST7789 screen (screen stays white or black with the first file) |
| HealthPayBox_E32R28T.bin | 2.8" ESP32-32E (E32R28T) |
| HealthPayBox_E32R28T_ST7789.bin | E32R28T with an ST7789 screen |

Flash the file from address 0x0 (one step) with Espressif's web flasher.
The server side is the `type=box` API of the demo Google Apps Script (v0.4.1+).
Demo only: the server certificate is not checked.
