/*
 * RP2350-ETH MSC → TCP Streaming Bridge
 * ======================================
 * Прошивка для WaveShare RP2350-ETH.
 *
 * Алгоритм:
 *   1. Boot → CH9120 настраивается как TCP client (SERVER_IP:SERVER_PORT).
 *   2. Плата появляется как USB-флешка (FAT12, 128 KB RAM).
 *   3. Хост создаёт папки и пишет лог-файл .txt (append).
 *   4. После каждого WRITE_IDLE_MS молчания — читаем НОВЫЕ строки файла,
 *      парсим и шлём по TCP. Диск НЕ очищается — хост продолжает append.
 *   5. При замене файла (новый cluster) — сбрасываем позицию.
 *
 * Входной формат (лог хоста):
 *   START PACK WEIGHT LOG
 *        1-APR-2026  14:02
 *   891318   79.70    79.20
 *   ...
 *
 * Исходящий TCP (raw text):
 *   891318:79.70;891319:79.20\r\n
 *   (два веса на строку → два отдельных энумератора n и n+1)
 *
 * Debug фреймы (для диагностики): [0x01][1-byte len][ASCII text]
 *
 * Зависимости:
 *   - Board package: earlephilhower/arduino-pico >= 3.x
 *   - Library:       Adafruit TinyUSB Library >= 3.x
 *   - FQBN:          rp2040:rp2040:waveshare_rp2350_plus:usbstack=tinyusb
 *
 * Индикация LED_BUILTIN:
 *   Медленное мигание (1 Гц)  — IDLE, TCP подключён
 *   Быстрое мигание  (5 Гц)   — IDLE, TCP не подключён
 *   Очень быстрое    (10 Гц)  — STREAMING: данные пишутся/читаются
 */

#include <Adafruit_TinyUSB.h>
#include <Adafruit_NeoPixel.h>
#include <stdarg.h>
#include "config.h"

// ── Verbose debug macro ───────────────────────────────────────────
#ifdef DEBUG_VERBOSE
  #define DBG_V(...)  dbg_sendf(__VA_ARGS__)
#else
  #define DBG_V(...)  ((void)0)
#endif

// ── FS event ring buffer (DEBUG_FS) ──────────────────────────────
#ifdef DEBUG_FS
struct FsEvt {
    uint32_t ms;
    uint32_t lba;
    uint16_t sz;
};
static FsEvt          fs_evts[FS_EVT_COUNT];
static volatile uint8_t fs_head = 0;   // write ptr (from write_cb)
static          uint8_t fs_tail = 0;   // read ptr  (from loop)
#endif

// ── SCSI event ring buffer (DEBUG_SCSI) ──────────────────────────
#ifdef DEBUG_SCSI
struct ScsiEvt {
    uint32_t ms;
    uint8_t  cmd;    // SCSI opcode; 0x28=READ10 from msc_read_cb
    uint32_t lba;    // LBA for READ10; start param for START_STOP; 0 others
};
static ScsiEvt          scsi_evts[SCSI_EVT_COUNT];
static volatile uint8_t scsi_head = 0;
static          uint8_t scsi_tail = 0;
#endif

// ── WS2812 цвета (GRB, dim — не слепит) ─────────────────────────
#define LED_OFF     0x000000u
#define LED_RED     0x300000u   // IDLE, нет TCP
#define LED_BLUE    0x000030u   // IDLE, TCP OK
#define LED_GREEN   0x003000u   // STREAMING
#define LED_WHITE   0x181818u   // загрузка / успех
#define LED_ORANGE  0x200800u   // ожидание TCP на старте

// ================================================================
// Типы
// ================================================================
enum class State : uint8_t { IDLE, STREAMING };

struct TxtFile {
    uint16_t start_cluster;
    uint32_t file_size;
};

// ================================================================
// FAT12 layout (320 секторов × 512 байт = 160 KB):
//   Sector 0      — Boot Record
//   Sector 1      — FAT1
//   Sector 2      — FAT2
//   Sectors 3–6   — Root Directory (64 entries × 32 байт = 4 сектора)
//   Sectors 7–319 — Data
// BPB соответствует WinImage FAT12: root=64, heads=1, track=32
// ================================================================
#define DISK_BYTES    ((uint32_t)SECTOR_COUNT * SECTOR_SIZE)
#define FAT1_SECTOR   1
#define FAT2_SECTOR   2
#define ROOT_SECTOR   3
#define DATA_SECTOR   7
#define ROOT_ENTRIES  64

// ================================================================
// Глобальные переменные
// ================================================================
static uint8_t disk[DISK_BYTES] __attribute__((aligned(4)));

static volatile State    state          = State::IDLE;
static volatile bool     g_write_pending = false;
static volatile uint32_t g_last_write_ms = 0;

// Streaming state
static uint16_t g_stream_cluster  = 0;   // start cluster текущего файла
static uint32_t g_processed_bytes = 0;   // сколько байт уже отправлено

// LED write-activity: обновляется в msc_write_cb
static volatile uint32_t g_led_write_ms = 0;

// Буфер частичной строки (персистентный между вызовами stream_process_new)
static char    s_line_buf[80];
static uint8_t s_line_len = 0;

// Boot time
static uint32_t boot_ms = 0;

// TCP prev state для детекции
static bool dbg_tcp_prev = false;

