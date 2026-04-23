/*
 * AdaCon - Adaura Controller
 *
 */

#include <masc.h>

#include "cfg.h"
#include "adacom.h"
#include "tui.h"


typedef enum {
    ADACON_STATE_STOPPED,
    ADACON_STATE_PLAY_SINGLE,
    ADACON_STATE_PLAY_COUNTINOUS
} AdaConState;

typedef enum {
    HANDOFF_STATE_ACTIVE,
    HANDOFF_STATE_RECOVER
} HandoffState;


static AdaConState state = ADACON_STATE_STOPPED;
static int n_channels = 0;
static int current_channel = -1;
static int ctrl_chs[ADACOM_MAX_CHANNELS];
static int n_ctrl_chs = 0;
static double atten_interval = 5.0;
static MlTimer *play_timer = NULL;
static HandoffState ho_state;
static int ho_ctrl_ch_idx = -1;
static int ho_interval;
static int ho_start;


static void action_select_ch(int key) {
    if (state != ADACON_STATE_STOPPED)
        return;
    int ch = key - '1';
    current_channel = tui_select_channel(ch);
}

static void action_shift_ch_left(int key) {
    if (state != ADACON_STATE_STOPPED)
        return;
    if (current_channel <= 0) {
        current_channel = n_channels - 1;
    } else {
        current_channel--;
    }
    current_channel = tui_select_channel(current_channel);
}

