#!/bin/bash
# ================================================================
# build.sh — сборка и прошивка RP2350_ETH_MSC через arduino-cli
# Запускать из C:\DEV\ARDUINO\RP2350_ETH_MSC\
# ================================================================

CLI="$(dirname "$0")/../arduino-cli.exe"
SKETCH="$(dirname "$0")/RP2350_ETH_MSC.ino"
FQBN="rp2040:rp2040:waveshare_rp2350_plus:usbstack=tinyusb"   # WaveShare PID=0x10B1 + TinyUSB stack

# Порт DFU/UF2 (определится автоматически при --port)
# Для DFU: подключите плату с зажатой кнопкой BOOTSEL, она появится как USB-диск
# arduino-cli для rp2040 использует UF2-загрузчик автоматически

echo "=== Установка board package (если ещё не установлен) ==="
"$CLI" core update-index --additional-urls https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json
"$CLI" core install rp2040:rp2040 --additional-urls https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json

echo ""
echo "=== Установка библиотеки Adafruit TinyUSB ==="
"$CLI" lib install "Adafruit TinyUSB Library"

echo ""
echo "=== Компиляция ==="
"$CLI" compile \
  --fqbn "$FQBN" \
  --build-property "build.extra_flags=-DUSE_TINYUSB" \
  --output-dir "$(dirname "$SKETCH")/build" \
  "$SKETCH"

if [ $? -ne 0 ]; then
  echo "ОШИБКА компиляции"
  exit 1
fi

echo ""
echo "=== Загрузка (UF2/DFU) ==="
# Для UF2 arduino-cli сам найдёт смонтированный диск RPI-RP2 или RP2350
"$CLI" upload \
  --fqbn "$FQBN" \
  --build-property "build.extra_flags=-DUSE_TINYUSB" \
  "$SKETCH"

echo ""
echo "=== Готово ==="
