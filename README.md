# RP2350-ETH MSC → TCP Streaming Bridge

Прошивка для [WaveShare RP2350-ETH](https://www.waveshare.com/wiki/RP2350-ETH).
Плата эмулирует USB-флешку (FAT12 160 KB), хост пишет `.txt` строки в файл — прошивка читает их в реальном времени и отправляет на TCP-сервер.

## Назначение

Устройство для хостов, которые умеют только записывать файл на USB-накопитель, но не имеют прямого сетевого стека. Плата прозрачно стримит данные в TCP — строка за строкой, без накопления файла целиком.

```
Хост (USB MSC)                RP2350-ETH                    Сервер (LAN)
──────────────                ──────────                    ────────────
пишет строки   ──USB MSC──►  FAT12 RAM-диск  ──TCP:2000──► принимает строки
в .txt файл                  (STREAMING)                   в реальном времени
```

**Задержка:** ~155 мс от записи на диск до TCP RECV.
**Пропускная способность:** ~130 пакетов/мин → disk reset каждые ~37 минут.

## Железо

| Компонент | Описание |
|-----------|----------|
| MCU | RP2350, dual-core Arm Cortex-M33 / RISC-V, 150 MHz |
| RAM | 520 KB SRAM (160 KB под FAT12-диск) |
| Flash | 4 MB |
| Ethernet | CH9120 (10M, UART-to-TCP/UDP) |
| USB | USB 2.0 Full Speed (TinyUSB MSC) |
| LED | WS2812B RGB (GP25, GRB) |

## Внутренние соединения RP2350 ↔ CH9120

![diagram](diagram.svg)

> Все GPIO (GP17–GP21, GP25) используются **внутри платы** и на внешние разъёмы не выведены.

| GPIO RP2350 | Пин CH9120 | Направление | Описание |
|-------------|------------|-------------|----------|
| GP20 | RXD | RP2350 → CH9120 | UART1 TX — данные в CH9120 |
| GP21 | TXD | RP2350 ← CH9120 | UART1 RX — данные из CH9120 |
| GP17 | TCPCS | RP2350 ← CH9120 | LOW = TCP-соединение установлено |
| GP18 | CFG0  | RP2350 → CH9120 | LOW = режим конфигурации |
| GP19 | RSTI  | RP2350 → CH9120 | LOW = аппаратный сброс |

## Алгоритм работы

```
Boot
 │
 ├─ CH9120 настраивается как TCP client → SERVER_IP:SERVER_PORT (сохр. в EEPROM)
 ├─ USB MSC монтируется как диск (160 KB FAT12)
 ├─ LED: 3 белые вспышки
 │
 └─ IDLE ─── ждём файл на диске
      │        LED: медленно синий (TCP OK) / медленно красный (нет TCP)
      │
      │  Хост создаёт output_data/wc/EVERY_*.txt и начинает писать строки
      ▼
    STREAMING ─── прошивка читает новые строки по мере записи
      │             LED: мигает градиентом (белый→зелёный→жёлтый→красный)
      │             во время каждого пакета записи
      │
      │  Каждые WRITE_IDLE_MS=150 мс тишины → читаем новые данные → TCP
      │
      │  Три сценария выхода из STREAMING:
      │  1. Диск < 80%, нет записей > 10 с → IDLE (диск не сбрасывается)
      │  2. Диск ≥ 80%, нет записей > 10 с → DISK RESET (плановый)
      │  3. Диск ≥ 95% в любой момент     → DISK RESET (аварийный)
      ▼
    DISK RESET ─── disk_init() + media-change уведомление хосту
      │             Хост видит пустой диск, создаёт новый файл
      └─► IDLE
```

## Формат данных (TCP)

Строки данных передаются как raw ASCII text:
```
891318:76.80;891319:78.59\r\n
891320:78.32;891321:77.68\r\n
```
Каждая строка — два замера: `enum1:weight1;enum2:weight2`.

Debug-сообщения мультиплексируются в тот же поток в framed-формате:

| Байт [0] | Формат | Содержимое |
|----------|--------|-----------|
| `0x01` | `[0x01][1-byte len][ASCII text]` | Debug-сообщение от платы |
| — | raw text `...\r\n` | Строка данных |

### Python-слушатель (минимальный пример)

```python
import socket

s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('', 2000))
s.listen(1)
conn, _ = s.accept()

buf = b''
while True:
    chunk = conn.recv(4096)
    if not chunk:
        break
    if chunk[0] == 0x01:           # debug frame
        n = chunk[1]
        print('[DBG]', chunk[2:2+n].decode())
    else:                          # raw data line
        buf += chunk
        while b'\r\n' in buf:
            line, buf = buf.split(b'\r\n', 1)
            print('[DATA]', line.decode())
```

## Формат файла на диске

Хост создаёт файл по пути `output_data\wc\EVERY_<timestamp>_.txt`.
Строки данных:
```
891318   76.80    78.59 \r\n
```

## Ёмкость диска и тайминги

| Параметр | Значение |
|----------|----------|
| Размер RAM-диска | 320 секторов × 512 байт = **160 KB** |
| Data area | (320 − 7) × 512 = **~152 KB** (sector 7+) |
| Строка данных | 26 байт |
| При 130 пакетах/мин | ~3380 байт/мин |
| Плановый disk reset (80%) | ~121 KB данных, ~36 мин |
| Аварийный disk reset (95%) | ~144 KB данных, немедленно |

После reset плата отправляет хосту media-change → хост видит пустой диск → создаёт новый файл → стриминг продолжается автоматически.

## Индикация WS2812 RGB LED

| Цвет / паттерн | Состояние |
|----------------|-----------|
| Белый, 3 вспышки | Успешный boot |
| Синий, медленно 0.5 Гц | IDLE, TCP подключён |
| Красный, медленно 0.5 Гц | IDLE, нет TCP |
| **Мигает 5 Гц во время записи** | STREAMING — физическая запись USB-пакета |
| — цвет белый | Диск почти пустой (< 33%) |
| — цвет зелёный | Диск заполнен ~33% |
| — цвет жёлтый | Диск заполнен ~66% |
| — цвет красный | Диск заполнен ~95%+ |
| LED тёмный | STREAMING, между пакетами записи |

## Конфигурация

Все параметры в [`config.h`](config.h):

```c
// TCP сервер
#define SERVER_IP_BYTES   {10, 32, 232, 200}
#define SERVER_PORT       2000

// Сетевые настройки платы
#define LOCAL_IP_BYTES    {10, 32, 232, 213}
#define LOCAL_MASK_BYTES  {255, 255, 254, 0}
#define LOCAL_GW_BYTES    {10, 32, 232, 1}
#define LOCAL_PORT        50000

// RAM-диск
#define SECTOR_COUNT      320          // 160 KB
#define SECTOR_SIZE       512

// Тайминги
#define WRITE_IDLE_MS           150    // пауза после последней записи перед обработкой
#define DISK_RESET_FILL_PCT     80     // плановый сброс: 80% + 10 с тишины
#define STREAM_IDLE_RESET_MS    10000  // окно тишины для планового сброса
#define DISK_FORCE_RESET_PCT    95     // аварийный сброс: немедленно

// Отладка (отключить в production)
// #define DEBUG_VERBOSE           // разбор строк: SEND/SKIP/PARSE ERR
// #define DEBUG_FS                // трейс всех USB-секторов + дамп директорий
```

## Тестовые инструменты

| Файл | Назначение |
|------|-----------|
| [`test_bridge.py`](test_bridge.py) | Writer + TCP сервер: симулирует хост и проверяет сквозную доставку |
| [`tcp_monitor.py`](tcp_monitor.py) | Только TCP сервер: слушает порт и логирует всё что приходит |
| [`dist/test_bridge.exe`](dist/test_bridge.exe) | Скомпилированный test_bridge (Windows, без Python) |
| [`dist/tcp_monitor.exe`](dist/tcp_monitor.exe) | Скомпилированный tcp_monitor (Windows, без Python) |

### Запуск

```bash
# Полный тест (запись на диск D: + слушатель)
test_bridge.exe D:\

# Только слушатель (для реального хоста)
tcp_monitor.exe 2000
```

Лог пишется рядом с `.exe` в файл `test_bridge.log` / `tcp_monitor.log`.

### Пример вывода test_bridge

```
[07:20:36] WRITER  file=D:\output_data\wc\Every_20260402_072036_.txt
[07:20:36] SERVER  client connected (10.32.232.213, 50000)
[07:20:36] DBG     BOOT OK: uptime=6863 ms
[07:20:38] WRITE   2 lines  enum 891318..891321
[07:20:38] RECV    891318:76.80;891319:78.59
[07:26:06] STATS   written=326  received=326  loss=0  errors=0
```

## Сборка

### Зависимости

- Board package: [earlephilhower/arduino-pico](https://github.com/earlephilhower/arduino-pico) ≥ 3.x
- Library: **Adafruit TinyUSB Library** ≥ 3.x
- Library: **Adafruit NeoPixel** ≥ 1.x
- USB Stack: **TinyUSB** (board option `usbstack=tinyusb`)

### arduino-cli

```bash
# Установить зависимости (один раз)
arduino-cli core install rp2040:rp2040 \
  --additional-urls https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json
arduino-cli lib install "Adafruit TinyUSB Library" "Adafruit NeoPixel"

# Собрать (через готовый скрипт, Windows)
build_fw.bat

# Или вручную — флаг CFG_TUD_CDC=0 обязателен (см. ниже)
arduino-cli compile \
  --fqbn "rp2040:rp2040:waveshare_rp2350_plus:usbstack=tinyusb" \
  --build-property "build.extra_flags=-DCFG_TUD_CDC=0" \
  --output-dir build \
  RP2350_ETH_MSC.ino
```

### Прошивка (UF2)

1. Зажать кнопку **BOOTSEL**, подключить USB
2. Плата появится как диск `RP2350` в проводнике
3. Скопировать `build/RP2350_ETH_MSC.ino.uf2` на этот диск
4. Плата перезагрузится и запустит прошивку

---

## Совместимость с промышленными хостами (VxWorks)

### Проблема: composite USB device

По умолчанию arduino-pico с TinyUSB добавляет USB CDC (виртуальный COM-порт) в дескриптор
автоматически — даже если в коде нет `Serial.begin()`. В результате устройство объявляется
как **composite (MSC + CDC)** с `bDeviceClass=0xEF` (IAD).

Промышленные хосты под VxWorks принимают только **чистый MSC** (`bDeviceClass=0x00`).
При виде composite-дескриптора они молча игнорируют устройство.

**Решение: флаг `-DCFG_TUD_CDC=0`** при компиляции.

Это также требует трёх патчей в `Adafruit_TinyUSB_Library` (файлы в `Documents/Arduino/libraries`):

| Файл | Патч |
|------|------|
| `Adafruit_TinyUSB.h` | Добавить `extern "C"` к `TinyUSB_Device_Init` — иначе линкер падает с ошибкой C/C++ linkage при CDC=0 |
| `Adafruit_TinyUSB_API.cpp` | Обернуть `tud_cdc_n_write_flush` в `#if CFG_TUD_CDC` |
| `Adafruit_USBD_Device.cpp` | Обернуть `SerialTinyUSB.begin()` и установку `bDeviceClass=TUSB_CLASS_MISC` в `#if CFG_TUD_CDC` / `#else bDeviceClass=0x00` |

---

## Клонирование USB-дескриптора рабочей флешки

Промышленный хост (VxWorks) мог отклонять устройство по несовпадению USB-дескриптора.
Для максимальной совместимости был снят полный профиль рабочей FAT12-флешки (VendorCo),
которую хост гарантированно принимает, и дескриптор RP2350 клонирован под неё.

### Сравнение lsusb (Linux) до и после клонирования

| Поле | VendorCo (эталон) | RP2350 (итог) |
|------|-------------------|---------------|
| bDeviceClass | 0 | 0 ✓ |
| idVendor | 0x346d | 0x346d ✓ |
| idProduct | 0x5678 | 0x5678 ✓ |
| bcdDevice | 2.00 | 2.00 ✓ |
| iManufacturer | "USB" | "USB" ✓ |
| iProduct | "Disk 2.0" | "Disk 2.0" ✓ |
| bmAttributes | 0x80 | 0x80 ✓ |
| MaxPower | 100mA | 100mA ✓ |
| Interface | 8/6/80 (MSC/SCSI/Bulk) | 8/6/80 ✓ |
| EP OUT | 0x01 | 0x01 ✓ |
| EP IN | 0x82 | 0x82 ✓ |
| wMaxPacketSize | 512 (HS) | 64 (FS) — аппаратное ограничение |
| iSerial | уникальный | chip ID (намеренно) |

### Что пришлось исправить

**`bcdDevice 2.00`** — `TinyUSBDevice.setVersion()` меняет `bcdUSB` (версию протокола USB),
а не `bcdDevice`. Правильный метод — `TinyUSBDevice.setDeviceVersion(0x0200)`.

**EP IN 0x82 вместо 0x81** — TinyUSB выдаёт эндпоинты последовательно начиная с EP1.
MSC — единственный интерфейс, поэтому получает EP1 IN (0x81).
Эталонная флешка использует EP2 IN (0x82).
Исправление в `Adafruit_USBD_MSC.cpp`: добавить фиктивную аллокацию перед MSC,
чтобы сдвинуть счётчик:
```cpp
(void)TinyUSBDevice.allocEndpoint(TUSB_DIR_IN); // skip EP1 IN → force EP2 IN (0x82)
uint8_t const ep_in = TinyUSBDevice.allocEndpoint(TUSB_DIR_IN);
```

**Уникальный серийный номер** — при двух одинаковых клонах на одном Linux-хосте
`/dev/disk/by-id` конфликтует. Решение: `rp2040.getChipID()` даёт уникальный 64-bit ID.

**`wMaxPacketSize 512 vs 64`** — RP2350 работает в USB Full Speed (64 байт/пакет),
эталонная флешка High Speed (512 байт/пакет). Аппаратное ограничение, не исправляется.

---

## Совместимость FAT12 BPB

### Проблема

ОС при монтировании читает **BPB (BIOS Parameter Block)** — метаданные в boot-секторе.
Несоответствие BPB реальной флешке может приводить к отказу монтирования или ошибкам
при работе с томом на промышленных хостах.

Для отладки был снят hex-дамп рабочей FAT12-флешки (WinImage) на Linux и проведён
побайтовый анализ boot-сектора.

### Параметры BPB (соответствуют реальной FAT12-флешке WinImage)

```
OEM Name:         "MSDOS5.0"
Bytes/Sector:     512        (0x0200)
Sectors/Cluster:  1
Reserved Sectors: 1
FAT Count:        2
Root Entries:     64         (0x0040)  ← было 16, исправлено
Total Sectors:    320        (0x0140)  ← было 256, исправлено
Media Byte:       0xF8
Sectors/FAT:      1
Sectors/Track:    32         (0x0020)  ← было 63, исправлено
Heads:            1          (0x0001)  ← было 255, исправлено
Drive Number:     0x80
Boot Signature:   0x29       (extended BPB)
Volume Label:     "NO NAME   "
FS Type:          "FAT12   "
Boot Signature:   0x55AA
```

### Расположение секторов

```
Sector 0        Boot Record (BPB)
Sector 1        FAT1
Sector 2        FAT2
Sectors 3–6     Root Directory (64 entries × 32 байт = 4 сектора)
Sectors 7–319   Data (DATA_SECTOR = 7)
```

### FAT-заголовок

Первые 3 байта каждой FAT-таблицы — зарезервированные записи:
```
FAT[0] = 0xFF8  (media byte, packed FAT12)
FAT[1] = 0xFFF  (end-of-chain, clean / dirty bit = 1)
Байты: F8 FF FF
```

---

## Структура файлов

```
RP2350_ETH_MSC.ino   — прошивка (STREAMING + disk_reset)
config.h             — конфигурация
diagram.svg          — схема внутренних соединений
test_bridge.py       — тест: writer + TCP сервер
tcp_monitor.py       — TCP монитор (только слушатель)
build_fw.bat         — сборка прошивки через arduino-cli
build.bat            — сборка exe через PyInstaller
dist/
  test_bridge.exe    — Windows exe (без Python)
  tcp_monitor.exe    — Windows exe (без Python)
build/               — артефакты сборки (.gitignore)
  RP2350_ETH_MSC.ino.uf2  — последняя прошивка (tracked)
```
