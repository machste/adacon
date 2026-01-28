#define _NCURSES

#ifdef _NCURSES
#define filter ncurses_filter
    #include <ncurses.h>
#undef filter
#endif

#include <signal.h>
#include <masc.h>

#include "tui.h"


typedef struct TuiAction {
    Object;
    int key;
    tui_action_cb action_cb;
} TuiAction;


static const class *TuiActionCls;

// Input via stdin and signal handling of SIGWINCH
static Io input;
struct sigaction winch_act, ncurses_winch_act;
static volatile sig_atomic_t resize_flag = 0;
// Values to display
static Str title;
static AdaComState ada_state = ADACOM_STATE_CONNECTING;
static const char *ada_model = NULL;
static const char *ada_sn = NULL;
static int ada_num_channels = 0;
static double ada_attenuations[ADACOM_MAX_CHANNELS];
static int selected_channel = -1;
// Graphics
static int y_max, x_max;
static int y_ada_state = 2;
static int x_ada_name = 2;
static int x_ada_value = 12;
static int y_ada_infos = 3;
static WINDOW *wtab = NULL;
static int y_tab = 7;
static int x_tab = 2;
static int y_tab_head = 0;
static int y_tab_val = 1;
static int x_tab_val = 18;
static int tab_height = 4;
static int tab_col_width = 8;
static WINDOW *wlog = NULL;
static int y_wlog = 14;
// Actions
static List *actions = NULL;
static tui_action_cb num_action_cb = NULL;


static void sigwinch_handler(int sig)
{
    //TODO: This should be replaced by MlEvent (if it once exists) 
    resize_flag = 1;
    // Call the original signal handler of ncurses manually
    if (ncurses_winch_act.sa_handler != SIG_DFL
            && ncurses_winch_act.sa_handler != SIG_IGN) {
        ncurses_winch_act.sa_handler(sig);
    }
}

static void update_ada_state(void)
{
    mvaddstr(y_ada_state, x_ada_value, adacom_state_to_cstr(ada_state));
    clrtoeol();
}

static void draw_ada_state(void)
{
    mvaddstr(y_ada_state, x_ada_name, "State:");
    update_ada_state();
}

static void update_ada_infos(void)
{
    int y = y_ada_infos;
    mvaddstr(y++, x_ada_value, ada_model == NULL ? "---" : ada_model);
    clrtoeol();
    mvaddstr(y++, x_ada_value, ada_sn == NULL ? "---" : ada_sn);
    clrtoeol();
    mvprintw(y++, x_ada_value, "%i", ada_num_channels);
    clrtoeol();
}

static void draw_ada_infos(void)
{
    int y = y_ada_infos;
    mvaddstr(y++, x_ada_name, "Model:");
    mvaddstr(y++, x_ada_name, "S/N:");
    mvaddstr(y++, x_ada_name, "Channels:");
    update_ada_infos();
}

static void print_channel_header(int ch, bool selected)
{
    int x_col = x_tab_val + ch * tab_col_width;
    if (selected) wattron(wtab, A_REVERSE);
    mvwprintw(wtab, y_tab_head, x_col, "   CH%02i ", ch + 1);
    if (selected) wattroff(wtab, A_REVERSE);
}

static void update_selected_channel(int ch)
{
    // Unselect previous channel
    if (selected_channel >= 0) {
        print_channel_header(selected_channel, false);
    }
    // Select new channel
    if (ch >= 0) {
        print_channel_header(ch, true);
    }
    selected_channel = ch;
}

static void update_attenuation(int ch)
{
    int x_col = x_tab_val + ch * tab_col_width;
    double value = ada_attenuations[ch];
    if (value >= 0) {
        mvwprintw(wtab, y_tab_val, x_col, "  %5.2f ", value);
    } else {
        mvwaddstr(wtab, y_tab_val, x_col, "   --   ");
    }
}

static void draw_channel_table(void)
{
    if (wtab == NULL) {
        wtab = newwin(tab_height, x_max - x_tab, y_tab, x_tab);
    } else {
        wresize(wtab, tab_height, x_max - x_tab);
    }
    wclear(wtab);
    if (ada_num_channels > 0) {
        mvwaddstr(wtab, y_tab_val, 0, "Attenuation [dB]:");
        for (int ch = 0; ch < ada_num_channels; ch++) {
            print_channel_header(ch, ch == selected_channel);
            update_attenuation(ch);
        }
    }
    wrefresh(wtab);
}

static void draw_log(void)
{
    if (wlog == NULL) {
        wlog = newwin(y_max - y_wlog, x_max, y_wlog, 0);
        scrollok(wlog, TRUE);
    } else {
        wresize(wlog, y_max - y_wlog, x_max);
    }
    wrefresh(wlog);
}

static void draw(void)
{
    getmaxyx(stdscr, y_max, x_max);
    // Draw title bar
    attron(A_REVERSE);
    mvprintw(0, 0, title.cstr);
    hline(' ', x_max);
    attroff(A_REVERSE);
    // Draw adacom state and infos
    draw_ada_state();
    draw_ada_infos();
    mvhline(y_wlog - 1, 0, ACS_HLINE, x_max);
    refresh();
    // Draw channel table
    draw_channel_table();
    // Draw log window
    draw_log();
}

