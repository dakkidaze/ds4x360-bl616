#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Configuration body — byte-layout identical to DS4Dongle's Config_body
 * (version 6) so tools/config_tool.py keeps working unchanged.
 * Audio-related fields are preserved but unused until audio support (P2).
 */
struct __attribute__((packed)) config_body {
    uint8_t config_version;
    float   haptics_gain;         /* [1.0,2.0]  — unused on DS4 (kept for layout) */
    uint8_t speaker_volume;       /* [0,127]    — unused (seeds USB GET_CUR) */
    uint8_t headset_volume;       /* [0,127]    — unused */
    uint8_t speaker_gain;         /* [0,7]      — unused (0: auto) */
    uint8_t inactive_time;        /* [0,60] min (0 = disable) */
    uint8_t disable_led;          /* bool — LED off when connected */
    uint8_t polling_rate_mode;    /* 0: 250Hz, 1: 500Hz, 2: real-time */
    uint8_t audio_buffer_length;  /* [16,127]   — unused */
    uint8_t controller_mode;      /* unused (DS5 legacy, kept for layout) */
    uint8_t enable_usb_sn;        /* bool — expose USB serial number */
    uint8_t ps_shortcut_enabled;  /* bool — PS button → Win+G / Win+Tab (P3) */
    uint8_t mic_select;           /* 0: auto, 1: builtin, 2: headphone, 3: disable */
    uint8_t speaker_select;       /* 0: auto, 1: builtin, 2: headphone, 3: disable */
    uint8_t enable_wake;          /* bool — wake host on PS press (P3) */
    uint8_t trigger_reduce;       /* [0,10] — unused on DS4 (kept for layout) */
    uint8_t lock_volume;          /* bool — strip volume-enable bits from host output */
    uint8_t audio_follow_jack;    /* bool — audio device only while headset plugged (P2) */
};

#define CONFIG_VERSION  6

void config_load(void);
bool config_save(void);
void config_validate(void);

struct config_body *config_get(void);
void config_set(const uint8_t *data, uint16_t len);

/* Convenience accessors for hot-path fields */
static inline bool config_wake_enabled(void)       { return config_get()->enable_wake; }
static inline bool config_led_disabled(void)        { return config_get()->disable_led; }
static inline uint8_t config_inactive_minutes(void) { return config_get()->inactive_time; }
static inline uint8_t config_polling_mode(void)     { return config_get()->polling_rate_mode; }
static inline bool config_ps_shortcut(void)         { return config_get()->ps_shortcut_enabled; }
static inline bool config_usb_sn(void)              { return config_get()->enable_usb_sn; }
static inline bool config_lock_volume(void)         { return config_get()->lock_volume; }
static inline uint8_t config_mic_select(void)       { return config_get()->mic_select; }
static inline uint8_t config_speaker_select(void)   { return config_get()->speaker_select; }

#endif /* CONFIG_H */
