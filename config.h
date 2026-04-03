#pragma once

// ================================================================
// config.h — User configuration for RP2350-ETH MSC→TCP Bridge
// ================================================================

// ── TCP сервер ───────────────────────────────────────────────────
// Единственное подключение: плата держит постоянный TCP-сокет.
// Данные и debug идут по одному потоку в framed-протоколе:
//   [0x02][4-byte LE size][raw bytes]  — файл
//   [0x01][1-byte len][ASCII text]     — debug log
#define SERVER_IP_BYTES   {10, 32, 232, 200}
#define SERVER_PORT       2000

// ── Сетевые настройки платы ──────────────────────────────────────
#define LOCAL_IP_BYTES    {10, 32, 232, 213}
#define LOCAL_MASK_BYTES  {255, 255, 254, 0}
#define LOCAL_GW_BYTES    {10, 32, 232, 1}
#define LOCAL_PORT        50000          // Исходящий TCP-порт платы

// ── Пины CH9120 (внутренние, на внешние разъёмы не выведены) ─────
// GP20 = RP2350 UART1 TX → CH9120 RXD  (RP2350 передаёт в CH9120)
// GP21 = RP2350 UART1 RX ← CH9120 TXD  (RP2350 принимает от CH9120)
// GP17 = CH9120 TCPCS: LOW когда TCP-соединение установлено
// GP18 = CH9120 CFG0:  LOW для входа в режим конфигурации
// GP19 = CH9120 RSTI:  LOW для аппаратного сброса
#define CH9120_UART         Serial2
#define CH9120_TX_PIN       20   // GP20 → CH9120 RXD
#define CH9120_RX_PIN       21   // GP21 ← CH9120 TXD
#define CH9120_TCPCS_PIN    17   // GP17 ← CH9120 TCPCS (LOW = соединение есть)
#define CH9120_CFG_PIN      18   // GP18 → CH9120 CFG0
#define CH9120_RST_PIN      19   // GP19 → CH9120 RSTI

// Таймаут ожидания TCP-соединения при boot и перед отправкой (мс)
#define TCP_CONNECT_TIMEOUT_MS  5000

// Скорость UART при конфигурации CH9120 (стандарт — 9600)
#define CH9120_CFG_BAUD   9600UL
// Скорость UART при передаче данных (115200 — проверено WaveShare demo)
#define CH9120_DATA_BAUD  115200UL

// ── WS2812 RGB LED ───────────────────────────────────────────────
// GP25 = встроенный WS2812B (GRB, 800 кГц). Цвета индикации:
//   Красный  (медленно)  — IDLE, нет TCP
//   Синий    (медленно)  — IDLE, TCP подключён
//   Зелёный  (быстро)   — STREAMING: данные пишутся/читаются
//   Белый    (3 вспышки) — успешный старт
#define WS2812_PIN   25

// ── RAM-диск ─────────────────────────────────────────────────────
// FAT12: 320 секторов × 512 байт = 160 KB
// Параметры BPB соответствуют реальной FAT12-флешке (WinImage):
//   root entries=64, sectors_per_track=32, heads=1, media=0xF8
#define SECTOR_COUNT      320
#define SECTOR_SIZE       512

// ── Тайминги ─────────────────────────────────────────────────────
// Сколько ждать после последней USB-записи, прежде чем обработать файл (мс).
#define WRITE_IDLE_MS           150
// Мягкий сброс: диск >= DISK_RESET_FILL_PCT % + тишина STREAM_IDLE_RESET_MS мс.
#define DISK_RESET_FILL_PCT     80
#define STREAM_IDLE_RESET_MS    10000UL
// Аварийный сброс: диск >= DISK_FORCE_RESET_PCT % — немедленно, без ожидания.
// Защита от ситуации когда хост пишет непрерывно и окно тишины не наступает.
#define DISK_FORCE_RESET_PCT    95

// ── Отладка ───────────────────────────────────────────────────────
// DEBUG_VERBOSE: подробный разбор строк (SEND/SKIP/PARSE ERR/FILE stats)
#define DEBUG_VERBOSE

// DEBUG_FS: трейс файловой системы — для анализа поведения хост-устройства.
// Каждый USB-сектор-запись → в кольцевой буфер → отправляется по TCP:
//   FS t=1234 lba=1 [FAT1] sz=512
//   FS t=1236 lba=3 [ROOT] sz=512
//   FS t=1238 lba=5 [DATA clust=2] sz=512
// При каждом WRITE_IDLE — дамп директорий:
//   FS DIR ROOT[0] SYSTEM~1     attr=12 cl=2 sz=0
//   FS DIR ROOT[2] OUTPUT~1     attr=10 cl=3 sz=0
//   FS DIR  sub[0] WC           attr=10 cl=4 sz=0
//   FS DIR   wc[0] EVERY_2~1TXT attr=20 cl=5 sz=1024
// Отключить в production — значительный трафик.
#define DEBUG_FS
#define FS_EVT_COUNT  64   // размер кольцевого буфера событий

// DEBUG_SCSI: log all SCSI commands + READ10 LBAs via TCP debug frames.
// Calls via msc_debug_hook (weak symbol in Adafruit library) and msc_read_cb.
// Disable in production — one debug frame per USB transaction.
#define DEBUG_SCSI
#define SCSI_EVT_COUNT  32   // ring buffer for SCSI command events