// Heartbeat: периодически шлём статус независимо от TCP-состояния
static uint32_t hb_last_ms   = 0;
static uint32_t hb_scsi_count = 0;  // счётчик SCSI событий с момента boot
static uint32_t hb_wr_count   = 0;  // счётчик disk write событий

// LED анимация
static uint32_t led_timer    = 0;
static bool     led_state    = false;
static uint8_t  flash_count  = 0;
static uint8_t  flash_total  = 0;
static uint16_t flash_on_ms  = 100;
static uint16_t flash_off_ms = 100;
static bool     flash_looped = false;
static uint32_t flash_color  = LED_WHITE;

Adafruit_NeoPixel pixel(1, WS2812_PIN, NEO_GRB + NEO_KHZ800);

Adafruit_USBD_MSC usb_msc;

// ================================================================
// Прототипы
// ================================================================
static int32_t  msc_read_cb(uint32_t lba, void* buf, uint32_t bufsize);
static int32_t  msc_write_cb(uint32_t lba, uint8_t* buf, uint32_t bufsize);
static void     msc_flush_cb();
static void     disk_init();
static uint16_t fat12_next(uint16_t cluster);
static bool     fat12_scan_entries(const uint8_t* entries, int count, TxtFile& out);
static bool     fat12_scan_dir(uint16_t cluster, TxtFile& out);
static bool     fat12_find_txt(TxtFile& out);
static void     disk_reset();
#ifdef DEBUG_FS
static void     fs_log_drain();
static void     fs_dump_dirs();
#endif
#ifdef DEBUG_SCSI
static void     scsi_log_drain();
#endif
static void     ch9120_raw_cmd(uint8_t cmd, const uint8_t* data, uint8_t len);
static void     ch9120_cfg_enter();
static void     ch9120_cfg_exit(bool save_eeprom);
static void     ch9120_setup_tcp();
static bool     tcp_connected();
static bool     tcp_wait_connected(uint32_t timeout_ms);
static void     parse_and_send_line(const char* line, uint8_t len);
static void     stream_process_new(uint16_t start_cluster, uint32_t from_pos, uint32_t to_pos);
static void     dbg_send(const char* msg);
static void     dbg_sendf(const char* fmt, ...);
static void     led_set(uint32_t color);
static void     led_flash(uint8_t count, uint32_t color, uint16_t on_ms, uint16_t off_ms, bool loop);
static void     led_update();
static void     led_flash_sync(uint8_t count, uint32_t color, uint16_t on_ms, uint16_t off_ms);

// ================================================================
// USB MSC callbacks
// ================================================================
static int32_t msc_read_cb(uint32_t lba, void* buf, uint32_t bufsize) {
#ifdef DEBUG_SCSI
    {
        uint8_t h = scsi_head;
        scsi_evts[h] = { millis(), 0x28, lba };
        scsi_head = (uint8_t)((h + 1) % SCSI_EVT_COUNT);
    }
#endif
    // За пределами RAM-диска возвращаем нули (хост видит пустое пространство 8 GB)
    if (lba >= SECTOR_COUNT) {
        memset(buf, 0, bufsize);
        return (int32_t)bufsize;
    }
    if (lba + bufsize / SECTOR_SIZE > SECTOR_COUNT) {
        uint32_t valid = (SECTOR_COUNT - lba) * SECTOR_SIZE;
        memcpy(buf, disk + lba * SECTOR_SIZE, valid);
        memset((uint8_t*)buf + valid, 0, bufsize - valid);
        return (int32_t)bufsize;
    }
    memcpy(buf, disk + lba * SECTOR_SIZE, bufsize);
    return (int32_t)bufsize;
}

static int32_t msc_write_cb(uint32_t lba, uint8_t* buf, uint32_t bufsize) {
    if (lba + bufsize / SECTOR_SIZE > SECTOR_COUNT) return -1;
    memcpy(disk + lba * SECTOR_SIZE, buf, bufsize);
    g_write_pending  = true;
    g_last_write_ms  = millis();
    g_led_write_ms   = millis();
    if (state == State::IDLE) state = State::STREAMING;
    hb_wr_count++;
#ifdef DEBUG_FS
    uint8_t h = fs_head;
    fs_evts[h] = { millis(), lba, (uint16_t)bufsize };
    fs_head = (uint8_t)((h + 1) % FS_EVT_COUNT);
#endif
    return (int32_t)bufsize;
}

static void msc_flush_cb() {}

// ================================================================
// SCSI debug hook + drain (DEBUG_SCSI)
// Overrides the weak symbol defined in Adafruit_USBD_MSC.cpp.
// Called from USB interrupt context → only touches volatile ring buffer.
// ================================================================
#ifdef DEBUG_SCSI

void msc_debug_hook(uint8_t event, uint32_t param) {
    uint8_t h = scsi_head;
    scsi_evts[h] = { millis(), event, param };
    scsi_head = (uint8_t)((h + 1) % SCSI_EVT_COUNT);
    hb_scsi_count++;
}

