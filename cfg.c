#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "adacom.h"


/* Default Configuration */
Config cfg = {
    .log_level = LOG_INFO,
    .file_path = NULL,
    .ada.device = "/dev/ttyUSB_ADAURA",
    .pivot_attenuation = 47.5,
    .sample_rate = 10,
    .action_time = 1000,
    .recovery_time = 5000
};

static const char *cfg_file_paths[] = { "~/.adacon.json", "/etc/adacon.json" };
static const char *prog_name = NULL;
static Map *cmdline_args = NULL;


static void *log_level_check(Str *log_level_str, Str **err_msg)
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

static void *config_file_check(Str *path, Str **err_msg)
{
    Json *js = NULL;
    File *js_file = argparse_file(path, err_msg);
    if (!is_none(js_file)) {
        Str *js_str = readstr(js_file, -1);
        js = json_new_cstr(str_cstr(js_str));
        delete(js_str);
        if (json_is_valid(js)) {
            // Save config file path
            cfg.file_path = strdup(js_file->path);
        } else {
            *err_msg = str_new("config file '%O' is invalid!", path);
            delete(js);
            js = NULL;
        }
        delete(js_file);
    }
    return js;
}

static void *device_check(Str *path, Str **err_msg)
{
    if (!path_exists(str_cstr(path)))
    {
        *err_msg = str_new("device '%O' does not exist!", path);
        return NULL;
    }
    return new_copy(path);
}

static Map *parse_cmdline_args(int argc, char *argv[])
{
    Map *args;
    prog_name = path_basename(argv[0]);
    // Setup argument parser
    Argparse *ap = new(Argparse, prog_name, PROJECT_TITLE);
    // * Log level
    argparse_add_opt(ap, 'l', "log-level", "LEVEL", "1", log_level_check,
                     "log level (0 - 7)");
    // * Config file
    argparse_add_opt(ap, 'c', "cfg-file", "FILE", "1", config_file_check,
                     "configuration file");
    // * Device name
    argparse_add_opt(ap, 'd', "device", "DEV", "1", device_check,
                     "path to serial device");
    // Parse command line arguments
    args = argparse_parse(ap, argc, argv);
    delete(ap);
    return args;
}

static void merge_cmdline_args(Map *args)
{
    // Log level
    Int *log_level = map_get(args, "log-level");
    if (!is_none(log_level)) {
        cfg.log_level = int_get(log_level);
    }
    // Adaura device
    Str *device = map_get(args, "device");
    if (!is_none(device)) {
        cfg.ada.device = str_cstr(device);
    }
}

static Str *path_expanduser(const char *path)
{
    if (!cstr_startswith(path, "~/")) {
        goto out;
    }
    char *home = getenv("HOME");
    if (home == NULL) {
        goto out;
    }
    return path_join(home, path + 2);
out:
    // Expanding home directory failed, return the original path.
    return str_new_cstr(path);
}

static Json *check_default_config_files(void)
{
    Json *js = NULL;
    Str *err_msg = NULL;
    for (int i = 0; i < ARRAY_LEN(cfg_file_paths); i++) {
        const char *path_cstr = cfg_file_paths[i];
        Str *path = path_expanduser(path_cstr);
        if (path_is_file(path->cstr)) {
            js = config_file_check(path, &err_msg);
        }
        delete(path);
        if (js != NULL) {
            return js;
        } else if (err_msg != NULL) {
            fprint(stderr, "%s: error: %O\n", prog_name, err_msg);
            delete(err_msg);
            exit(1);
        }
    }
    return js;
}

