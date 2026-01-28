#include <string.h>
#include <masc.h>

#include "adacom.h"


typedef enum {
    CONN_STEP_GET_INFOS,
    CONN_STEP_GET_STATUS,
    CONN_STEP_UNKNOWN
} ConnectionStep;

typedef enum {
    COMMAND_NONE,
    COMMAND_SET,
    COMMAND_SET_ALL,
    COMMAND_SAA,
    COMMAND_RESET,
    COMMAND_UNKNOWN
} CommandId;


static const char *state_to_cstr[] = {
    [ADACOM_STATE_INITIALISED] = "INITIALISED",
    [ADACOM_STATE_CONNECTING] = "CONNECTING",
    [ADACOM_STATE_CONNECTED] = "CONNECTED",
    [ADACOM_STATE_DISCONNECTED] = "DISCONNECTED",
    [ADACOM_STATE_ERROR] = "ERROR",
    [ADACOM_STATE_UNKNOWN] = "UNKNOWN"
};


// Adaura communiction settings
static AdaComState state = ADACOM_STATE_UNKNOWN;
static const char *device = NULL;
static Serial *serial = NULL;
static MlTimer *com_wdog = NULL;
static int timeout = 1000;
// Adaura Infos
static Str *model = NULL;
static Str *sn = NULL;
static List *def_attenuations = NULL;
static int num_channels = 0;
// General values
static double attenuations[ADACOM_MAX_CHANNELS];
static int cur_channel;
static void *cmd_cb = NULL;
// State CONNECTING
static ConnectionStep conn_step = CONN_STEP_UNKNOWN;
static Regex *regex_channel = NULL;
// State CONNECTED
static CommandId cmd_id = COMMAND_UNKNOWN;
static double req_attenuations[ADACOM_MAX_CHANNELS];
static Regex *regex_set_resp = NULL;

// Forward declarations
static void call_cmd_cb(AdaComError err);
static void com_wdog_cb(MlTimer *timer, void *arg);


void adacom_init(const char *com_device)
{
    device = com_device;
    regex_channel = new(Regex, "Channel\\s+([0-9]+):\\s+(.+)");
    regex_set_resp = new(Regex, "Channel\\s+([0-9]+).+set to\\s+(.+)");
    state = ADACOM_STATE_INITIALISED;
}

static void reset_adainfos(void)
{
    delete(model);
    delete(sn);
    delete(def_attenuations);
    num_channels = 0;
}

void adacom_destroy(void)
{
    adacom_disconnect();
    reset_adainfos();
    delete(regex_channel);
    delete(regex_set_resp);
}

AdaComState adacom_state(void)
{
    return state;
}

const char *adacom_state_to_cstr(AdaComState state)
{
    if (state < 0 || state > ADACOM_STATE_UNKNOWN) {
        return "INVALID_STATE";
    }
    return state_to_cstr[state];
}

const char *adacom_model(void)
{
    return model != NULL ? model->cstr : NULL;
}

const char *adacom_sn(void)
{
    return sn != NULL ? sn->cstr : NULL;
}

int adacom_num_channels(void)
{
    return num_channels;
}

static void change_state(AdaComState new_state)
{
    if (state == new_state)
        return;
    log_debug("adacom: %s ---> %s", state_to_cstr[state],
            state_to_cstr[new_state]);
    state = new_state;
}

static void start_com_wdog(int ms)
{
    if (com_wdog != NULL) {
        mloop_timer_in(com_wdog, ms);
    } else {
        com_wdog = mloop_timer_new(ms, com_wdog_cb, NULL);
    }
}

static void stop_com_wdog(void)
{
    mloop_timer_cancle(com_wdog);
}

static bool is_cmd_running(void)
{
    return com_wdog != NULL ? com_wdog->pending : false;
}

static AdaComError send_cmd(const char *cmd)
{
    if (serial == NULL)
        return ADACOM_ERR_DEVICE_NOT_AVAILABLE;
    if (is_cmd_running())
        return ADACOM_ERR_DEVICE_BUSY;
    log_debug("adacom: [->] %s", cmd);
    // Send command
    write(serial, cmd, strlen(cmd));
    // Start communication watchdog timer
    start_com_wdog(timeout);
    return ADACOM_OK;
}

static void call_cmd_cb(AdaComError err)
{
    if (cmd_cb != NULL) {
        if (cmd_id == COMMAND_NONE) {
            adacom_connect_cb cb = (adacom_connect_cb)cmd_cb;
            cb(err);
        } else if (cmd_id == COMMAND_SET) {
            adacom_channel_cb cb = (adacom_channel_cb)cmd_cb;
            cb(err, cur_channel, req_attenuations[cur_channel]);
        } else if (cmd_id == COMMAND_SET_ALL) {
            adacom_channels_cb cb = (adacom_channels_cb)cmd_cb;
            cb(err, req_attenuations, num_channels);
        }
    }
}

static void com_wdog_cb(MlTimer *timer, void *arg)
{
    change_state(ADACOM_STATE_ERROR);
    call_cmd_cb(ADACOM_ERR_CMD_TIMEOUTED);
}

