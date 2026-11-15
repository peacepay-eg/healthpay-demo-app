"""Joins bootloader, partition table, app and voice into one file per board,
written from address 0x0, so it can be flashed in one step from a web page."""
import os
import subprocess
import sys

ENVS = ["cyd", "cyd_st7789", "e32", "e32_st7789"]
NAMES = {"cyd": "HealthPayBox_2432S028R", "cyd_st7789": "HealthPayBox_2432S028_ST7789",
         "e32": "HealthPayBox_E32R28T", "e32_st7789": "HealthPayBox_E32R28T_ST7789"}
home = os.path.expanduser("~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin")
os.makedirs("out", exist_ok=True)
for env in ENVS:
    b = ".pio/build/" + env + "/"
    out = "out/" + NAMES[env] + ".bin"
    subprocess.run([sys.executable, "-m", "esptool", "--chip", "esp32", "merge_bin", "-o", out,
                    "--flash_mode", "dio", "--flash_freq", "40m", "--flash_size", "4MB",
                    "0x1000", b + "bootloader.bin", "0x8000", b + "partitions.bin", "0xe000", home,
                    "0x10000", b + "firmware.bin", "0x190000", b + "littlefs.bin"], check=True)
    print(out, os.path.getsize(out))