static void scsi_log_drain() {
    while (scsi_tail != scsi_head) {
        uint8_t  i   = scsi_tail;
        scsi_tail    = (uint8_t)((i + 1) % SCSI_EVT_COUNT);
        uint8_t  cmd = scsi_evts[i].cmd;
        uint32_t lba = scsi_evts[i].lba;
        const char* nm;
        switch (cmd) {
            case 0x00: nm = "TUR";        break;
            case 0x03: nm = "REQ_SENSE";  break;
            case 0x12: nm = "INQUIRY";    break;
            case 0x1A: nm = "MSENSE6";    break;
            case 0x1B: nm = "START_STOP"; break;
            case 0x1E: nm = "PREV_ALLOW"; break;
            case 0x23: nm = "FMT_CAP";    break;
            case 0x25: nm = "RD_CAP";     break;
            case 0x28: nm = "READ10";     break;
            case 0x2A: nm = "WRITE10";    break;
            case 0x5A: nm = "MSENSE10";   break;
            default:   nm = NULL;         break;
        }
        if (cmd == 0x28)
            dbg_sendf("SCSI %s lba=%lu t=%lu", nm,
                      (unsigned long)lba, (unsigned long)scsi_evts[i].ms);
        else if (cmd == 0x1B)
            dbg_sendf("SCSI %s start=%lu t=%lu", nm,
                      (unsigned long)lba, (unsigned long)scsi_evts[i].ms);
        else if (nm)
            dbg_sendf("SCSI %s t=%lu", nm, (unsigned long)scsi_evts[i].ms);
        else
            dbg_sendf("SCSI CMD=%02X t=%lu", (unsigned)cmd,
                      (unsigned long)scsi_evts[i].ms);
    }
}

#endif  // DEBUG_SCSI

// ================================================================
// FAT12 — инициализация
// ================================================================
static void disk_init() {
    memset(disk, 0, sizeof(disk));
    uint8_t* bs = disk;
    bs[0] = 0xEB; bs[1] = 0x3C; bs[2] = 0x90;
    memcpy(bs + 3,  "MSDOS5.0", 8);
    bs[11] = 0x00; bs[12] = 0x02;
    bs[13] = 0x01;
    bs[14] = 0x01; bs[15] = 0x00;
    bs[16] = 0x02;
    bs[17] = 0x40; bs[18] = 0x00;  // root entries = 64
    bs[19] = 0x40; bs[20] = 0x01;  // total sectors = 320 (0x0140)
    bs[21] = 0xF8;                  // media descriptor = fixed disk
    bs[22] = 0x01; bs[23] = 0x00;  // sectors per FAT = 1
    bs[24] = 0x20; bs[25] = 0x00;  // sectors per track = 32
    bs[26] = 0x01; bs[27] = 0x00;  // heads = 1
    bs[36] = 0x80; bs[37] = 0x01; bs[38] = 0x29;
    bs[39] = 0x50; bs[40] = 0x23; bs[41] = 0x10; bs[42] = 0x01;
    memcpy(bs + 43, "RP2350-ETH ", 11);
    memcpy(bs + 54, "FAT12   ",   8);
    bs[510] = 0x55; bs[511] = 0xAA;
    const uint8_t fh[6] = {0xF8, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    memcpy(disk + FAT1_SECTOR * SECTOR_SIZE, fh, 6);
    memcpy(disk + FAT2_SECTOR * SECTOR_SIZE, fh, 6);
}

// Сброс диска + уведомление хоста о смене носителя.
// Вызывается когда хост перестал писать дольше STREAM_IDLE_RESET_MS
// (конец сессии или диск заполнен). Windows переподключает носитель.
static void disk_reset() {
    dbg_sendf("DISK RESET: processed=%lu bytes -> reinit", (unsigned long)g_processed_bytes);
    disk_init();
    g_stream_cluster  = 0;
    g_processed_bytes = 0;
    s_line_len        = 0;
    state = State::IDLE;
    // Сигнал хосту: носитель заменён → Windows перечитает диск с нуля
    usb_msc.setUnitReady(false);
    delay(250);
    usb_msc.setUnitReady(true);
    led_flash_sync(2, LED_WHITE, 80, 80);
    led_flash(255, tcp_connected() ? LED_BLUE : LED_RED, 500, 500, true);
}

// ================================================================
// DEBUG_FS — трейс секторов и дамп директорий
// ================================================================
#ifdef DEBUG_FS

// Дрейн кольцевого буфера → debug-фреймы по TCP
static void fs_log_drain() {
    while (fs_tail != fs_head) {
        uint8_t i  = fs_tail;
        fs_tail    = (uint8_t)((i + 1) % FS_EVT_COUNT);
        uint32_t lba = fs_evts[i].lba;
        const char* t;
        uint16_t cl = 0;
        if      (lba == 0)                               t = "BOOT";
        else if (lba == FAT1_SECTOR)                     t = "FAT1";
        else if (lba == FAT2_SECTOR)                     t = "FAT2";
        else if (lba >= ROOT_SECTOR && lba < DATA_SECTOR) t = "ROOT";
        else { t = "DATA"; cl = (uint16_t)(lba - DATA_SECTOR + 2); }
        if (cl)
            dbg_sendf("FS t=%lu lba=%lu [%s clust=%u] sz=%u",
                      (unsigned long)fs_evts[i].ms, (unsigned long)lba,
                      t, cl, fs_evts[i].sz);
        else
            dbg_sendf("FS t=%lu lba=%lu [%s] sz=%u",
                      (unsigned long)fs_evts[i].ms, (unsigned long)lba,
                      t, fs_evts[i].sz);
    }
}

// Форматирует имя 8.3 из записи каталога в строку "XXXXXXXX.EEE"
static void fmt83(const uint8_t* de, char* out) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < 8 && de[i] != ' '; i++) out[n++] = (char)de[i];
    if (de[8] != ' ') {
        out[n++] = '.';
        for (uint8_t i = 8; i < 11 && de[i] != ' '; i++) out[n++] = (char)de[i];
    }
    out[n] = '\0';
}

