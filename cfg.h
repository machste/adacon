#ifndef _CFG_H_
#define _CFG_H_

#include <masc.h>

#define CFG_SAMPLE_RATE_MIN 1
#define CFG_SAMPLE_RATE_MAX 100
#define CFG_ACTION_TIME_MIN 0
#define CFG_RECOVERY_TIME_MIN 500


typedef struct {
    const char *device;
} AdauraConfig;

typedef struct {
    int log_level;
    char *file_path;
    AdauraConfig ada;
    double pivot_attenuation;
    int sample_rate;
    int action_time;
    int recovery_time;
} Config;


extern Config cfg;


void cfg_init(int argc, char *argv[]);
void cfg_destroy(void);

#endif /* _CFG_H_ */