static void complete_cmd(AdaComError err) {
    // Stop the communication watchdog timer
    stop_com_wdog();
    // Call callback of the specific command
    call_cmd_cb(err);
    // The user is allowed to send a new command in the callback function.
    // However, if no command is running, ...
    if (!is_cmd_running()) {
        // ... cleanup command variables.
        cmd_id = COMMAND_NONE;
        cmd_cb = NULL;
    }
}

static void process_get_infos(Str *line)
{
    List *info = str_split(line, ": ", 2);
    if (list_len(info) == 2) {
        Str *name = list_get_at(info, 0);
        if (str_eq_cstr(name, "Model")) {
            model = list_remove_at(info, 1);
        } else if (str_eq_cstr(name, "SN")) {
            sn = list_remove_at(info, 1);
        } else if (str_eq_cstr(name, "Default Attenuations")) {
            Str *value = list_get_at(info, 1);
            def_attenuations = str_split(value, " ", -1);
            num_channels = list_len(def_attenuations);
            if (num_channels > ADACOM_MAX_CHANNELS) {
                log_error("adacom: Too many channels!");
                change_state(ADACOM_STATE_ERROR);
                complete_cmd(ADACOM_ERR_DEVICE_NOT_SUPPORTED);
            }
        } else if (str_eq_cstr(name, "DHCP")) {
            if (model != NULL && sn != NULL && num_channels > 0) {
                // Basic infos have been read
                stop_com_wdog();
                log_debug("adacom: Response from %O (%O) with %i channels.",
                        model, sn, num_channels);
                // Now get current attenuations
                send_cmd("status");
                cur_channel = 1;
                conn_step = CONN_STEP_GET_STATUS;
            } else {
                log_error("adacom: Missing information!");
                change_state(ADACOM_STATE_ERROR);
                complete_cmd(ADACOM_ERR_DEVICE_NOT_SUPPORTED);
            }
        }
    }
    delete(info);
}

static void process_get_status(Str *line)
{
    Array *match = regex_search(regex_channel, line->cstr);
    if (match == NULL)
        return;
    Int *channel = str_to_int(array_get_at(match, 1), true);
    Double *value = str_to_double(array_get_at(match, 2), true);
    if (channel == NULL || value == NULL) {
        log_warn("adacom: Unable to parse channel value!");
    } else if (channel->val >= 1 && channel->val <= num_channels) {
        log_debug("adacom: Got %.2fdB attenuation for channel %i",
                value->val, channel->val);
        if (channel->val == cur_channel) {
            attenuations[channel->val - 1] = value->val;
            cur_channel++;
        } else {
            log_warn("adacom: Unexpected channel number!");
        }
        // Check for completeness
        if (cur_channel > num_channels) {
            change_state(ADACOM_STATE_CONNECTED);
            complete_cmd(ADACOM_OK);
        }
    }
    delete(value);
    delete(channel);
    delete(match);
}

static void process_connecting(Str *line)
{
    if (conn_step == CONN_STEP_GET_INFOS) {
        process_get_infos(line);
    } else if (conn_step == CONN_STEP_GET_STATUS) {
        process_get_status(line);
    } else {
        log_error("adacom: Error in connection state machine!");
        change_state(ADACOM_STATE_ERROR);
        complete_cmd(ADACOM_ERR_UNKONWN);
    }
}

static int skip_good_values(int channel)
{
    int ch;
    for (ch = channel; ch < num_channels; ch++) {
        if (req_attenuations[ch] != attenuations[ch])
            return ch;
    }
    return ch;
}


static void process_cmd_set(Str *line)
{
    // Check response
    Array *match = regex_search(regex_set_resp, line->cstr);
    if (match != NULL) {
        Int *channel = str_to_int(array_get_at(match, 1), true);
        Double *value = str_to_double(array_get_at(match, 2), true);
        if (channel == NULL || value == NULL) {
            log_warn("adacom: Unable to parse channel value!");
        } else if (channel->val - 1 != cur_channel) {
            log_warn("adacom: Unexpected channel number!");
        } else {
            // Setting attenuation has been successful, stop watchdog timer
            stop_com_wdog();
            // Update mirror variable
            attenuations[channel->val - 1] = value->val;
            if (cmd_id == COMMAND_SET_ALL) {
                // Generate and send next command for set all command
                cur_channel = skip_good_values(++cur_channel);
                if (cur_channel < num_channels) {
                    Str cmd = init(Str, "set %i %.2f",
                            cur_channel + 1, req_attenuations[cur_channel]);
                    // Send command
                    send_cmd(cmd.cstr);
                    destroy(&cmd);
                } else {
                    complete_cmd(ADACOM_OK);
                }
            } else {
                complete_cmd(ADACOM_OK);
            }
        }
        delete(value);
        delete(channel);
        delete(match);
    } else if (str_startswith(line, "Invalid command")) {
        complete_cmd(false);
    }
}