// Дамп одного сектора директории с префиксом метки
static void fs_dump_sector(const uint8_t* data, uint16_t count,
                            const char* label, uint8_t depth) {
    char indent[5]; uint8_t d = depth < 4 ? depth : 4;
    for (uint8_t i = 0; i < d; i++) indent[i] = ' '; indent[d] = '\0';

    for (uint16_t i = 0; i < count; i++) {
        const uint8_t* de = data + i * 32;
        if (de[0] == 0x00) break;
        if (de[0] == 0xE5) continue;
        uint8_t attr = de[11];
        if (attr == 0x0F) continue;   // LFN — пропускаем
        if (attr & 0x08) continue;    // volume label
        char name[14]; fmt83(de, name);
        uint16_t cl = (uint16_t)(de[26] | ((uint16_t)de[27] << 8));
        uint32_t sz = (uint32_t)(de[28] | ((uint32_t)de[29] << 8)
                    | ((uint32_t)de[30] << 16) | ((uint32_t)de[31] << 24));
        dbg_sendf("FS%s %s[%u] %-12s attr=%02X cl=%u sz=%lu",
                  indent, label, i, name, attr, cl, (unsigned long)sz);
    }
}

// Рекурсивный дамп директорий (2 уровня вглубь)
static void fs_dump_subdir(uint16_t cluster, const char* label, uint8_t depth) {
    uint8_t chain = 0;
    while (cluster >= 2 && cluster < 0xFF8 && chain < 16) {
        uint32_t sector = DATA_SECTOR + (cluster - 2);
        if (sector >= SECTOR_COUNT) break;
        fs_dump_sector(disk + sector * SECTOR_SIZE, SECTOR_SIZE / 32, label, depth);
        cluster = fat12_next(cluster);
        chain++;
    }
}

// Полный дамп: root + все найденные субдиректории (1 уровень)
static void fs_dump_dirs() {
    dbg_send("FS --- DIR DUMP ---");
    const uint8_t* root = disk + ROOT_SECTOR * SECTOR_SIZE;
    fs_dump_sector(root, ROOT_ENTRIES, "ROOT", 0);

    // Ищем директории в root и дампим их содержимое
    for (uint8_t i = 0; i < ROOT_ENTRIES; i++) {
        const uint8_t* de = root + i * 32;
        if (de[0] == 0x00) break;
        if (de[0] == 0xE5 || de[11] == 0x0F || (de[11] & 0x08)) continue;
        if (!(de[11] & 0x10)) continue;   // только директории
        uint16_t cl = (uint16_t)(de[26] | ((uint16_t)de[27] << 8));
        if (cl < 2) continue;
        char lbl[12]; fmt83(de, lbl);
        fs_dump_subdir(cl, lbl, 1);

        // Ещё один уровень вглубь
        uint16_t c2 = cl;
        uint8_t ch = 0;
        while (c2 >= 2 && c2 < 0xFF8 && ch < 8) {
            uint32_t sec = DATA_SECTOR + (c2 - 2);
            if (sec >= SECTOR_COUNT) break;
            const uint8_t* sd = disk + sec * SECTOR_SIZE;
            for (uint8_t j = 0; j < SECTOR_SIZE / 32; j++) {
                const uint8_t* sde = sd + j * 32;
                if (sde[0] == 0x00) break;
                if (sde[0] == 0xE5 || sde[11] == 0x0F || (sde[11] & 0x08)) continue;
                if (!(sde[11] & 0x10) || sde[0] == '.') continue;
                uint16_t cl2 = (uint16_t)(sde[26] | ((uint16_t)sde[27] << 8));
                if (cl2 < 2) continue;
                char lbl2[12]; fmt83(sde, lbl2);
                fs_dump_subdir(cl2, lbl2, 2);
            }
            c2 = fat12_next(c2); ch++;
        }
    }
    dbg_send("FS --- END DUMP ---");
}

#endif  // DEBUG_FS

// ================================================================
// FAT12 — обход цепочки кластеров
// ================================================================
static uint16_t fat12_next(uint16_t cluster) {
    const uint8_t* fat = disk + FAT1_SECTOR * SECTOR_SIZE;
    uint32_t off = ((uint32_t)cluster * 3) / 2;
    if (cluster & 1)
        return (fat[off] >> 4) | ((uint16_t)fat[off + 1] << 4);
    else
        return fat[off] | (((uint16_t)fat[off + 1] & 0x0F) << 8);
}

