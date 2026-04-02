# RP2350-ETH MSC → TCP Bridge

Прошивка для [WaveShare RP2350-ETH](https://www.waveshare.com/wiki/RP2350-ETH).
Плата эмулирует USB-флешку (FAT12), принимает `.txt` файл и отправляет его на TCP-сервер.

## Назначение

Устройство предназначено для хостов, которые умеют только записывать файлы на USB-накопитель, но не имеют прямого сетевого стека. Плата прозрачно транслирует файл в TCP-поток.

```
Хост (USB)                  RP2350-ETH                  Сервер (LAN)
──────────                  ──────────                  ────────────
копирует .txt  ──USB MSC──► RAM FAT12  ──TCP:8888──►  принимает данные
на "флешку"                 CH9120
```

Пропускная способность: **~130 файлов/мин** (WRITE_IDLE_MS=150 мс, файлы до ~5 KB).

## Железо

| Компонент | Описание |
|-----------|----------|
| MCU | RP2350, dual-core Arm Cortex-M33 / RISC-V, 150 MHz |
| RAM | 520 KB SRAM (128 KB отведено под FAT12-диск) |
| Flash | 4 MB |
| Ethernet | CH9120 (10M, UART-to-TCP/UDP) |
| USB | USB 2.0 Full Speed (TinyUSB) |

## Внутренние соединения RP2350 ↔ CH9120

![diagram](diagram.svg)

> Все пять GPIO (GP17–GP21) используются **внутри платы** и на внешние разъёмы не выведены.
> TCPCS (GP17) — аппаратный сигнал готовности: прошивка ждёт LOW перед отправкой файла.

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
 ├─ CH9120 настраивается как TCP client:8888 (сохр. в EEPROM CH9120)
 │  CH9120 держит соединение сам, автоматически переподключается
 ├─ USB MSC монтируется как диск "RP2350-ETH" (128 KB FAT12)
 │
 └─ IDLE ──── ждём файл; LED: медленно мигает (TCP OK) / быстро (нет связи)
      │
      │  копируют .txt на диск
      ▼
    RECEIVING ─── 150 мс тишины после последней записи
      │
      │  .txt найден в корне диска
      ▼
    SENDING
      ├─ (переключения режима CH9120 нет — уже TCP client)
      ├─ ждём TCPCS LOW (GP17), до TCP_CONNECT_TIMEOUT_MS=5 сек
      ├─ файл → UART → CH9120 → TCP → сервер (framed)
      └─ диск очищается → IDLE
```

> CH9120 остаётся в TCP client режиме **постоянно**.
> Переключений нет — EEPROM не изнашивается.

## TCP framed protocol

Данные и debug-сообщения мультиплексируются по одному TCP-соединению:

| Байт [0] | Формат | Содержимое |
|----------|--------|-----------|
| `0x02` | `[0x02][4-byte LE len][raw bytes]` | Содержимое .txt файла |
| `0x01` | `[0x01][1-byte len][ASCII text]` | Debug-сообщение |

### Python-слушатель (пример сервера)

```python
import socket, struct

s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('', 8888))
s.listen(1)
print("Listening on :8888")
conn, addr = s.accept()
print(f"Connected: {addr}")

while True:
    hdr = conn.recv(1)
    if not hdr:
        break
    t = hdr[0]
    if t == 0x01:                          # debug
        n = conn.recv(1)[0]
        print('[DBG]', conn.recv(n).decode())
    elif t == 0x02:                        # file
        n = struct.unpack('<I', conn.recv(4))[0]
        data = b''
        while len(data) < n:
            data += conn.recv(n - len(data))
        fname = f"received_{len(data)}.txt"
        open(fname, 'wb').write(data)
        print(f'[FILE] {n} bytes → {fname}')
```

### Debug-сообщения (плата → сервер)

| Сообщение | Когда |
|-----------|-------|
| `READY` | Boot завершён |
| `FILE_FOUND N bytes` | Файл обнаружен |
| `SENT N bytes #M` | Файл успешно отправлен (M — счётчик) |
| `ERROR: no_txt` | .txt не найден после записи |
| `ERROR: tcp_timeout` | TCP не подключился за 5 сек |

## Индикация светодиода

| Паттерн | Состояние |
|---------|-----------|
| Медленное мигание 1 Гц | IDLE, TCP подключён |
| Среднее мигание 5 Гц | IDLE, TCP не подключён |
| Быстрое мигание 10 Гц | RECEIVING — идёт запись |
| Непрерывно горит | SENDING — TCP передача |
| 3 быстрые вспышки | Успех |
| 5 долгих вспышек | Ошибка (нет .txt или TCP timeout) |

## Конфигурация

Все параметры в [`config.h`](config.h):

```c
// Куда отправлять файл (данные + debug по одному соединению)
#define SERVER_IP_BYTES   {192, 168, 1, 100}
#define SERVER_PORT       8888

// Сетевые настройки платы
#define LOCAL_IP_BYTES    {192, 168, 1, 200}
#define LOCAL_MASK_BYTES  {255, 255, 255, 0}
#define LOCAL_GW_BYTES    {192, 168, 1, 1}
#define LOCAL_PORT        50000

// Пины CH9120 (внутренние, по схеме WaveShare RP2350-ETH)
#define CH9120_TX_PIN       20   // GP20 → CH9120 RXD
#define CH9120_RX_PIN       21   // GP21 ← CH9120 TXD
#define CH9120_TCPCS_PIN    17   // GP17 ← LOW когда TCP connected
#define CH9120_CFG_PIN      18   // GP18 → CFG0
#define CH9120_RST_PIN      19   // GP19 → RSTI

// Таймаут ожидания TCP-соединения (мс)
#define TCP_CONNECT_TIMEOUT_MS  5000

// Задержка обнаружения конца записи (мс)
// Уменьшить нельзя ниже времени, за которое ОС заканчивает запись файла
#define WRITE_IDLE_MS     150
```

## Сборка

### Зависимости

- Board package: [earlephilhower/arduino-pico](https://github.com/earlephilhower/arduino-pico) ≥ 3.x
- Library: **Adafruit TinyUSB Library** ≥ 3.x
- USB Stack: **TinyUSB** (board option `usbstack=tinyusb`)

### arduino-cli

```bash
# Установить зависимости (один раз)
arduino-cli core install rp2040:rp2040 \
  --additional-urls https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json
arduino-cli lib install "Adafruit TinyUSB Library"

# Собрать
arduino-cli compile \
  --fqbn "rp2040:rp2040:generic_rp2350:usbstack=tinyusb" \
  --output-dir build \
  RP2350_ETH_MSC.ino

# Или через готовый скрипт
bash build.sh
```

### Прошивка (DFU / UF2)

1. Зажать кнопку **BOOTSEL**, подключить USB
2. Плата появится как диск `RP2350` в проводнике
3. Скопировать `build/RP2350_ETH_MSC.ino.uf2` на этот диск
4. Плата перезагрузится и запустит прошивку

## Структура файлов

```
RP2350_ETH_MSC.ino   — прошивка
config.h             — конфигурация пользователя
diagram.svg          — схема внутренних соединений
build.sh             — скрипт сборки (arduino-cli)
build/               — артефакты сборки (в .gitignore)
  RP2350_ETH_MSC.ino.uf2   ← файл для прошивки
```
