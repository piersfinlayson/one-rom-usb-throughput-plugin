// Copyright (C) 2026 Piers Finlayson <piers@piers.rocks>
//
// MIT License

// One ROM USB throughput test
//
// Writes binary frames to log channel 0, which the USB plugin sends to the
// host's serial port.  Commands from the host are read from log channel 1.
// Replies are text on channel 0 between frames.
//
// Frame layout, little-endian:
//
//   0   u32  FRAME_MAGIC
//   4   u16  frame size in bytes, including this header and the CRC
//   6   u16  FRAME_LAYOUT
//   8   u32  sequence number
//   12  u32  plugin uptime when the frame was built, ms
//   16  u32  writes that failed because channel 0 was full, since start
//   20  u32  USB CRC errors, since start
//   24  u32  USB bit-stuff errors, since start
//   28  u32  flash passes, since start
//   32  u32  flash passes with the wrong hash, since start
//   36  ...  payload, xorshift32 words seeded from the sequence number
//   n-4 u32  CRC-32 (IEEE, as zlib) of bytes 0 to n-5
//
// Plugin v0.1.0 frames have 0 at offset 6 and the payload from offset 20.
// Plugin v0.1.1 frames have 1 at offset 6 and the payload from offset 28.
//
// A frame is built once and retried until there is room for it in channel 0,
// so the counters in it are from when it was built.  A gap in
// sequence numbers at the host is a frame lost after channel 0.
//
// The flash command reads a flash region through the uncached XIP alias
// whenever a frame isn't sent, so the flash bus is busy while the stream runs
// at full rate.  Each word is a separate QSPI transfer.  A pass hashes the
// region with FNV-1a over 32-bit words and is counted bad if the hash differs
// from the one provided by the host.

#include "plugin.h"

#define VERSION_MAJOR 0u
#define VERSION_MINOR 2u
#define VERSION_PATCH 0u

#if defined(DATA_DRIVE_MA)
void drive_main(ora_lookup_fn_t, ora_plugin_type_t, const ora_entry_args_t *);
#define PLUGIN_ENTRY drive_main
#else
#define PLUGIN_ENTRY plugin_main
#endif

ORA_DEFINE_USER_PLUGIN(
    PLUGIN_ENTRY,
    VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, 0,
    0, 8, 0         // ORA_ID_SET_STANDBY is new in firmware 0.8.0
);

#define FRAME_MAGIC         0x3CC3A55Au
#define FRAME_LAYOUT        2u
#define FRAME_HEADER_SIZE   36u
#define FRAME_MIN_SIZE      40u
#define FRAME_MAX_SIZE      256u
#define FRAME_DEFAULT_SIZE  64u

#define LINE_MAX            48u

// Both chip select windows.  The uncached alias doesn't allocate in the cache.
#define FLASH_START         0x10000000u
#define FLASH_END           0x12000000u
#define XIP_NOCACHE_OFFSET  0x04000000u
#define FLASH_BURST_WORDS   64u
#define FNV_BASIS           0x811C9DC5u
#define FNV_PRIME           0x01000193u

// A frame is retried from RAM this many times before the loop reads commands.
#define SEND_TRIES          1000u


// Credit is in byte-milliseconds so the rate is applied without a division.
// The cap limits the burst after a stall and keeps rate times elapsed ms within
// 32 bits.
#define RATE_MAX            2000000u
#define CREDIT_MAX_MS       100u

// The USB controller's SIE_STATUS register and its atomic clear alias.  TinyUSB
// in the USB plugin doesn't use the two error flags, so clearing them here
// doesn't affect it.
#define USB_SIE_STATUS      ((volatile uint32_t *)0x50110050u)
#define USB_SIE_STATUS_CLR  ((volatile uint32_t *)0x50113050u)
#define SIE_CRC_ERROR       (1u << 24)
#define SIE_BIT_STUFF_ERROR (1u << 25)

