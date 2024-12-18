#include <stdio.h>
#include <signal.h>
#include <time.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <unistd.h>
#include <sys/wait.h>
#include <wayland-client.h>
#include <wayland-server.h>
#include <wayland-util.h>
#include <libnotify/notify.h>
#include "ext-idle-notify-v1-client-protocol.h"
#include "log.h"

struct seat {
    struct wl_list link;
    struct wl_seat *proxy;

    char *name;
    uint32_t capabilities;
};

static const char *verbosity_colors[] = {
    [LOG_SILENT] = "",
    [LOG_ERROR ] = "\x1B[1;31m",
    [LOG_INFO  ] = "\x1B[1;34m",
    [LOG_DEBUG ] = "\x1B[1;30m",
};
static enum log_level curr_log_level = LOG_INFO;

struct wl_display *display = NULL;
struct wl_event_loop *event_loop = NULL;
static struct ext_idle_notifier_v1 *idle_notifier = NULL;
static struct ext_idle_notification_v1 *idle_notification = NULL;
static struct wl_list seats;
static struct wl_seat *seat = NULL;
static char *seat_name = NULL;
static NotifyNotification *message = NULL;
static unsigned int idle_timeout = 30 * 1000;
static unsigned int alarm_seconds = 30 * 60;
static const char *message_summary = NULL;
static const char *message_body = NULL;
static const char *message_icon = NULL;
static int message_urgency = NOTIFY_URGENCY_NORMAL;
static char *command = NULL;

static unsigned int time_left = 0;
static time_t idle_timestamp = 0;
static bool inhibited = false;

static void init_libnotify(const char *app_name) {
    notify_init(app_name);
}

static NotifyNotification * get_notify_message(const char *summary, const char *body, const char *icon, const NotifyUrgency urgency) {
    NotifyNotification *msg = notify_notification_new(
            summary == NULL ? "Protect Your Eyes"                    : summary,
            body    == NULL ? "Timeout reached. Have a rest please." : body,
            icon    == NULL ? "dialog-information"                   : icon
            );
    notify_notification_set_urgency(msg, urgency);
    return msg;
}

static void show_notify_message(NotifyNotification *msg) {
    pme_log(LOG_INFO, "Showing notification message.");
    notify_notification_show(msg, NULL);
}

static void destroy_libnotify(const NotifyNotification *msg) {
    g_object_unref(G_OBJECT(msg));
    notify_uninit();
}