static void redraw()
{
    clear();
    draw();
}

static TuiAction *get_action_by_key(int key) {
    TuiAction *a = NULL;
    Iter *itr = new(Iter, actions);
    for (TuiAction *action = next(itr); action != NULL; action = next(itr)) {
        if (action->key == key) {
            a = action;
            break;
        }
    }
    delete(itr);
    return a;
}

static bool call_action(int key) {
    TuiAction *a = get_action_by_key(key);
    if (a != NULL && a->action_cb != NULL) {
        a->action_cb(key);
        return true;
    }
    return false;
}

static void keyboard_input_cb(MlIo *self, int fd, ml_io_flag_t events, void *arg)
{
    if (events & ML_IO_READ) {
        while (true) {
            int key = getch();
            if (key == ERR)
                break;
            if (key == KEY_RESIZE) {
                redraw();
            } else if (key == 'q' || key == 'Q' || key == TUI_KEY_ESC) {
                mloop_stop();
            }
            if (call_action(key))
                break;
            if (num_action_cb != NULL && key >= '0' && key <= '9') {
                num_action_cb(key);
            }
        }
    }
}

static void log_message_cb(int level, Str *msg, void *arg)
{
    if (wlog == NULL)
        return;
    str_append(msg, "\n");
    wprintw(wlog, msg->cstr);
    wrefresh(wlog);
}

void tui_init(void)
{
    initscr();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    set_escdelay(50);
    // Add handling of SIGWINCH after setting up ncurses
    winch_act.sa_handler = sigwinch_handler;
    sigaction(SIGWINCH, &winch_act, &ncurses_winch_act);
    // Register stdin to main loop
    input = init(Io, STDIN_FILENO);
    mloop_io_new(&input, ML_IO_READ, keyboard_input_cb, NULL);
    // Setup log facility for log window
    log_add_custom(log_message_cb, NULL);
    // Initialise Adaura values
    for (int ch = 0; ch < ADACOM_MAX_CHANNELS; ch++) {
        ada_attenuations[ch] = -1;
    }
    // Initialise TUI values
    getmaxyx(stdscr, y_max, x_max);
    title = init(Str, "%s v%s", PROJECT_TITLE, PROJECT_VERSION);
    actions = new(List);
    // Draw TUI
    draw();
}

void tui_destroy(void)
{
    destroy(&title);
    destroy(&input);
    endwin();
}

void tui_add_action(int key, tui_action_cb cb)
{
    TuiAction *a = get_action_by_key(key);
    if (a == NULL) {
        list_append(actions, new(TuiAction, key, cb));
    } else {
        log_error("tui: Action for '%c' already is defined!", key);
    }
}

void tui_add_num_action(tui_action_cb cb)
{
    num_action_cb = cb;
}

void tui_adacom_state(AdaComState state)
{
    ada_state = state;
    update_ada_state();
    refresh();
}

void tui_adacom_infos(const char *model, const char *sn, int num_channels)
{
    ada_model = model;
    ada_sn = sn;
    ada_num_channels = num_channels;
    draw_channel_table();
    update_ada_infos();
    refresh();
}

int tui_select_channel(int channel)
{
    if (channel < 0 || channel >= ada_num_channels) {
        // Unselect current channel
        channel = -1;
    }
    update_selected_channel(channel);
    wrefresh(wtab);
    return selected_channel;
}

void tui_set_attenuation(int channel, double value)
{
    if (channel < 0 || channel >= ada_num_channels)
        return;
    ada_attenuations[channel] = value;
    update_attenuation(channel);
    wrefresh(wtab);
}

void tui_set_attenuations(double *values, int n)
{
    if (n != ada_num_channels)
        return;
    for (int ch = 0; ch < n; ch++) {
        ada_attenuations[ch] = values[ch];
        update_attenuation(ch);
    }
    wrefresh(wtab);
}

static void _vinit(TuiAction *self, va_list va)
{
    object_init(self, TuiActionCls);
    self->key = (int)va_arg(va, int);
    self->action_cb = (tui_action_cb)va_arg(va, tui_action_cb);
}

static void _destroy(TuiAction *self)
{
}

static void _init_class(class *cls)
{
    cls->super = ObjectCls;
}


static class _TuiActionCls = {
    .name = "TuiAction",
    .size = sizeof(TuiAction),
    .super = NULL,
    .init_class = _init_class,
    .vinit = (vinit_cb)_vinit,
    .init_copy = (init_copy_cb)object_init_copy,
    .destroy = (destroy_cb)_destroy,
    .cmp = (cmp_cb)object_cmp,
    .repr = (repr_cb)object_to_cstr,
    .to_cstr = (to_cstr_cb)object_to_cstr,
};

static const class *TuiActionCls = &_TuiActionCls;
