#include "config.h"
#include <string.h>
#include "debug_log.h"
#include <math.h>

extern int bt_settings_set_bin(const char *key, const uint8_t *value,
                               size_t length);
extern int bt_settings_get_bin(const char *key, uint8_t *value,
                               size_t exp_len, size_t *real_len);

#define CONFIG_EF_KEY  "dongle_cfg"

static struct config_body cfg;

void config_validate(void)
{
    struct config_body *b = &cfg;

    if (b->config_version != CONFIG_VERSION) {
        LOG_WRN("[CFG] Version mismatch (%d vs %d), resetting\n",
               b->config_version, CONFIG_VERSION);
        memset(b, 0, sizeof(*b));
        b->config_version = CONFIG_VERSION;
        b->haptics_gain      = 1.0f;
        b->speaker_volume    = 100;
        b->headset_volume    = 100;
        b->speaker_gain      = 2;
        b->inactive_time     = 30;
        b->audio_buffer_length = 64;
        b->controller_mode   = 2;
        b->audio_follow_jack = 1;
    }

    if (isnan(b->haptics_gain) || b->haptics_gain < 1.0f || b->haptics_gain > 2.0f)
        b->haptics_gain = 1.0f;
    if (b->speaker_volume > 127)
        b->speaker_volume = 100;
    if (b->headset_volume > 127)
        b->headset_volume = 100;
    if (b->speaker_gain > 7)
        b->speaker_gain = 2;
    if (b->inactive_time > 60)
        b->inactive_time = 30;
    if (b->disable_led > 1)
        b->disable_led = 0;
    if (b->polling_rate_mode > 2)
        b->polling_rate_mode = 0;
    if (b->audio_buffer_length < 16 || b->audio_buffer_length > 127)
        b->audio_buffer_length = 64;
    if (b->controller_mode > 2)
        b->controller_mode = 2;
    if (b->enable_usb_sn > 1)
        b->enable_usb_sn = 0;
    if (b->ps_shortcut_enabled > 1)
        b->ps_shortcut_enabled = 0;
    if (b->mic_select > 3)
        b->mic_select = 0;
    if (b->speaker_select > 3)
        b->speaker_select = 0;
    if (b->enable_wake > 1)
        b->enable_wake = 0;
    if (b->trigger_reduce > 10)
        b->trigger_reduce = 0;
    if (b->lock_volume > 1)
        b->lock_volume = 0;
    if (b->audio_follow_jack > 1)
        b->audio_follow_jack = 1;
}

static void config_defaults(void)
{
    memset(&cfg, 0, sizeof(cfg));
    cfg.config_version      = CONFIG_VERSION;
    cfg.haptics_gain        = 1.0f;
    cfg.speaker_volume      = 100;
    cfg.headset_volume      = 100;
    cfg.speaker_gain        = 2;
    cfg.inactive_time       = 30;
    cfg.disable_led         = 0;
    cfg.polling_rate_mode   = 0;     /* 250 Hz (matches stock DS4 v2) */
    cfg.audio_buffer_length = 64;
    cfg.controller_mode     = 2;     /* DS5 legacy field, inert on DS4 */
    cfg.enable_usb_sn       = 0;
    cfg.ps_shortcut_enabled = 0;
    cfg.mic_select          = 0;
    cfg.speaker_select      = 0;
    cfg.enable_wake         = 0;
    cfg.trigger_reduce      = 0;
    cfg.lock_volume         = 0;
    cfg.audio_follow_jack   = 1;
}

void config_load(void)
{
    size_t rlen = 0;
    memset(&cfg, 0, sizeof(cfg));
    int err = bt_settings_get_bin(CONFIG_EF_KEY, (uint8_t *)&cfg,
                                  sizeof(cfg), &rlen);
    if (err != 0 || rlen < sizeof(cfg)) {
        LOG_INF("[CFG] No saved config, using defaults\n");
        config_defaults();
    }
    config_validate();
    LOG_INF("[CFG] Loaded: wake=%d led_off=%d inactive=%dmin poll=%d ps=%d jack=%d\n",
           cfg.enable_wake, cfg.disable_led, cfg.inactive_time,
           cfg.polling_rate_mode, cfg.ps_shortcut_enabled,
           cfg.audio_follow_jack);
}

bool config_save(void)
{
    int rc = bt_settings_set_bin(CONFIG_EF_KEY, (const uint8_t *)&cfg,
                                 sizeof(cfg));
    if (rc == 0) {
        LOG_INF("[CFG] Saved to flash\n");
        return true;
    }
    LOG_ERR("[CFG] Save failed: %d\n", rc);
    return false;
}

struct config_body *config_get(void)
{
    return &cfg;
}

void config_set(const uint8_t *data, uint16_t len)
{
    uint16_t copy_len = len < sizeof(cfg) ? len : sizeof(cfg);
    memcpy(&cfg, data, copy_len);
    config_validate();
    /* R01: reached from the HID SET_REPORT/EP0 path — no printf here. */
}
