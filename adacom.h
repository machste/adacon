#ifndef _ADACOM_H_
#define _ADACOM_H_

#include <stdbool.h>

#define ADACOM_MAX_CHANNELS 16
#define ADACOM_MIN_ATTENUATION 0
#define ADACOM_MAX_ATTENUATION 95
#define ADACOM_MIN_INTERVAL 0.25


typedef enum {
    ADACOM_STATE_INITIALISED,
    ADACOM_STATE_CONNECTING,
    ADACOM_STATE_CONNECTED,
    ADACOM_STATE_DISCONNECTED,
    ADACOM_STATE_ERROR,
    ADACOM_STATE_UNKNOWN
} AdaComState;

typedef enum {
    ADACOM_OK,
    ADACOM_ERR_NOT_CONNECTED,
    ADACOM_ERR_DEVICE_NOT_FOUND,
    ADACOM_ERR_DEVICE_NOT_SUPPORTED,
    ADACOM_ERR_DEVICE_NOT_AVAILABLE,
    ADACOM_ERR_DEVICE_BUSY,
    ADACOM_ERR_INVALID_CHANNEL,
    ADACOM_ERR_INVALID_ATTENUATION,
    ADACOM_ERR_NUM_CHANNELS,
    ADACOM_ERR_CMD_TIMEOUTED,
    ADACOM_ERR_UNKONWN
} AdaComError;

typedef void (*adacom_connect_cb)(AdaComError err);
typedef void (*adacom_channel_cb)(AdaComError err, int ch, double value);
typedef void (*adacom_channels_cb)(AdaComError err, double *values, int n);


void adacom_init(const char *com_device);
void adacom_destroy(void);

AdaComState adacom_state(void);
const char *adacom_state_to_cstr(AdaComState state);

const char *adacom_model(void);
const char *adacom_sn(void);
int adacom_num_channels(void);

AdaComError adacom_connect(adacom_connect_cb state_cb);
void adacom_disconnect(void);

double adacom_get_channel(int ch);
AdaComError adacom_set_channel(int ch, double value, adacom_channel_cb ch_cb);
AdaComError adacom_get_all(double *values, int n);
AdaComError adacom_set_all(double *values, int n, adacom_channels_cb chs_cb);

#endif /* _ADACOM_H_ */