static void process_command(Str *line)
{
    if (cmd_id == COMMAND_SET || cmd_id == COMMAND_SET_ALL) {
        process_cmd_set(line);
    } else if (is_cmd_running()) {
        log_warn("adacom: Got unexpectet reponse from device.");
    } else {
        log_warn("adacom: Command is not implemented!");
    }
}

static void serial_line_cb(MlIoPkg *self, void *data, size_t size, void *arg)
{
    Str line;
    str_init_ncopy(&line, data, size);
    str_strip(&line);
    if (str_len(&line) > 0 
            && !str_startswith(&line, "--")
            && !str_startswith(&line, "#")) {
        log_debug("adacom: [<-] %s", line.cstr);
        if (state == ADACOM_STATE_CONNECTING) {
            process_connecting(&line);
        } else if (state == ADACOM_STATE_CONNECTED) {
            process_command(&line);
        }
    }
    destroy(&line);
}

static void serial_eof_cb(MlIoReader *self, void *arg)
{
    log_error("adacom: Received EOF from serial device!");
    adacom_disconnect();
}

AdaComError adacom_connect(adacom_connect_cb state_cb)
{
    serial = new(Serial, device, SERIAL_SPEED_B115200, SERIAL_PARITY_NONE);
    if (!is_open(serial)) {
        log_error("adacom: Unable to connect to serial '%s'!", device);
        change_state(ADACOM_STATE_ERROR);
        serial_delete(serial);
        serial = NULL;
        return ADACOM_ERR_DEVICE_NOT_FOUND;
    }
    change_state(ADACOM_STATE_CONNECTING);
    reset_adainfos();
    cmd_cb = state_cb;
    cmd_id = COMMAND_NONE;
    mloop_io_pkg_new(serial, '\n', serial_line_cb, serial_eof_cb, NULL);
    conn_step = CONN_STEP_GET_INFOS;
    send_cmd("info");
    return ADACOM_OK;
}

void adacom_disconnect(void)
{
    if (serial == NULL || state != ADACOM_STATE_CONNECTED)
        return;
    serial_close(serial);
    serial_delete(serial);
    serial = NULL;
    change_state(ADACOM_STATE_DISCONNECTED);
}

double adacom_get_channel(int ch)
{
    return (ch < 0 || ch > num_channels) ? -1 : attenuations[ch];
}

static double validate_attenuation(double value)
{
    // Check limits
    if (value > ADACOM_MAX_ATTENUATION) {
        value = ADACOM_MAX_ATTENUATION;
    } else if (value < ADACOM_MIN_ATTENUATION) {
        value = ADACOM_MIN_ATTENUATION;
    }
    // Check the minimal steps (only quater steps are allowed)
    int a_int = value;
    double a_remain = value - a_int;
    int ivals = a_remain / ADACOM_MIN_INTERVAL;
    return a_int + ivals * ADACOM_MIN_INTERVAL;
}

AdaComError adacom_set_channel(int ch, double value, adacom_channel_cb ch_cb)
{
    if (state != ADACOM_STATE_CONNECTED)
        return ADACOM_ERR_NOT_CONNECTED;
    if (ch < 0 || ch > num_channels)
        return ADACOM_ERR_INVALID_CHANNEL;
    // Save channel number and requested value
    cur_channel = ch;
    req_attenuations[cur_channel] = validate_attenuation(value);
    // Generate command
    Str cmd = init(Str, "set %i %.2f",
            cur_channel + 1, req_attenuations[cur_channel]);
    // Send command
    cmd_id = COMMAND_SET;
    cmd_cb = ch_cb;
    AdaComError err = send_cmd(cmd.cstr);
    destroy(&cmd);
    return err;
}

AdaComError adacom_get_all(double *values, int n)
{
    if (state != ADACOM_STATE_CONNECTED)
        return ADACOM_ERR_NOT_CONNECTED;
    if (n != num_channels)
        return ADACOM_ERR_NUM_CHANNELS;
    for (int ch = 0; ch < n; ch++) {
        values[ch] = attenuations[ch];
    }
    return ADACOM_OK;
}

AdaComError adacom_set_all(double *values, int n, adacom_channels_cb chs_cb)
{
    if (state != ADACOM_STATE_CONNECTED)
        return ADACOM_ERR_NOT_CONNECTED;
    if (n != num_channels)
        return ADACOM_ERR_NUM_CHANNELS;
    // Start with the first channel and save requested values
    for (int ch = 0; ch < n; ch++) {
        req_attenuations[ch] = validate_attenuation(values[ch]);
    }
    cur_channel = skip_good_values(0);
    if (cur_channel >= num_channels) {
        log_debug("adacom: Channels are already set to requested values.");
        return ADACOM_OK;
    }
    // Generate command for the first channel
    Str cmd = init(Str, "set %i %.2f",
            cur_channel + 1, req_attenuations[cur_channel]);
    // Send command
    cmd_id = COMMAND_SET_ALL;
    cmd_cb = chs_cb;
    send_cmd(cmd.cstr);
    destroy(&cmd);
    return ADACOM_OK;
}