typedef struct {
    ora_yield_fn_t                yield;
    ora_log_write_fn_t            write;
    ora_log_read_fn_t             read;
    ora_get_plugin_uptime_ms_fn_t uptime;
    ora_led_set_fn_t              led_set;
    ora_set_standby_fn_t          set_standby;
    void                          (*flash_task)(void);


    uint8_t  streaming;
    uint32_t size;
    uint32_t rate;          // bytes per second, 0 for as fast as possible

    uint32_t seq;
    uint32_t full;
    uint32_t crc_errors;
    uint32_t stuff_errors;
    uint32_t start_ms;
    uint32_t stop_ms;
    uint32_t last_ms;
    uint32_t credit;
    uint8_t  frame_built;

    uint8_t  flash;
    const volatile uint32_t *flash_addr;
    uint32_t flash_words;
    uint32_t flash_hash_expected;
    uint32_t flash_pos;
    uint32_t flash_hash;
    uint32_t flash_passes;
    uint32_t flash_bad;

    uint32_t line_len;
    char     line[LINE_MAX];
} state_t;

static state_t s;

static const char channel_name[] = "usb-throughput";

// The stream rate must not depend on where the loop's code is in flash.  Code
// run from flash shares the XIP cache with the USB plugin's code, so moving the
// loop by a few bytes could change the stream rate by 15%.  The loop runs
// through the uncached XIP alias, where its fetches don't take cache lines,
// and the work it repeats for every frame runs from RAM, where it isn't slowed
// by uncached fetches.
//
// Calls from the loop must be PC-relative to stay in the uncached alias.
// check_loop.py fails the build where code other than plugin_main refers to a
// flash code address.
// Calls between flash and RAM are out of range of a PC-relative call.
#define RAMFUNC __attribute__((section(".ramfunc"), noinline, long_call))

static void init_ram(void) {
    extern uint32_t __ramfunc_start;
    extern uint32_t __ramfunc_end;
    extern uint32_t __ramfunc_load;
    extern uint32_t __bss_start;
    extern uint32_t __bss_end;
    const volatile uint32_t *src = &__ramfunc_load;
    volatile uint32_t *dst = &__ramfunc_start;
    while (dst < &__ramfunc_end) {
        *dst++ = *src++;
    }
    dst = &__bss_start;
    while (dst < &__bss_end) {
        *dst++ = 0u;
    }
}

// ---------------------------------------------------------------------------
// Text output
// ---------------------------------------------------------------------------

// Text is retried until there is room for it so a reply isn't lost while the
// channel is full of frames.
static void emit(const void *buf, uint32_t len) {
    while (s.write(ORA_LOG_CHANNEL_0, buf, len) == ORA_RESULT_LOG_FULL) {
        (void)s.yield(NULL);
    }
}

static uint32_t str_len(const char *str) {
    const volatile char *p = str;
    uint32_t n = 0u;
    while (*p++ != '\0') {
        n++;
    }
    return n;
}

static void out_str(const char *str) {
    emit(str, str_len(str));
}

static void out_u32(uint32_t v) {
    char buf[10];
    uint32_t i = sizeof(buf);
    do {
        buf[--i] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u);
    emit(&buf[i], sizeof(buf) - i);
}

static void out_end(void) {
    emit("\r\n", 2u);
}

static void out_line(const char *str) {
    out_str(str);
    out_end();
}

static const char *const menu[] = {
    "Commands:",
    "  help                  this menu",
    "  status                settings and counters",
    "  start                 start streaming frames",
    "  stop                  stop streaming",
    "  size <bytes>          frame size, 40 to 256, a multiple of 4",
    "  rate <bytes/s>        stream rate, 0 for as fast as possible",
    "  led <mode>            RGB LED: off on flame cycle breathe blink",
    "  serve <on|off>        ROM serving",
    "  flash <addr> <bytes> <hash>",
    "                        read flash, checking each pass's hash",
    "  flash off             stop reading flash",
};

static void print_menu(void) {
    for (uint32_t i = 0u; i < sizeof(menu) / sizeof(menu[0]); i++) {
        out_line(menu[i]);
    }
}

