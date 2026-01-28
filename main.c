/*
 * AdaCon - Adaura Controller
 *
 */

#include <masc.h>

#include "adacom.h"
#include "tui.h"


typedef struct {
    const char *device;
    int pivot_attenuation;
} AdaConfig;

typedef struct {
    int log_level;
    AdaConfig ada;
} Config;


static Map *cmdline_args = NULL;
static Config cfg = {
    .log_level = LOG_INFO,
    .ada.device = "/dev/ttyUSB_ADAURA",
    .ada.pivot_attenuation = 60,
};
static int n_channels = 0;
static int current_channel = -1;
static double atten_interval = 5.0;


static void connect_cb(AdaComError err)
{
    if (err == ADACOM_OK) {
        current_channel = -1;
        n_channels = adacom_num_channels();
        tui_adacom_infos(adacom_model(), adacom_sn(), n_channels);
        for (int ch = 0; ch < n_channels; ch++) {
            tui_set_attenuation(ch, adacom_get_channel(ch));
        }
        log_info("Connected to %s (%s) with %i channels.",
                        adacom_model(), adacom_sn(), n_channels);
    }
    tui_adacom_state(adacom_state());
}

static void action_select_ch(int key) {
    int ch = key - '1';
    current_channel = tui_select_channel(ch);
}

static void action_shift_ch_left(int key) {
    if (adacom_state() != ADACOM_STATE_CONNECTED)
        return;
    if (current_channel <= 0) {
        current_channel = n_channels - 1;
    } else {
        current_channel--;
    }
    current_channel = tui_select_channel(current_channel);
}

static void action_shift_ch_right(int key) {
    if (adacom_state() != ADACOM_STATE_CONNECTED)
        return;
    if (current_channel >= n_channels - 1) {
        current_channel = 0;
    } else {
        current_channel++;
    }
    current_channel = tui_select_channel(current_channel);
}

static void atten_set_cb(AdaComError err, int ch, double value)
{
    if (err != ADACOM_OK) {
        log_error("Unable to set attenuation of channel %i!", ch);
        tui_adacom_state(adacom_state());
        return;
    }
    tui_set_attenuation(ch, value);
}

static void action_min_max_atten(int key)
{
    if (current_channel < 0 || adacom_state() != ADACOM_STATE_CONNECTED)
        return;
    double atten;
    if (key == TUI_KEY_PPAGE)
        atten = ADACOM_MAX_ATTENUATION;
    else
        atten = ADACOM_MIN_ATTENUATION;
    adacom_set_channel(current_channel, atten, atten_set_cb);
}

static double inc_dec_attenuation(double value, bool increase)
{
    int steps = value / atten_interval;
    return (increase ? steps + 1 : steps - 1) * atten_interval;
}

static void action_up_down_atten(int key) {
    if (current_channel < 0 || adacom_state() != ADACOM_STATE_CONNECTED)
        return;
    double atten = adacom_get_channel(current_channel);
    atten = inc_dec_attenuation(atten, key == TUI_KEY_UP);
    adacom_set_channel(current_channel, atten, atten_set_cb);
}

static void atten_set_all_cb(AdaComError err, double *values, int n)
{
    if (err != ADACOM_OK) {
        log_error("Unable to set all attenuations!");
        tui_adacom_state(adacom_state());
        return;
    }
    tui_set_attenuations(values, n);
}

static void action_ch_solo(int key) {
    if (current_channel < 0 || adacom_state() != ADACOM_STATE_CONNECTED)
        return;
    double values[n_channels];
    for (int ch = 0; ch < n_channels; ch++) {
        if (current_channel == ch) {
            values[ch] = ADACOM_MIN_ATTENUATION;
        } else {
            values[ch] = ADACOM_MAX_ATTENUATION;
        }
    }
    adacom_set_all(values, n_channels, atten_set_all_cb);
}