static void run_command(char *cmd) {
    pme_log(LOG_INFO, "Executing command: %s", cmd);
    pid_t pid = fork();
    if (pid == 0) {
        char *const wrapped_cmd[] = {"sh", "-c", cmd, NULL};
        pid = fork();
        if (pid == 0) {
            sigset_t set;
            sigemptyset(&set);
            sigprocmask(SIG_SETMASK, &set, NULL);
            signal(SIGINT, SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            signal(SIGUSR1, SIG_DFL);
            signal(SIGUSR2, SIG_DFL);
            signal(SIGALRM, SIG_DFL);

            execvp(wrapped_cmd[0], wrapped_cmd);
            pme_log_errno(LOG_ERROR, "execvp failed.");
            exit(1);
        } else if (pid > 0) {
            pme_log(LOG_DEBUG, "Spawned grandchild process to execute: %s %s %s", wrapped_cmd[0], wrapped_cmd[1], wrapped_cmd[2]);
            exit(0);
        } else {
            pme_log_errno(LOG_ERROR, "fork grandchild failed.");
            exit(1);
        }
    } else if (pid > 0) {
        pme_log(LOG_DEBUG, "Spawned child process to execute: %s", cmd);
        int status;
        if (waitpid(pid, &status, 0) == -1)
            pme_log_errno(LOG_ERROR, "waitpid failed.");
        if (WIFEXITED(status))
            pme_log(LOG_DEBUG, "Child process exited with status %d.", WEXITSTATUS(status));
    } else {
        pme_log_errno(LOG_ERROR, "fork child failed.");
    }
}

void register_alarm(unsigned int seconds) {
    alarm(seconds);
    if (seconds == 0)
        pme_log(LOG_DEBUG, "Alarm cancelled.");
    else
        pme_log(LOG_DEBUG, "Register alarm with %u seconds.", seconds);
}

static void pme_init(const char *app_name) {
    init_libnotify(app_name);
    message = get_notify_message(message_summary, message_body, message_icon, message_urgency);
    wl_list_init(&seats);
}

static void pme_terminate(int exit_code) {
    pme_log(LOG_INFO, "Terminating.");
    wl_display_disconnect(display);
    wl_event_loop_destroy(event_loop);
    destroy_libnotify(message);
    exit(exit_code);
}

void pme_log_init(enum log_level verbosity) {
    if (verbosity < LOG_LEVEL_LAST) {
        curr_log_level = verbosity;
    }
}

void _pme_log(enum log_level verbosity, const char *fmt, ...) {
    if (verbosity > curr_log_level) {
        return;
    }

    va_list args;
    va_start(args, fmt);

    // prefix the time to the log message
    struct tm result;
    time_t t = time(NULL);
    struct tm *tm_info = localtime_r(&t, &result);
    char buffer[26];

    // generate time prefix
    strftime(buffer, sizeof(buffer), "%F %T - ", tm_info);
    fprintf(stderr, "%s", buffer);

    unsigned c = (verbosity < LOG_LEVEL_LAST)
        ? verbosity : LOG_LEVEL_LAST - 1;

    if (isatty(STDERR_FILENO)) {
        fprintf(stderr, "%s", verbosity_colors[c]);
    }

    vfprintf(stderr, fmt, args);

    if (isatty(STDERR_FILENO)) {
        fprintf(stderr, "\x1B[0m");
    }
    fprintf(stderr, "\n");

    va_end(args);
}

static int handle_signal(int sig, void *data) {
    switch (sig) {
        case SIGINT:
        case SIGTERM:
            pme_log(LOG_DEBUG, "Got SIGTERM.");
            pme_terminate(0);
            return 0;
        case SIGUSR1:
            pme_log(LOG_DEBUG, "Got SIGUSR1.");
            // Cancel the alarm.
            register_alarm(0);
            // Reset the alarm.
            time_left = 0;
            register_alarm(alarm_seconds);
            return 0;
        case SIGUSR2:
            pme_log(LOG_DEBUG, "Got SIGUSR2.");
            if (inhibited) {
                // Resume.
                pme_log(LOG_INFO, "Resumed.");
                time_left = 0;
                idle_timestamp = time(NULL);
                register_alarm(alarm_seconds);
            } else {
                // Inhibit.
                pme_log(LOG_INFO, "Inhibited.");
                register_alarm(0);
            }
            inhibited = !inhibited;
            return 0;
        case SIGALRM:
            pme_log(LOG_DEBUG, "Got SIGALRM.");
            // If time_left is zero, show the notification message.
            // Otherwise, alarm in time_left seconds.
            if (time_left == 0) {
                show_notify_message(message);
                if (command != NULL)
                    run_command(command);
                register_alarm(alarm_seconds);
            } else {
                register_alarm(time_left);
                time_left = 0;
            }
            return 0;
    }
    abort(); // not reached
}

static void seat_handle_capabilities(void *data, struct wl_seat *seat,
        uint32_t capabilities) {
    struct seat *self = data;
    self->capabilities = capabilities;
}

static void seat_handle_name(void *data, struct wl_seat *seat,
        const char *name) {
    struct seat *self = data;
    self->name = strdup(name);
}

static const struct wl_seat_listener wl_seat_listener = {
    .name = seat_handle_name,
    .capabilities = seat_handle_capabilities,
};

static void handle_global(void *data, struct wl_registry *registry,
        uint32_t name, const char *interface, uint32_t version) {
    pme_log(LOG_DEBUG, "Found interface %s.", interface);
    if (strcmp(interface, ext_idle_notifier_v1_interface.name) == 0) {
        idle_notifier =
            wl_registry_bind(registry, name, &ext_idle_notifier_v1_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        struct seat *s = calloc(1, sizeof(struct seat));
        s->proxy = wl_registry_bind(registry, name, &wl_seat_interface, 2);

        wl_seat_add_listener(s->proxy, &wl_seat_listener, s);
        wl_list_insert(&seats, &s->link);
    }
}

static void handle_global_remove(void *data, struct wl_registry *registry,
        uint32_t name) {
    // blank
}

static const struct wl_registry_listener registry_listener = {
    .global = handle_global,
    .global_remove = handle_global_remove,
};

static void handle_idled(void *data, struct ext_idle_notification_v1 *notif) {
    pme_log(LOG_DEBUG, "Idled.");
    idle_timestamp = time(NULL);
}

static void handle_resumed(void *data, struct ext_idle_notification_v1 *notif) {
    pme_log(LOG_DEBUG, "Resumed.");
    time_left += time(NULL) - idle_timestamp;
    pme_log(LOG_DEBUG, "Idle time: %u.", time_left);
}

static const struct ext_idle_notification_v1_listener idle_notification_listener = {
    .idled = handle_idled,
    .resumed = handle_resumed,
};

static int display_event(int fd, uint32_t mask, void *data) {
    if ((mask & WL_EVENT_HANGUP) || (mask & WL_EVENT_ERROR)) {
        pme_terminate(0);
    }

    int count = 0;
    if (mask & WL_EVENT_READABLE) {
        count = wl_display_dispatch(display);
    }
    if (mask & WL_EVENT_WRITABLE) {
        wl_display_flush(display);
    }
    if (mask == 0) {
        count = wl_display_dispatch_pending(display);
        wl_display_flush(display);
    }

    if (count < 0) {
        pme_log_errno(LOG_ERROR, "wl_display_dispatch failed, exiting.");
        pme_terminate(0);
    }

    return count;
}

void ext_idle_notify_v1_setup(unsigned int timeout) {
    event_loop = wl_event_loop_create();
    wl_event_loop_add_signal(event_loop, SIGINT, handle_signal, NULL);
    wl_event_loop_add_signal(event_loop, SIGTERM, handle_signal, NULL);
    wl_event_loop_add_signal(event_loop, SIGUSR1, handle_signal, NULL);
    wl_event_loop_add_signal(event_loop, SIGUSR2, handle_signal, NULL);
    wl_event_loop_add_signal(event_loop, SIGALRM, handle_signal, NULL);

    display = wl_display_connect(NULL);
    if (display == NULL) {
        pme_log(LOG_ERROR, "Cannot connect to wayland display.");
        pme_terminate(-1);
    }
    pme_log(LOG_DEBUG, "Connected to wayland display.");

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);
    pme_log(LOG_DEBUG, "Got registry and registerd listener.");

    struct seat *seat_i;
    wl_list_for_each(seat_i, &seats, link) {
        if (seat_name == NULL || strcmp(seat_i->name, seat_name) == 0) {
            seat = seat_i->proxy;
        }
    }

    if (idle_notifier == NULL) {
        pme_log(LOG_ERROR, "Compositor doesn't support ext_idle_notify_v1 protocol.");
        pme_terminate(-2);
    }
    pme_log(LOG_DEBUG, "Got idle notifier object.");
    if (seat == NULL) {
        if (seat_name != NULL) {
            pme_log(LOG_ERROR, "Seat %s not found.", seat_name);
        } else {
            pme_log(LOG_ERROR, "No seat found.");
        }
        pme_terminate(-3);
    }
    pme_log(LOG_DEBUG, "Found seat.");

    idle_notification =
        ext_idle_notifier_v1_get_idle_notification(idle_notifier, timeout, seat);
    ext_idle_notification_v1_add_listener(idle_notification,
        &idle_notification_listener, NULL);
    pme_log(LOG_DEBUG, "Got idle notification object and registered listener.");
    register_alarm(alarm_seconds);
    wl_display_roundtrip(display);

    struct wl_event_source *source = wl_event_loop_add_fd(event_loop,
        wl_display_get_fd(display), WL_EVENT_READABLE,
        display_event, NULL);
    wl_event_source_check(source);
    pme_log(LOG_DEBUG, "Setup display event loop and callback. Entering event loop.");

    while (wl_event_loop_dispatch(event_loop, -1) != 1) {
        // blank
    }
}

static void print_usage(int argc, char *argv[]) {
    printf("Usage: %s [OPTIONS]\n", argv[0]);
    printf("\t-h\tthis help message\n");
    printf("\t-i\tidle timeout (ms) - seat with no activity within this timeout are considered as idled\n");
    printf("\t-t\talarm interval (s) - the time after which %s will alarm you\n", argv[0]);
    printf("\t-s\talarm message summary\n");
    printf("\t-b\talarm message body\n");
    printf("\t-c\talarm message icon\n");
    printf("\t-u\talarm message urgency\n");
    printf("\t-S\tspecify seat name\n");
    printf("\t-x\tcommand to execute when timeout reached\n");
    printf("\t-d\tdebug mode - enable debug log\n");
}

static void parse_args(int argc, char *argv[]) {
    int c;
    char *inval_ptr;
    while ((c = getopt(argc, argv, "i:t:dhs:b:c:u:S:x:")) != -1) {
        switch (c) {
            case 'i':
                unsigned long timeout = strtoul(optarg, &inval_ptr, 0);
                if (timeout == ULONG_MAX) {
                    pme_log_errno(LOG_ERROR, "Parse idle timeout %s failed.", optarg);
                    pme_terminate(1);
                }
                if (timeout > UINT_MAX) {
                    pme_log(LOG_ERROR, "Idle timeout %lu too large, should less than UINT_MAX.", timeout);
                    pme_terminate(2);
                }
                idle_timeout = (unsigned int)timeout;
                pme_log(LOG_INFO, "Got idle timeout %ums.", idle_timeout);
                break;
            case 't':
                unsigned long interval = strtoul(optarg, &inval_ptr, 0);
                if (interval == ULONG_MAX) {
                    pme_log_errno(LOG_ERROR, "Parse alarm interval %s failed.", optarg);
                    pme_terminate(1);
                }
                if (interval > UINT_MAX) {
                    pme_log(LOG_ERROR, "Alarm interval %lu too large, should less than UINT_MAX.", interval);
                    pme_terminate(2);
                }
                alarm_seconds = (unsigned int)interval;
                pme_log(LOG_INFO, "Got alarm interval: %us.", alarm_seconds);
                break;
            case 's':
                message_summary = strdup(optarg);
                pme_log(LOG_INFO, "Got message summary: %s.", message_summary);
                break;
            case 'b':
                message_body = strdup(optarg);
                pme_log(LOG_INFO, "Got message body: %s.", message_body);
                break;
            case 'c':
                message_icon = strdup(optarg);
                pme_log(LOG_INFO, "Got message icon: %s.", message_icon);
                break;
            case 'u':
                message_urgency = atoi(optarg);
                if (message_urgency == 0) {
                    pme_log_errno(LOG_ERROR, "Parse message urgency %s failed.", optarg);
                    pme_terminate(1);
                }
                if (message_urgency < NOTIFY_URGENCY_LOW || message_urgency > NOTIFY_URGENCY_CRITICAL) {
                    pme_log(LOG_ERROR, "Message urgency %d invalid, should between %d and %d.",
                            message_urgency, NOTIFY_URGENCY_LOW , NOTIFY_URGENCY_CRITICAL);
                    pme_terminate(2);
                }
                pme_log(LOG_INFO, "Got message urgency: %d.", message_urgency);
                break;
            case 'S':
                seat_name = strdup(optarg);
                pme_log(LOG_INFO, "Got seat name: %s.", seat_name);
                break;
            case 'x':
                command = strdup(optarg);
                pme_log(LOG_INFO, "Got timeout command: %s.", command);
                break;
            case 'd':
                pme_log_init(LOG_DEBUG);
                break;
            case 'h':
                print_usage(argc, argv);
                exit(0);
                break;
            case '?':
                print_usage(argc, argv);
                exit(1);
                break;
            default:
                break;
        }
    }
}

int main(int argc, char *argv[]) {
    parse_args(argc, argv);
    pme_init(argv[0]);
    // TODO: parse_configs();
    ext_idle_notify_v1_setup(idle_timeout);
    pme_terminate(0);
}