// Сканирует записи директории, ищет первый .TXT файл.
// Сканирует entries и обновляет out если нашли .txt с бо́льшим cluster (= более новый файл).
// Возвращает true если хоть что-то нашли.
static bool fat12_scan_entries(const uint8_t* entries, int count, TxtFile& out) {
    auto uc = [](uint8_t c) -> uint8_t {
        return (c >= 'a' && c <= 'z') ? (uint8_t)(c - 32u) : c;
    };
    bool found = false;
    for (int i = 0; i < count; i++) {
        const uint8_t* de = entries + i * 32;
        if (de[0] == 0x00) break;
        if (de[0] == 0xE5 || de[0] == '.') continue;
        uint8_t attr = de[11];
        if (attr & 0x08) continue;

        uint16_t cluster = (uint16_t)(de[26] | ((uint16_t)de[27] << 8));

        if (attr & 0x10) {
            if (cluster >= 2)
                if (fat12_scan_dir(cluster, out)) found = true;
        } else {
            if (uc(de[8]) == 'T' && uc(de[9]) == 'X' && uc(de[10]) == 'T') {
                uint32_t size = (uint32_t)(de[28] | ((uint32_t)de[29] << 8)
                                | ((uint32_t)de[30] << 16) | ((uint32_t)de[31] << 24));
                // Берём файл с наибольшим cluster — он аллоцирован последним = самый новый
                if (size > 0 && cluster > out.start_cluster) {
                    out.start_cluster = cluster;
                    out.file_size = size;
                    found = true;
                }
            }
        }
    }
    return found;
}

static bool fat12_scan_dir(uint16_t cluster, TxtFile& out) {
    uint8_t chain_len = 0;
    while (cluster >= 2 && cluster < 0xFF8) {
        uint32_t sector = DATA_SECTOR + (cluster - 2);
        if (sector >= SECTOR_COUNT) break;
        fat12_scan_entries(disk + sector * SECTOR_SIZE, SECTOR_SIZE / 32, out);
        cluster = fat12_next(cluster);
        if (++chain_len > 252) break;
    }
    return out.start_cluster >= 2;
}

static bool fat12_find_txt(TxtFile& out) {
    out.start_cluster = 0;
    out.file_size = 0;
    fat12_scan_entries(disk + ROOT_SECTOR * SECTOR_SIZE, ROOT_ENTRIES, out);
    return out.start_cluster >= 2;
}

// ================================================================
// CH9120 — конфигурация
// ================================================================
static void ch9120_raw_cmd(uint8_t cmd, const uint8_t* data, uint8_t len) {
    const uint8_t hdr[2] = {0x57, 0xAB};
    CH9120_UART.write(hdr, 2);
    CH9120_UART.write(cmd);
    if (data && len > 0) CH9120_UART.write(data, len);
    CH9120_UART.flush();
    delay(50);
}

static void ch9120_cfg_enter() {
    CH9120_UART.setTX(CH9120_TX_PIN);
    CH9120_UART.setRX(CH9120_RX_PIN);
    CH9120_UART.begin(CH9120_CFG_BAUD);
    while (CH9120_UART.available()) CH9120_UART.read();
    digitalWrite(CH9120_CFG_PIN, LOW);
    delay(500);
}

static void ch9120_cfg_exit(bool save_eeprom) {
    const uint8_t hdr[2] = {0x57, 0xAB};
    if (save_eeprom) {
        CH9120_UART.write(hdr, 2); CH9120_UART.write((uint8_t)0x0D);
        CH9120_UART.flush(); delay(200);
    }
    CH9120_UART.write(hdr, 2); CH9120_UART.write((uint8_t)0x0E);
    CH9120_UART.flush(); delay(200);
    CH9120_UART.write(hdr, 2); CH9120_UART.write((uint8_t)0x5E);
    CH9120_UART.flush(); delay(500);
    digitalWrite(CH9120_CFG_PIN, HIGH);
    CH9120_UART.setTX(CH9120_TX_PIN);
    CH9120_UART.setRX(CH9120_RX_PIN);
    CH9120_UART.begin(CH9120_DATA_BAUD);
}

static void ch9120_setup_tcp() {
    ch9120_cfg_enter();

    uint8_t mode = 0x01;  // TCP Client
    ch9120_raw_cmd(0x10, &mode, 1);

    uint8_t lip[]  = LOCAL_IP_BYTES;   ch9120_raw_cmd(0x11, lip,  4);
    uint8_t mask[] = LOCAL_MASK_BYTES; ch9120_raw_cmd(0x12, mask, 4);
    uint8_t gw[]   = LOCAL_GW_BYTES;   ch9120_raw_cmd(0x13, gw,   4);

    uint8_t lp[2] = {(uint8_t)(LOCAL_PORT & 0xFF), (uint8_t)(LOCAL_PORT >> 8)};
    ch9120_raw_cmd(0x14, lp, 2);

    uint8_t dip[] = SERVER_IP_BYTES;
    ch9120_raw_cmd(0x15, dip, 4);
    uint8_t dp[2] = {(uint8_t)(SERVER_PORT & 0xFF), (uint8_t)(SERVER_PORT >> 8)};
    ch9120_raw_cmd(0x16, dp, 2);

    uint32_t baud = CH9120_DATA_BAUD;
    uint8_t bd[4] = {(uint8_t)baud, (uint8_t)(baud>>8),
                     (uint8_t)(baud>>16), (uint8_t)(baud>>24)};
    ch9120_raw_cmd(0x21, bd, 4);

    ch9120_cfg_exit(true);
}

// ================================================================
// TCP helpers
// ================================================================
static bool tcp_connected() {
    return digitalRead(CH9120_TCPCS_PIN) == LOW;
}

static bool tcp_wait_connected(uint32_t timeout_ms) {
    uint32_t t0 = millis();
    while (!tcp_connected() && millis() - t0 < timeout_ms)
        delay(50);
    return tcp_connected();
}