static void print_status(void) {
    uint32_t end = s.streaming ? s.uptime() : s.stop_ms;

    out_str("status streaming=");
    out_u32(s.streaming);
    out_str(" size=");
    out_u32(s.size);
    out_str(" rate=");
    out_u32(s.rate);
    out_str(" frames=");
    out_u32(s.seq);
    out_str(" full=");
    out_u32(s.full);
    out_str(" ms=");
    out_u32(end - s.start_ms);
    out_str(" crc=");
    out_u32(s.crc_errors);
    out_str(" stuff=");
    out_u32(s.stuff_errors);
    out_str(" flash_passes=");
    out_u32(s.flash_passes);
    out_str(" flash_bad=");
    out_u32(s.flash_bad);
    out_end();
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

// Inlined into build_frame, which runs from RAM.
static inline __attribute__((always_inline))
uint32_t crc32(const uint8_t *p, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0u; i < len; i++) {
        crc ^= p[i];
        for (uint32_t b = 0u; b < 8u; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

RAMFUNC static void build_frame(uint32_t *frame) {
    uint32_t words = s.size / 4u;

    frame[0] = FRAME_MAGIC;
    frame[1] = s.size | (FRAME_LAYOUT << 16);
    frame[2] = s.seq;
    frame[3] = s.uptime();
    frame[4] = s.full;
    frame[5] = s.crc_errors;
    frame[6] = s.stuff_errors;
    frame[7] = s.flash_passes;
    frame[8] = s.flash_bad;

    // xorshift32 started at 0 stays at 0.
    uint32_t x = s.seq ^ 0x9E3779B9u;
    if (x == 0u) {
        x = 1u;
    }
    for (uint32_t i = FRAME_HEADER_SIZE / 4u; i < words - 1u; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        frame[i] = x;
    }

    frame[words - 1u] = crc32((const uint8_t *)frame, s.size - 4u);
}

// Returns 1 if the frame was sent, or 0 if the channel was full for every try.
RAMFUNC static uint8_t send_frame(const uint32_t *frame) {
    for (uint32_t i = 0u; i < SEND_TRIES; i++) {
        if (s.write(ORA_LOG_CHANNEL_0, frame, s.size) != ORA_RESULT_LOG_FULL) {
            return 1u;
        }
        s.full++;
        (void)s.yield(NULL);
        if (s.flash) {
            s.flash_task();
        }
    }
    return 0u;
}

// Sends at most one frame so commands are read between frames.  Returns 1 if
// it sent one.
static uint8_t stream_task(uint32_t *frame) {
    if (s.rate != 0u) {
        uint32_t now = s.uptime();
        uint32_t elapsed = now - s.last_ms;
        uint32_t cap = s.rate * CREDIT_MAX_MS;
        if (cap < s.size * 1000u) {
            cap = s.size * 1000u;
        }
        s.last_ms = now;
        if (elapsed > CREDIT_MAX_MS) {
            elapsed = CREDIT_MAX_MS;
        }
        s.credit += s.rate * elapsed;
        if (s.credit > cap) {
            s.credit = cap;
        }
        if (s.credit < s.size * 1000u) {
            return 0u;
        }
    }

    if (!s.frame_built) {
        build_frame(frame);
        s.frame_built = 1u;
    }
    if (!send_frame(frame)) {
        return 0u;
    }

    s.frame_built = 0u;
    s.seq++;
    if (s.rate != 0u) {
        s.credit -= s.size * 1000u;
    }
    return 1u;
}

// ---------------------------------------------------------------------------
// Flash
// ---------------------------------------------------------------------------

static void flash_restart(void) {
    s.flash_pos = 0u;
    s.flash_hash = FNV_BASIS;
    s.flash_passes = 0u;
    s.flash_bad = 0u;
}

static void flash_task(void) {
    uint32_t pos = s.flash_pos;
    uint32_t hash = s.flash_hash;
    uint32_t end = pos + FLASH_BURST_WORDS;
    if (end > s.flash_words) {
        end = s.flash_words;
    }
    for (; pos < end; pos++) {
        hash = (hash ^ s.flash_addr[pos]) * FNV_PRIME;
    }
    if (pos == s.flash_words) {
        s.flash_passes++;
        if (hash != s.flash_hash_expected) {
            s.flash_bad++;
        }
        pos = 0u;
        hash = FNV_BASIS;
    }
    s.flash_pos = pos;
    s.flash_hash = hash;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

static uint8_t str_eq(const char *a, const char *b) {
    while ((*a != '\0') && (*a == *b)) {
        a++;
        b++;
    }
    return *a == *b;
}

// Returns 0 if str isn't a decimal or 0x-prefixed hex number that fits in 32
// bits.
static uint8_t parse_u32(const char *str, uint32_t *out) {
    uint32_t base = 10u;
    uint32_t v = 0u;
    if ((str[0] == '0') && (str[1] == 'x')) {
        base = 16u;
        str += 2;
    }
    if (*str == '\0') {
        return 0u;
    }
    for (; *str != '\0'; str++) {
        uint32_t d;
        if ((*str >= '0') && (*str <= '9')) {
            d = (uint32_t)(*str - '0');
        } else if ((base == 16u) && (*str >= 'a') && (*str <= 'f')) {
            d = (uint32_t)(*str - 'a') + 10u;
        } else if ((base == 16u) && (*str >= 'A') && (*str <= 'F')) {
            d = (uint32_t)(*str - 'A') + 10u;
        } else {
            return 0u;
        }
        if (v > (0xFFFFFFFFu - d) / base) {
            return 0u;
        }
        v = (v * base) + d;
    }
    *out = v;
    return 1u;
}

// Splits off the first space-separated word of *str and returns it.
static char *next_word(char **str) {
    char *word = *str;
    char *p = word;
    while ((*p != '\0') && (*p != ' ')) {
        p++;
    }
    if (*p == ' ') {
        *p++ = '\0';
        while (*p == ' ') {
            p++;
        }
    }
    *str = p;
    return word;
}

static void reply_ok(void) {
    out_line("ok");
}

static void reply_err(const char *msg) {
    out_str("error ");
    out_line(msg);
}

static void cmd_start(void) {
    s.seq = 0u;
    s.frame_built = 0u;
    s.full = 0u;
    s.crc_errors = 0u;
    s.stuff_errors = 0u;
    flash_restart();
    s.credit = 0u;
    s.start_ms = s.uptime();
    s.last_ms = s.start_ms;
    reply_ok();
    s.streaming = 1u;
}

static void cmd_stop(void) {
    if (s.streaming) {
        s.streaming = 0u;
        s.stop_ms = s.uptime();
    }
    reply_ok();
    print_status();
}

static void cmd_size(const char *arg) {
    uint32_t v;
    if (!parse_u32(arg, &v) || (v < FRAME_MIN_SIZE) || (v > FRAME_MAX_SIZE) ||
        ((v % 4u) != 0u)) {
        reply_err("size is 32 to 256, a multiple of 4");
        return;
    }
    s.size = v;
    s.frame_built = 0u;
    reply_ok();
}

static void cmd_rate(const char *arg) {
    uint32_t v;
    if (!parse_u32(arg, &v) || (v > RATE_MAX)) {
        reply_err("rate is 0 to 2000000");
        return;
    }
    s.rate = v;
    s.credit = 0u;
    s.last_ms = s.uptime();
    reply_ok();
}

static const struct {
    const char *name;
    uint8_t     mode;
} led_modes[] = {
    { "off",     ORA_LED_MODE_OFF },
    { "on",      ORA_LED_MODE_ON },
    { "flame",   ORA_LED_MODE_FLAME },
    { "cycle",   ORA_LED_MODE_CYCLE },
    { "breathe", ORA_LED_MODE_BREATHE },
    { "blink",   ORA_LED_MODE_BLINK },
};

static void cmd_led(const char *arg) {
    for (uint32_t i = 0u; i < sizeof(led_modes) / sizeof(led_modes[0]); i++) {
        if (!str_eq(arg, led_modes[i].name)) {
            continue;
        }
        ora_led_request_t req = {
            .size  = sizeof(req),
            .led   = ORA_LED_RGB,
            .mode  = led_modes[i].mode,
            .red   = 255u,
            .green = 255u,
            .blue  = 255u,
        };
        ora_result_t result = s.led_set(&req);
        if (result != ORA_RESULT_OK) {
            out_str("error led_set returned ");
            out_u32((uint32_t)result);
            out_end();
            return;
        }
        reply_ok();
        return;
    }
    reply_err("led mode is off on flame cycle breathe blink");
}

static void cmd_serve(const char *arg) {
    uint8_t standby;
    if (str_eq(arg, "on")) {
        standby = 0u;
    } else if (str_eq(arg, "off")) {
        standby = 1u;
    } else {
        reply_err("serve is on or off");
        return;
    }
    ora_result_t result = s.set_standby(standby, 0u);
    if (result != ORA_RESULT_OK) {
        out_str("error set_standby returned ");
        out_u32((uint32_t)result);
        out_end();
        return;
    }
    reply_ok();
}

static void cmd_flash(char *arg) {
    if (str_eq(arg, "off")) {
        s.flash = 0u;
        reply_ok();
        return;
    }
    uint32_t addr, bytes, hash;
    if (!parse_u32(next_word(&arg), &addr) ||
        !parse_u32(next_word(&arg), &bytes) ||
        !parse_u32(next_word(&arg), &hash) || (*arg != '\0') ||
        (addr < FLASH_START) || (addr >= FLASH_END) || ((addr % 4u) != 0u) ||
        (bytes == 0u) || ((bytes % 4u) != 0u) ||
        (bytes > FLASH_END - addr)) {
        reply_err("flash is off, or <addr> <bytes> <hash> within 0x10000000 to"
                  " 0x12000000, word aligned");
        return;
    }
    s.flash_addr = (const volatile uint32_t *)(addr + XIP_NOCACHE_OFFSET);
    s.flash_words = bytes / 4u;
    s.flash_hash_expected = hash;
    flash_restart();
    s.flash = 1u;
    reply_ok();
}

static void run_command(char *line) {
    char *arg = line;
    line = next_word(&arg);

    if (line[0] == '\0') {
        return;
    } else if (str_eq(line, "help")) {
        print_menu();
    } else if (str_eq(line, "status")) {
        print_status();
    } else if (str_eq(line, "start")) {
        cmd_start();
    } else if (str_eq(line, "stop")) {
        cmd_stop();
    } else if (str_eq(line, "size")) {
        cmd_size(arg);
    } else if (str_eq(line, "rate")) {
        cmd_rate(arg);
    } else if (str_eq(line, "led")) {
        cmd_led(arg);
    } else if (str_eq(line, "serve")) {
        cmd_serve(arg);
    } else if (str_eq(line, "flash")) {
        cmd_flash(arg);
    } else {
        reply_err("unknown command, type help");
    }
}

// Reads a line from channel 1 a byte at a time and runs it at CR or LF.
// A line longer than LINE_MAX - 1 is discarded.
static void input_task(void) {
    uint8_t c;
    uint32_t got = 0u;
    if ((s.read(ORA_LOG_CHANNEL_1, &c, 1u, &got) != ORA_RESULT_OK) ||
        (got == 0u)) {
        return;
    }

    if ((c == '\r') || (c == '\n')) {
        if (s.line_len < LINE_MAX) {
            s.line[s.line_len] = '\0';
            run_command(s.line);
        } else {
            reply_err("line too long");
        }
        s.line_len = 0u;
    } else if (s.line_len < LINE_MAX - 1u) {
        s.line[s.line_len++] = (char)c;
    } else {
        s.line_len = LINE_MAX;
    }
}

// Several errors between two checks count as one, so a count is a lower bound.
static void error_task(void) {
    uint32_t flags = *USB_SIE_STATUS & (SIE_CRC_ERROR | SIE_BIT_STUFF_ERROR);
    if (flags == 0u) {
        return;
    }
    if (flags & SIE_CRC_ERROR) {
        s.crc_errors++;
    }
    if (flags & SIE_BIT_STUFF_ERROR) {
        s.stuff_errors++;
    }
    *USB_SIE_STATUS_CLR = flags;
}

// ---------------------------------------------------------------------------
// Entry
// ---------------------------------------------------------------------------

// The frame is on the stack, as static RAM is too small for both it and the RAM
// functions.
__attribute__((noinline)) static void stream_loop(void) {
    uint32_t frame[FRAME_MAX_SIZE / 4u];
    for (;;) {
        (void)s.yield(NULL);
        error_task();
        input_task();
        uint8_t sent = 0u;
        if (s.streaming) {
            sent = stream_task(frame);
        }
        if (s.flash && !sent) {
            s.flash_task();
        }
    }
}

static void park(void) {
    for (;;) {
        if (s.yield != NULL) {
            (void)s.yield(NULL);
        }
    }
}

void plugin_main(
    ora_lookup_fn_t ora_lookup_fn,
    ora_plugin_type_t plugin_type,
    const ora_entry_args_t *entry_args
) {
    (void)plugin_type;
    (void)entry_args;

    init_ram();

    s.yield       = ora_lookup_fn(ORA_ID_YIELD);
    s.write       = ora_lookup_fn(ORA_ID_LOG_WRITE);
    s.read        = ora_lookup_fn(ORA_ID_LOG_READ);
    s.uptime      = ora_lookup_fn(ORA_ID_GET_PLUGIN_UPTIME_MS);
    s.led_set     = ora_lookup_fn(ORA_ID_LED_SET);
    s.set_standby = ora_lookup_fn(ORA_ID_SET_STANDBY);
    ora_log_open_write_fn_t open_write = ora_lookup_fn(ORA_ID_LOG_OPEN_WRITE);
    ora_log_open_read_fn_t  open_read  = ora_lookup_fn(ORA_ID_LOG_OPEN_READ);

    if ((s.yield == NULL) || (s.write == NULL) || (s.read == NULL) ||
        (s.uptime == NULL) || (s.led_set == NULL) || (s.set_standby == NULL) ||
        (open_write == NULL) || (open_read == NULL)) {
        park();
    }

    if (open_write(ORA_LOG_CHANNEL_0, channel_name) != ORA_RESULT_OK) {
        park();
    }

    out_str("One ROM USB Throughput Test v");
    out_u32(VERSION_MAJOR);
    out_str(".");
    out_u32(VERSION_MINOR);
    out_str(".");
    out_u32(VERSION_PATCH);
    out_end();

    if (open_read(ORA_LOG_CHANNEL_1) != ORA_RESULT_OK) {
        out_line("Can't read commands from log channel 1.");
        park();
    }

    s.size = FRAME_DEFAULT_SIZE;
    print_menu();

    // RAM can't hold flash_task as well, and uncached it reads flash at a sixth
    // of the rate, so it runs through the cache.  The stream rate while it runs
    // depends on where its code is.
    s.flash_task = flash_task;
    void (*loop)(void) =
        (void (*)(void))((uintptr_t)stream_loop + XIP_NOCACHE_OFFSET);
    loop();
}

#if defined(DATA_DRIVE_MA)
#if DATA_DRIVE_MA == 2
#define DATA_DRIVE_FIELD 0u
#elif DATA_DRIVE_MA == 4
#define DATA_DRIVE_FIELD 1u
#elif DATA_DRIVE_MA == 8
#define DATA_DRIVE_FIELD 2u
#elif DATA_DRIVE_MA == 12
#define DATA_DRIVE_FIELD 3u
#else
#error "DATA_DRIVE_MA must be 2, 4, 8 or 12"
#endif

#define DATA_PINS_MAX       16u
#define PADS_BANK0_BASE     0x40038000u
#define PADS_SET_OFFSET     0x2000u
#define PADS_CLR_OFFSET     0x3000u
#define PAD_SLEW_FAST       (1u << 0)
#define PAD_DRIVE_SHIFT     4u
#define PAD_DRIVE_MASK      (0x3u << PAD_DRIVE_SHIFT)

static volatile uint32_t *pad(uint8_t gpio, uint32_t alias) {
    return (volatile uint32_t *)(PADS_BANK0_BASE + alias + 4u + (4u * gpio));
}

void drive_main(
    ora_lookup_fn_t ora_lookup_fn,
    ora_plugin_type_t plugin_type,
    const ora_entry_args_t *entry_args
) {
    ora_get_data_pin_nums_fn_t get_data_pins =
        ora_lookup_fn(ORA_ID_GET_DATA_PIN_NUMS);
    uint8_t pins[DATA_PINS_MAX];
    uint8_t num_pins = 0u;
    if (get_data_pins != NULL) {
        num_pins = get_data_pins(pins, DATA_PINS_MAX);
    }
    for (uint32_t i = 0u; i < num_pins; i++) {
        *pad(pins[i], PADS_CLR_OFFSET) = PAD_DRIVE_MASK | PAD_SLEW_FAST;
        *pad(pins[i], PADS_SET_OFFSET) = (DATA_DRIVE_FIELD << PAD_DRIVE_SHIFT) |
                                         (DATA_SLEW_FAST ? PAD_SLEW_FAST : 0u);
    }
    plugin_main(ora_lookup_fn, plugin_type, entry_args);
}
#endif // DATA_DRIVE_MA