static void parse_config_file(Json *js)
{
    Str *err_msg = NULL;
    // Logging settings
    Object *log_level_obj = json_get_node(js, "log_level");
    if (!is_none(log_level_obj)) {
        Str *log_level_str = to_str(log_level_obj);
        Int *log_level = log_level_check(log_level_str, &err_msg);
        if (!is_none(log_level)) {
            cfg.log_level = int_get(log_level);
            delete(log_level);
        } else {
            goto out;
        }
    }
    // Device path
    Object *device_obj = json_get_node(js, "device");
    if (isinstance(device_obj, Str)) {
        // Only use device_check for checking, ...
        Str *device = device_check((Str *)device_obj, &err_msg);
        if (!is_none(device)) {
            // ... but use the content of the js_cfg as c-string.
            cfg.ada.device = str_cstr((Str *)device_obj);
            delete(device);
        } else {
            goto out;
        }
    } else if (!is_none(device_obj)) {
        err_msg = str_new("invalid type <%s> for device! (%O)",
                name_of(device_obj), device_obj);
        goto out;
    }
    // Pivot attenuation
    Object *pivot_atten_obj = json_get_node(js, "pivot_attenuation");
    if (!is_none(pivot_atten_obj)) {
        if (!isinstance(pivot_atten_obj, Num)) {
            err_msg = str_new("Expecting type Num for 'pivot_attenuation'!");
            goto out;
        }
        double atten = to_double((Num *)pivot_atten_obj);
        // Check the pivot attenuation value
        if (atten < ADACOM_MIN_ATTENUATION || atten > ADACOM_MAX_ATTENUATION) {
            err_msg = str_new("Value of 'pivot_attenuation' is out of range!");
            goto out;
        }
        cfg.pivot_attenuation = atten;
    }
    // Sample rate
    Object *sample_rate_obj = json_get_node(js, "sample_rate");
    if (!is_none(sample_rate_obj)) {
        if (!isinstance(sample_rate_obj, Int)) {
            err_msg = str_new("Expecting type Int for 'sample_rate'!");
            goto out;
        }
        long rate = int_get((Int *)sample_rate_obj);
        // Check the sample rate
        if (rate < CFG_SAMPLE_RATE_MIN || rate > CFG_SAMPLE_RATE_MAX) {
            err_msg = str_new("Value of 'sample_rate' is out of range!");
            goto out;
        }
        cfg.sample_rate = rate;
    }
    // Action time
    Object *action_time_obj = json_get_node(js, "action_time");
    if (!is_none(action_time_obj)) {
        if (!isinstance(action_time_obj, Int)) {
            err_msg = str_new("Expecting type Int for 'action_time'!");
            goto out;
        }
        long time = int_get((Int *)action_time_obj);
        // Check the action time
        if (time < CFG_ACTION_TIME_MIN) {
            err_msg = str_new("Value of 'action_time' is too small!");
            goto out;
        }
        cfg.action_time = time;
    }
    // Recovery time
    Object *recovery_time_obj = json_get_node(js, "recovery_time");
    if (!is_none(recovery_time_obj)) {
        if (!isinstance(recovery_time_obj, Int)) {
            err_msg = str_new("Expecting type Int for 'recovery_time'!");
            goto out;
        }
        long time = int_get((Int *)recovery_time_obj);
        // Check the recovery time
        if (time < CFG_RECOVERY_TIME_MIN) {
            err_msg = str_new("Value of 'recovery_time' is too small!");
            goto out;
        }
        cfg.recovery_time = time;
    }
    return;
out:
    fprint(stderr, "%s: error: %O\n", prog_name, err_msg);
    delete(err_msg);
    exit(1);
}

void cfg_init(int argc, char *argv[])
{
    // First parse command line arguments to get config file path
    cmdline_args = parse_cmdline_args(argc, argv);
    Json *cfg_js = map_get(cmdline_args, "cfg-file");
    // If no config file is defined ...
    if (is_none(cfg_js)) {
        // ... check the default config file locations.
        cfg_js = check_default_config_files();
    }
    // If a config file was found ...
    if (!is_none(cfg_js)) {
        // ... parse the config file.
        parse_config_file(cfg_js);
    }
    // Finally, merge the configuration with command line arguments.
    merge_cmdline_args(cmdline_args);
}

void cfg_destroy(void)
{
    free(cfg.file_path);
    delete(cmdline_args);
}