// ================================================================
// Streaming: парсинг и отправка строк
// ================================================================

// Парсит одну строку данных хоста и шлёт по TCP.
// Формат входа: "891318   79.70    79.20 "
// Формат выхода: "891318:79.70;891319:79.20\r\n"
// Заголовочные строки (не начинаются с цифры) — игнорируются.
static void parse_and_send_line(const char* line, uint8_t len) {
    if (len == 0 || line[0] < '0' || line[0] > '9') {
        DBG_V("DBG_V  SKIP \"%.*s\"", (int)len, line);
        return;
    }

    uint8_t i = 0;

    // Энумератор
    uint32_t enum_val = 0;
    while (i < len && line[i] >= '0' && line[i] <= '9')
        enum_val = enum_val * 10 + (line[i++] - '0');
    while (i < len && line[i] == ' ') i++;

    // Вес 1
    char w1[12]; uint8_t w1n = 0;
    while (i < len && w1n < 11 && (line[i] >= '0' && line[i] <= '9' || line[i] == '.'))
        w1[w1n++] = line[i++];
    w1[w1n] = '\0';
    while (i < len && line[i] == ' ') i++;

    // Вес 2
    char w2[12]; uint8_t w2n = 0;
    while (i < len && w2n < 11 && (line[i] >= '0' && line[i] <= '9' || line[i] == '.'))
        w2[w2n++] = line[i++];
    w2[w2n] = '\0';

    if (w1n == 0 || w2n == 0) {
        DBG_V("DBG_V  PARSE ERR \"%.*s\"", (int)len, line);
        return;
    }

    char out[56];
    int n = snprintf(out, sizeof(out), "%lu:%s;%lu:%s\r\n",
                     (unsigned long)enum_val,      w1,
                     (unsigned long)(enum_val + 1), w2);
    if (n > 0) {
        DBG_V("DBG_V  SEND %.*s", n - 2, out);  // без \r\n
        CH9120_UART.write((const uint8_t*)out, n);
        CH9120_UART.flush();
    }
}

// Читает байты файла [from_pos, to_pos), накапливает строки в s_line_buf,
// при каждом \n парсит и шлёт.
static void stream_process_new(uint16_t start_cluster, uint32_t from_pos, uint32_t to_pos) {
    uint16_t cluster    = start_cluster;
    uint32_t clust_base = 0;

    // Пропускаем кластеры до from_pos
    while (cluster >= 2 && cluster < 0xFF8 && clust_base + SECTOR_SIZE <= from_pos) {
        clust_base += SECTOR_SIZE;
        cluster = fat12_next(cluster);
    }

    // Обрабатываем байты
    while (cluster >= 2 && cluster < 0xFF8 && clust_base < to_pos) {
        uint32_t sector = DATA_SECTOR + (cluster - 2);
        if (sector >= SECTOR_COUNT) break;
        const uint8_t* data = disk + sector * SECTOR_SIZE;

        uint32_t i0 = (from_pos > clust_base) ? (from_pos - clust_base) : 0;
        uint32_t i1 = (to_pos < clust_base + SECTOR_SIZE)
                      ? (to_pos - clust_base) : SECTOR_SIZE;

        for (uint32_t i = i0; i < i1; i++) {
            uint8_t c = data[i];
            if (c == '\r') continue;
            if (c == '\n') {
                s_line_buf[s_line_len] = '\0';
                parse_and_send_line(s_line_buf, s_line_len);
                s_line_len = 0;
            } else if (s_line_len < (uint8_t)(sizeof(s_line_buf) - 1)) {
                s_line_buf[s_line_len++] = (char)c;
            }
        }

        clust_base += SECTOR_SIZE;
        cluster = fat12_next(cluster);
    }
}

// ================================================================
// Debug: фреймированный [0x01][len][text]
// ================================================================
static void dbg_send(const char* msg) {
    uint8_t len = (uint8_t)min((size_t)255, strlen(msg));
    uint8_t hdr[2] = {0x01, len};
    CH9120_UART.write(hdr, 2);
    CH9120_UART.write((const uint8_t*)msg, len);
    CH9120_UART.flush();
}

static void dbg_sendf(const char* fmt, ...) {
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    dbg_send(buf);
}

// ================================================================
// LED (WS2812 RGB)
// ================================================================