static void action_ch_solo_step(int key) {
    if (current_channel < 0 || adacom_state() != ADACOM_STATE_CONNECTED)
        return;
    double values[n_channels];
    // Get all channel attenuation values
    adacom_get_all(values, n_channels);
    // Calculate new attenuation for solo channel 
    double solo_ch = values[current_channel];
    solo_ch = inc_dec_attenuation(solo_ch, false);
    // Calculate minimal value for other channels
    double min_atten = 2 * cfg.ada.pivot_attenuation - solo_ch;
    for (int ch = 0; ch < n_channels; ch++) {
        if (current_channel == ch) {
            values[ch] = solo_ch;
        } else {
            if (values[ch] < min_atten) {
                values[ch] = min_atten;
            }
        }
    }
    adacom_set_all(values, n_channels, atten_set_all_cb);
}

static void set_all_channels_to(double value) {
    if (adacom_state() != ADACOM_STATE_CONNECTED)
        return;
    double values[n_channels];
    for (int ch = 0; ch < n_channels; ch++) {
        values[ch] = value;
    }
    adacom_set_all(values, n_channels, atten_set_all_cb);
}

static void action_all_min(int key) {
    set_all_channels_to(ADACOM_MIN_ATTENUATION);
}

static void action_all_max(int key) {
    set_all_channels_to(ADACOM_MAX_ATTENUATION);
}

static void action_connect(int key) {
    if (adacom_state() == ADACOM_STATE_CONNECTED) {
        log_info("Adaura already is connected.");
        return;
    }
    adacom_connect(connect_cb);
    tui_adacom_state(adacom_state());
}

static void action_disconnect(int key) {
    log_info("Disconnect from %s (%s).", adacom_model(), adacom_sn());
    tui_select_channel(-1);
    adacom_disconnect();
    tui_adacom_state(adacom_state());
    tui_adacom_infos(NULL, NULL, 0);
}

void *log_level_check(Str *log_level_str, Str **err_msg)
{
    Int *log_level = argparse_int(log_level_str, err_msg);
    if (log_level != NULL) {
        if (!int_in_range(log_level, 0, 7)) {
            *err_msg = str_new("invalid log level: %O!", log_level_str);
            delete(log_level);
            log_level = NULL;
        }
    }
    return log_level;
}

void *device_check(Str *path, Str **err_msg)
{
    if (!path_exists(str_cstr(path)))
    {
        *err_msg = str_new("device '%O' does not exist!", path);
        return NULL;
    }
    return new_copy(path);
}

static Map *parse_arguments(int argc, char *argv[])
{
    Map *args;
    const char *prog = path_basename(argv[0]);
    // Setup argument parser
    Argparse *ap = new(Argparse, prog, PROJECT_TITLE);
    // * Log level
    argparse_add_opt(ap, 'l', "log-level", "LEVEL", "1", log_level_check,
                     "log level (0 - 7)");
    // * Device name
    argparse_add_opt(ap, 'd', "device", "DEV", "1", device_check,
                     "path to serial device");
    // Parse command line arguments
    args = argparse_parse(ap, argc, argv);
    delete(ap);
    // Populate config structure
    Int *log_level = map_get(args, "log-level");
    if (!is_none(log_level)) {
        cfg.log_level = int_get(log_level);
    }
    Str *device = map_get(args, "device");
    if (!is_none(device)) {
        cfg.ada.device = str_cstr(device);
    }
    return args;
}

int main(int argc, char *argv[])
{
    cmdline_args = parse_arguments(argc, argv);
    log_init(cfg.log_level);
    mloop_init();
    tui_init();
    tui_add_action('x', action_disconnect);
    tui_add_action('c', action_connect);
    tui_add_action('m', action_all_max);
    tui_add_action('n', action_all_min);
    tui_add_action('s', action_ch_solo_step);
    tui_add_action('S', action_ch_solo);
    tui_add_action(TUI_KEY_UP, action_up_down_atten);
    tui_add_action(TUI_KEY_DOWN, action_up_down_atten);
    tui_add_action(TUI_KEY_PPAGE, action_min_max_atten);
    tui_add_action(TUI_KEY_NPAGE, action_min_max_atten);
    tui_add_action(TUI_KEY_RIGHT, action_shift_ch_right);
    tui_add_action(TUI_KEY_LEFT, action_shift_ch_left);
    tui_add_num_action(action_select_ch);
    adacom_init(cfg.ada.device);
    if (adacom_connect(connect_cb) != ADACOM_OK) {
        tui_adacom_state(adacom_state());
    }
    mloop_run();
    adacom_destroy();
    tui_destroy();
    delete(cmdline_args);
    return 0;
}