static void action_shift_ch_right(int key) {
    if (state != ADACON_STATE_STOPPED)
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

static void atten_set_all_cb(AdaComError err, double *values, int n)
{
    if (err != ADACOM_OK) {
        log_error("Unable to set all attenuations!");
        tui_adacom_state(adacom_state());
        return;
    }
    tui_set_attenuations(values, n);
}


static List *get_group_by_channel(int channel)
{
    List *group = NULL;
    Iter itr = init(Iter, cfg.groups);
    for (List *grp = next(&itr); grp != NULL; grp = next(&itr)) {
        Iter jtr = init(Iter, grp);
        for (Int *c = next(&jtr); c != NULL; c = next(&jtr)) {
            if (c->val == channel) {
                group = grp;
                break;
            }
        }
        destroy(&jtr);
    }
    destroy(&itr);
    return group;
}

static void group_set_channels(List *group, double *values, double atten)
{
    Iter itr = init(Iter, group);
    for (Int *c = next(&itr); c != NULL; c = next(&itr)) {
        if (c->val < n_channels) {
            values[c->val] = atten;
        }
    }
    destroy(&itr);
}

static void set_all_in_same_group(int ch, double *values, double atten)
{
    List *group = get_group_by_channel(ch);
    if (group == NULL) {
        values[ch] = atten;
        return;
    }
    group_set_channels(group, values, atten);
}

static void set_group(int ch, double atten)
{
    List *group = get_group_by_channel(ch);
    if (group == NULL) {
        // Channel is in no group, set in and leave.
        adacom_set_channel(current_channel, atten, atten_set_cb);
        return;
    }
    // Get all channel attenuation values
    double values[n_channels];
    adacom_get_all(values, n_channels);
    // Change value of the channels in the same group
    group_set_channels(group, values, atten);
    // Set all channels
    adacom_set_all(values, n_channels, atten_set_all_cb);
}

static void action_min_max_atten(int key)
{
    if (current_channel < 0 || state != ADACON_STATE_STOPPED ||
            adacom_state() != ADACOM_STATE_CONNECTED)
        return;
    double atten;
    if (key == TUI_KEY_PPAGE)
        atten = cfg.max_attenuation;
    else
        atten = cfg.min_attenuation;
    set_group(current_channel, atten);
}

static double inc_dec_attenuation(double value, bool increase)
{
    int steps = value / atten_interval;
    return (increase ? steps + 1 : steps - 1) * atten_interval;
}

static void action_up_down_atten(int key) {
    if (current_channel < 0 || state != ADACON_STATE_STOPPED ||
            adacom_state() != ADACOM_STATE_CONNECTED)
        return;
    double atten = adacom_get_channel(current_channel);
    atten = inc_dec_attenuation(atten, key == TUI_KEY_UP);
    set_group(current_channel, atten);
}

static void action_ch_solo(int key) {
    if (current_channel < 0 || state != ADACON_STATE_STOPPED ||
            adacom_state() != ADACOM_STATE_CONNECTED)
        return;
    double values[n_channels];
    adacom_get_all(values, n_channels);
    // Set all channels in channels except the solo chanel to max attenuation
    for (int i = 0; i < n_ctrl_chs; i++) {
        int ch = ctrl_chs[i];
        set_all_in_same_group(ch, values, cfg.max_attenuation);
    }
    // Set all channels in the same group as current channel to min attenuation
    set_all_in_same_group(current_channel, values, cfg.min_attenuation);
    adacom_set_all(values, n_channels, atten_set_all_cb);
}

static void set_solo_and_others(int solo_ch, double solo_val, double *values)
{
    // Calculate minimal value for other channels ...
    double min_atten;
    if (solo_val > cfg.min_attenuation) {
        // ... around the pivot point.
        min_atten = 2 * cfg.pivot_attenuation - solo_val;
    } else {
        // ... if the solo channel reaches the minimal attenuation, raise all
        // other channels to their maximum attenuation.
        min_atten = cfg.max_attenuation;
    }
    // Set all calculated attenuation values
    for (int i = 0; i < n_ctrl_chs; i++) {
        int ch = ctrl_chs[i];
        if (values[ch] < min_atten) {
            set_all_in_same_group(ch, values, min_atten);
        }
    }
    set_all_in_same_group(solo_ch, values, solo_val);
}

static void action_ch_solo_step(int key) {
    if (current_channel < 0 || state != ADACON_STATE_STOPPED ||
            adacom_state() != ADACOM_STATE_CONNECTED)
        return;
    double values[n_channels];
    // Get all channel attenuation values
    adacom_get_all(values, n_channels);
    // Calculate new attenuation for solo channel
    double solo_val = values[current_channel];
    solo_val = inc_dec_attenuation(solo_val, false);
    set_solo_and_others(current_channel, solo_val, values);
    adacom_set_all(values, n_channels, atten_set_all_cb);
}

static int get_ctrl_ch_idx(int channel)
{
    for (int i = 0; i < n_ctrl_chs; i++) {
        if (channel == ctrl_chs[i])
            return i;
    }
    return -1;
}

static int next_ho_channel(void)
{
    if (++ho_ctrl_ch_idx >= n_ctrl_chs) {
        ho_ctrl_ch_idx = 0;
    }
    return ctrl_chs[ho_ctrl_ch_idx];
}

static void player_cb(MlTimer *timer, void *arg)
{
    double values[n_channels];
    int ho_time = mloop_run_time() - ho_start;
    adacom_get_all(values, n_channels);
    // Calculate new attenuation for solo channel
    int solo_ch = ctrl_chs[ho_ctrl_ch_idx];
    double ho_progress = (double)ho_time / cfg.action_time;
    double solo_val = cfg.max_attenuation \
            - (cfg.max_attenuation - cfg.min_attenuation) * ho_progress;
    log_debug("player: time: %i ms, ch: %i, atten: %.2f",
            ho_time, solo_ch, solo_val);
    if (solo_val < values[solo_ch]) {
        set_solo_and_others(solo_ch, solo_val, values);
        adacom_set_all(values, n_channels, atten_set_all_cb);
    }
    // Decide the next step in the handoff sequence.
    if (ho_time < cfg.action_time) {
        ml_timer_add(play_timer, ho_interval);
    } else {
        current_channel = tui_select_channel(next_ho_channel());
        state = ADACON_STATE_STOPPED;
    }
}

static bool trigger_handoff_to(int ch)
{
    int idx = get_ctrl_ch_idx(ch);
    if (idx < 0) {
        return false;
    }
    ho_ctrl_ch_idx = idx;
    ho_state = HANDOFF_STATE_ACTIVE;
    ho_interval = 1000 / cfg.sample_rate;
    ho_start = mloop_run_time();
    ml_timer_in(play_timer, ho_interval);
    return true;
}

static void action_single_handoff(int key)
{
    if (adacom_state() != ADACOM_STATE_CONNECTED)
        return;
    if (state == ADACON_STATE_STOPPED) {
        if (current_channel < 0) {
            current_channel = tui_select_channel(next_ho_channel());
        }
        if (trigger_handoff_to(current_channel)) {
            state = ADACON_STATE_PLAY_SINGLE;
        } else {
            log_error("Not able to start handoff sequence for channel %i!",
                    current_channel + 1);
        }
    } else if (state == ADACON_STATE_PLAY_COUNTINOUS) {
        ml_timer_cancle(play_timer);
        state = ADACON_STATE_STOPPED;
    }
}

static void set_all_channels_to(double value) {
    if (state != ADACON_STATE_STOPPED ||
            adacom_state() != ADACOM_STATE_CONNECTED)
        return;
    double values[n_channels];
    for (int ch = 0; ch < n_channels; ch++) {
        values[ch] = value;
    }
    adacom_set_all(values, n_channels, atten_set_all_cb);
}

static void action_all_min(int key) {
    set_all_channels_to(cfg.min_attenuation);
}

static void action_all_max(int key) {
    set_all_channels_to(cfg.max_attenuation);
}

static void init_control_channels(void) {
    for (int ch = 0; ch < n_channels; ch++) {
        if (cfg_is_in_channels(ch)) {
            ctrl_chs[n_ctrl_chs++] = ch;
        }
    }
}

static void sync_grouped_channels(double *values)
{
    Iter itr = init(Iter, cfg.groups);
    for (List *grp = next(&itr); grp != NULL; grp = next(&itr)) {
        Int *ch = list_get_at(grp, 0);
        if (ch->val >= n_channels)
            continue;
        group_set_channels(grp, values, values[ch->val]);
    }
    destroy(&itr);
}

static void connect_cb(AdaComError err)
{
    if (err == ADACOM_OK) {
        current_channel = -1;
        ho_ctrl_ch_idx = -1;
        n_channels = adacom_num_channels();
        init_control_channels();
        tui_adacom_infos(adacom_model(), adacom_sn(), n_channels);
        log_info("Connected to %s (%s) with %i channels.",
                adacom_model(), adacom_sn(), n_channels);
        tui_adacom_state(adacom_state());
        for (int ch = 0; ch < n_channels; ch++) {
            tui_set_attenuation(ch, adacom_get_channel(ch));
        }
        // Synchronise groups if there are any defined
        if (len(cfg.groups) > 0) {
            double values[n_channels];
            adacom_get_all(values, n_channels);
            sync_grouped_channels(values);
            adacom_set_all(values, n_channels, atten_set_all_cb);
        } else {
        }
    } else {
        tui_adacom_state(adacom_state());
    }
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

static void action_show_config(int key) {
    if (cfg.file_path != NULL) {
        log_info("file: %s", cfg.file_path);
    }
    log_info("groups: %O", cfg.groups);
    log_info("channels: %O", cfg.channels);
    log_info("min: %.2f, max: %.2f, pivot: %.2f",
            cfg.min_attenuation, cfg.max_attenuation, cfg.pivot_attenuation);
    log_info("sample rate: %i, action: %i, recovery: %i",
            cfg.sample_rate, cfg.action_time, cfg.recovery_time);
}

int main(int argc, char *argv[])
{
    cfg_init(argc, argv);
    log_init(cfg.log_level);
    mloop_init();
    tui_init();
    tui_add_action('x', action_disconnect);
    tui_add_action('c', action_connect);
    tui_add_action('m', action_all_max);
    tui_add_action('n', action_all_min);
    tui_add_action('s', action_ch_solo_step);
    tui_add_action('S', action_ch_solo);
    tui_add_action('h', action_single_handoff);
    tui_add_action(TUI_KEY_UP, action_up_down_atten);
    tui_add_action(TUI_KEY_DOWN, action_up_down_atten);
    tui_add_action(TUI_KEY_PPAGE, action_min_max_atten);
    tui_add_action(TUI_KEY_NPAGE, action_min_max_atten);
    tui_add_action(TUI_KEY_RIGHT, action_shift_ch_right);
    tui_add_action(TUI_KEY_LEFT, action_shift_ch_left);
    tui_add_action('C', action_show_config);
    tui_add_num_action(action_select_ch);
    adacom_init(cfg.ada.device);
    if (adacom_connect(connect_cb) != ADACOM_OK) {
        tui_adacom_state(adacom_state());
    }
    play_timer = new(MlTimer, player_cb, NULL);
    mloop_run();
    delete(play_timer);
    adacom_destroy();
    tui_destroy();
    cfg_destroy();
    return 0;
}