// Градиент по заполнению диска: белый(0%) → зелёный(33%) → жёлтый(66%) → красный(100%)
static uint32_t write_fill_color() {
    const uint32_t data_bytes = (uint32_t)(SECTOR_COUNT - DATA_SECTOR) * SECTOR_SIZE;
    uint8_t pct = (g_processed_bytes == 0) ? 0
                : (uint8_t)min(100UL, (unsigned long)g_processed_bytes * 100UL / data_bytes);
    uint8_t r, g, b;
    if (pct < 33) {
        // белый → зелёный: убираем R и B, поднимаем G
        uint8_t t = pct * 255 / 33;
        r = 0x18 - (uint8_t)((uint16_t)0x18 * t / 255);
        g = 0x18 + (uint8_t)((uint16_t)0x18 * t / 255);
        b = 0x18 - (uint8_t)((uint16_t)0x18 * t / 255);
    } else if (pct < 66) {
        // зелёный → жёлтый: добавляем R
        uint8_t t = (pct - 33) * 255 / 33;
        r = (uint8_t)((uint16_t)0x30 * t / 255);
        g = 0x30;
        b = 0;
    } else {
        // жёлтый → красный: убираем G
        uint8_t t = (uint8_t)min(255, (int)(pct - 66) * 255 / 34);
        r = 0x30;
        g = 0x30 - (uint8_t)((uint16_t)0x30 * t / 255);
        b = 0;
    }
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static void led_set(uint32_t color) {
    pixel.setPixelColor(0, pixel.gamma32(color));
    pixel.show();
}

static void led_flash(uint8_t count, uint32_t color, uint16_t on_ms = 100, uint16_t off_ms = 150, bool loop = false) {
    flash_color  = color;
    flash_total  = count;
    flash_count  = 0;
    flash_on_ms  = on_ms;
    flash_off_ms = off_ms;
    flash_looped = loop;
    led_state    = false;
    led_timer    = 0;
    led_set(LED_OFF);
}

static void led_update() {
    // Физическая запись на диск — мигание 5 Гц, цвет = градиент заполнения.
    // Окно активности 500 мс после последней записи.
    // Период мигания по абсолютному времени — не зависит от частоты пакетов.
    if (millis() - g_led_write_ms < 500) {
        bool on = ((millis() / 100) % 2) == 0;
        led_set(on ? write_fill_color() : LED_OFF);
        return;
    }
    if (flash_total == 0) return;
    uint32_t now = millis();
    if (now - led_timer < (uint32_t)(led_state ? flash_on_ms : flash_off_ms)) return;
    led_timer = now;
    if (!led_state) {
        if (flash_count < flash_total) { led_state = true; led_set(flash_color); }
    } else {
        led_state = false; led_set(LED_OFF);
        if (++flash_count >= flash_total) {
            if (flash_looped) flash_count = 0; else flash_total = 0;
        }
    }
}

static void led_flash_sync(uint8_t count, uint32_t color, uint16_t on_ms = 150, uint16_t off_ms = 150) {
    for (uint8_t i = 0; i < count; i++) {
        led_set(color); delay(on_ms);
        led_set(LED_OFF); delay(off_ms);
    }
}

// ================================================================
// setup()
// ================================================================
void setup() {
    boot_ms = millis();

    pixel.begin();
    pixel.setBrightness(255);
    led_set(LED_OFF);

    pinMode(CH9120_CFG_PIN,   OUTPUT);
    pinMode(CH9120_RST_PIN,   OUTPUT);
    pinMode(CH9120_TCPCS_PIN, INPUT_PULLUP);
    digitalWrite(CH9120_CFG_PIN, HIGH);
    // CH9120 держим в RESET пока USB не энумерируется.
    // CH9120 в рабочем режиме потребляет ~60mA — вместе с RP2350 (~90mA)
    // превышаем лимит 100mA строгих хостов (VxWorks). В reset CH9120 < 5mA.
    digitalWrite(CH9120_RST_PIN, LOW);

    disk_init();

    // USB MSC — клон дескриптора рабочей FAT12-флешки (VendorCo 346D:5678).
    // bDeviceClass=0 уже выставлен патчем Adafruit_USBD_Device (CFG_TUD_CDC=0).
    TinyUSBDevice.setID(0x346D, 0x5678);
    TinyUSBDevice.setDeviceVersion(0x0200);        // bcdDevice 2.00
    TinyUSBDevice.setManufacturerDescriptor("USB");
    TinyUSBDevice.setProductDescriptor("Disk 2.0");
    // Серийник из уникального chip ID — каждая плата уникальна,
    // конфликтов при одновременном подключении нескольких плат нет.
    static char serial[17];
    snprintf(serial, sizeof(serial), "%016llX", rp2040.getChipID());
    TinyUSBDevice.setSerialDescriptor(serial);
    TinyUSBDevice.setConfigurationAttribute(0xC0);  // Self Powered

    usb_msc.setID("VendorCo", "ProductCode", "2.00"); // SCSI INQUIRY
    usb_msc.setCapacity(16777216, SECTOR_SIZE); // анонсируем 8 GB (как реальная VendorCo флешка)
    usb_msc.setReadWriteCallback(msc_read_cb, msc_write_cb, msc_flush_cb);
    usb_msc.setUnitReady(true);
    usb_msc.begin();

    // Ждём монтирования USB до 10 сек — CH9120 всё ещё в RESET (< 5mA).
    // После mount хост выделил порт → можно поднимать CH9120.
    for (int i = 0; i < 100 && !TinyUSBDevice.mounted(); i++) delay(100);

    // Поднимаем CH9120 и настраиваем TCP — теперь хост уже не ограничивает ток.
    digitalWrite(CH9120_RST_PIN, HIGH);
    delay(300);  // CH9120 startup
    ch9120_setup_tcp();

    // Ждём TCP (до TCP_CONNECT_TIMEOUT_MS)
    led_flash(255, LED_ORANGE, 100, 100, true);
    bool tcp_ok = tcp_wait_connected(TCP_CONNECT_TIMEOUT_MS);

    dbg_sendf("BOOT OK: uptime=%lu ms", (unsigned long)(millis() - boot_ms));
    {
        uint8_t lip[] = LOCAL_IP_BYTES;
        uint8_t sip[] = SERVER_IP_BYTES;
        dbg_sendf("BOOT: local=%d.%d.%d.%d:%d  server=%d.%d.%d.%d:%d",
            (int)lip[0],(int)lip[1],(int)lip[2],(int)lip[3],(int)LOCAL_PORT,
            (int)sip[0],(int)sip[1],(int)sip[2],(int)sip[3],(int)SERVER_PORT);
    }
    dbg_sendf("BOOT: USB %s  TCP %s",
        TinyUSBDevice.mounted() ? "MOUNTED" : "NOT MOUNTED",
        tcp_ok ? "CONNECTED" : "TIMEOUT");
    dbg_send("READY");

    if (tcp_ok) led_flash_sync(3, LED_WHITE, 80, 80);
    dbg_tcp_prev = tcp_ok;

    led_flash(255, tcp_ok ? LED_BLUE : LED_RED, 500, 500, true);
}

// ================================================================
// loop()
// ================================================================
void loop() {
    led_update();
#ifdef DEBUG_FS
    fs_log_drain();
#endif
#ifdef DEBUG_SCSI
    scsi_log_drain();
#endif

    // Heartbeat: каждые 5 сек шлём статус независимо от TCP-состояния.
    // Если received=0 в tcp_monitor даже после этого → CH9120 не пробрасывает UART→TCP.
    {
        uint32_t now = millis();
        if (now - hb_last_ms >= 5000) {
            hb_last_ms = now;
            dbg_sendf("HB: usb=%d tcp=%d scsi=%lu wr=%lu t=%lu",
                      (int)TinyUSBDevice.mounted(),
                      (int)tcp_connected(),
                      (unsigned long)hb_scsi_count,
                      (unsigned long)hb_wr_count,
                      (unsigned long)now);
        }
    }

    // Детект смены TCP
    bool tcp_now = tcp_connected();
    if (tcp_now != dbg_tcp_prev) {
        dbg_sendf("TCP %s", tcp_now ? "CONNECTED" : "DISCONNECTED");
        dbg_tcp_prev = tcp_now;
    }

    switch (state) {

    case State::IDLE:
        // LED: синий медленно = TCP OK; красный медленно = нет связи
        if (!tcp_now && flash_color != LED_RED)
            led_flash(255, LED_RED, 500, 500, true);
        else if (tcp_now && flash_color != LED_BLUE)
            led_flash(255, LED_BLUE, 500, 500, true);
        break;

    case State::STREAMING:
        // Гасим IDLE-мигание (красный/синий) — между записями LED тёмный,
        // во время записи write_fill_color() показывает градиент.
        if (flash_total != 0 && (flash_color == LED_RED || flash_color == LED_BLUE)) {
            flash_total = 0;
            led_set(LED_OFF);
        }

        #define DISK_DATA_BYTES ((uint32_t)(SECTOR_COUNT - DATA_SECTOR) * SECTOR_SIZE)
        // Аварийный сброс: диск >= 95% — немедленно, хост пишет без остановки
        if (g_processed_bytes >= (DISK_DATA_BYTES * DISK_FORCE_RESET_PCT / 100)) {
            disk_reset();
            break;
        }
        if (!g_write_pending && millis() - g_last_write_ms > STREAM_IDLE_RESET_MS) {
            if (g_processed_bytes >= (DISK_DATA_BYTES * DISK_RESET_FILL_PCT / 100)) {
                // Диск >= 80% + 10 сек тишины — плановый сброс
                disk_reset();
            } else {
                // Диск не заполнен — просто уходим в IDLE, файл остаётся
                state = State::IDLE;
                g_processed_bytes = 0;
                g_stream_cluster  = 0;
                s_line_len        = 0;
                led_flash(255, tcp_now ? LED_BLUE : LED_RED, 500, 500, true);
            }
            break;
        }

        // Ждём паузы WRITE_IDLE_MS после последней записи
        if (!g_write_pending) break;
        if (millis() - g_last_write_ms < WRITE_IDLE_MS) break;

        g_write_pending = false;

        {
            TxtFile f;
            if (!fat12_find_txt(f)) {
                // Файл пропал (диск переформатирован хостом?)
                dbg_send("STREAM: no file -> IDLE");
                g_stream_cluster  = 0;
                g_processed_bytes = 0;
                s_line_len        = 0;
                state = State::IDLE;
                led_flash(255, tcp_now ? LED_BLUE : LED_RED, 500, 500, true);
                break;
            }

            // Новый файл (другой cluster = хост создал новый сеанс)
            if (g_stream_cluster != 0 && f.start_cluster != g_stream_cluster) {
                dbg_sendf("STREAM: new file (clust %u->%u), reset",
                          g_stream_cluster, f.start_cluster);
                g_processed_bytes = 0;
                s_line_len        = 0;
            }
            g_stream_cluster = f.start_cluster;

            DBG_V("DBG_V  FILE clust=%u size=%lu processed=%lu new=%ld",
                  f.start_cluster, (unsigned long)f.file_size,
                  (unsigned long)g_processed_bytes,
                  (long)f.file_size - (long)g_processed_bytes);
#ifdef DEBUG_FS
            fs_dump_dirs();
#endif

            if (f.file_size > g_processed_bytes) {
                stream_process_new(f.start_cluster, g_processed_bytes, f.file_size);
                g_processed_bytes = f.file_size;
            }
        }
        break;
    }
}
