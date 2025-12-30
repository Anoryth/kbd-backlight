/*
 * kbd-backlight-daemon - Keyboard backlight controller based on input activity
 *
 * Monitors mouse/touchpad/keyboard input events and adjusts keyboard backlight brightness.
 * Designed for Framework Laptop 13 running Linux.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <linux/input.h>

#define DEFAULT_BRIGHTNESS_PATH "/sys/class/leds/chromeos::kbd_backlight/brightness"
#define DEFAULT_MAX_BRIGHTNESS_PATH "/sys/class/leds/chromeos::kbd_backlight/max_brightness"
#define DEFAULT_TIMEOUT_SEC 5
#define DEFAULT_FADE_STEPS 10
#define DEFAULT_FADE_INTERVAL_MS 50
#define MAX_INPUT_DEVICES 32
#define INPUT_DEV_PATH "/dev/input"
#define CONFIG_PATH "/etc/kbd-backlight-daemon.conf"

typedef struct {
    char brightness_path[256];
    char max_brightness_path[256];
    int timeout_sec;
    int fade_steps;
    int fade_interval_ms;
    int target_brightness;
    int dim_brightness;
} Config;

static volatile sig_atomic_t running = 1;
static Config config;
static int current_brightness = 0;
static int max_brightness = 100;
static int input_fds[MAX_INPUT_DEVICES];
static int input_fd_count = 0;
static int last_written_brightness = -1; /* Track what we last wrote to detect external changes */

static void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

static int read_int_from_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    int value;
    if (fscanf(f, "%d", &value) != 1) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return value;
}

static int write_int_to_file(const char *path, int value) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;

    fprintf(f, "%d", value);
    fclose(f);
    return 0;
}

static void set_brightness(int brightness) {
    if (brightness < 0) brightness = 0;
    if (brightness > max_brightness) brightness = max_brightness;

    if (brightness != current_brightness) {
        if (write_int_to_file(config.brightness_path, brightness) == 0) {
            current_brightness = brightness;
            last_written_brightness = brightness;
        }
    }
}

static void fade_brightness(int from, int to) {
    if (from == to) return;

    int step = (to - from) / config.fade_steps;
    if (step == 0) step = (to > from) ? 1 : -1;

    int current = from;
    struct timespec delay = {
        .tv_sec = 0,
        .tv_nsec = config.fade_interval_ms * 1000000L
    };

    while (running) {
        current += step;

        if ((step > 0 && current >= to) || (step < 0 && current <= to)) {
            set_brightness(to);
            break;
        }

        set_brightness(current);
        nanosleep(&delay, NULL);
    }
}

static int is_input_device(const char *path, const char **device_type) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return 0;

    unsigned long evbits = 0;
    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), &evbits) < 0) {
        close(fd);
        return 0;
    }

    /* Check for EV_KEY (keyboard/buttons) */
    int has_key = (evbits & (1 << EV_KEY)) != 0;
    /* Check for EV_REL (relative movement - mouse) */
    int has_rel = (evbits & (1 << EV_REL)) != 0;
    /* Check for EV_ABS (absolute - touchpad) */
    int has_abs = (evbits & (1 << EV_ABS)) != 0;

    /* Check for keyboard - has many keys including letters */
    if (has_key) {
        unsigned long keybits[KEY_MAX / 8 / sizeof(unsigned long) + 1] = {0};
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) >= 0) {
            /* Check if it has letter keys (A-Z) - indicates a keyboard */
            int has_letters = 0;
            for (int k = KEY_Q; k <= KEY_P; k++) {
                if (keybits[k / (8 * sizeof(unsigned long))] & (1UL << (k % (8 * sizeof(unsigned long))))) {
                    has_letters++;
                }
            }
            if (has_letters >= 5) {
                close(fd);
                *device_type = "keyboard";
                return 1;
            }
        }
    }

    /* Check for relative X/Y axes (mouse movement) */
    if (has_rel) {
        unsigned long relbits = 0;
        if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relbits)), &relbits) >= 0) {
            if ((relbits & (1 << REL_X)) && (relbits & (1 << REL_Y))) {
                close(fd);
                *device_type = "mouse";
                return 1;
            }
        }
    }

    /* Check for absolute X/Y axes (touchpad) */
    if (has_abs) {
        unsigned long absbits[2] = {0};
        if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) >= 0) {
            if ((absbits[0] & (1 << ABS_X)) && (absbits[0] & (1 << ABS_Y))) {
                close(fd);
                *device_type = "touchpad";
                return 1;
            }
        }
    }

    close(fd);
    return 0;
}

static void open_input_devices(void) {
    DIR *dir = opendir(INPUT_DEV_PATH);
    if (!dir) {
        fprintf(stderr, "Failed to open %s: %s\n", INPUT_DEV_PATH, strerror(errno));
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && input_fd_count < MAX_INPUT_DEVICES) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", INPUT_DEV_PATH, entry->d_name);

        const char *device_type = NULL;
        if (!is_input_device(path, &device_type)) continue;

        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            fprintf(stderr, "Failed to open %s: %s\n", path, strerror(errno));
            continue;
        }

        input_fds[input_fd_count++] = fd;
        fprintf(stderr, "Monitoring %s: %s\n", device_type, path);
    }

    closedir(dir);
}

static void close_input_devices(void) {
    for (int i = 0; i < input_fd_count; i++) {
        close(input_fds[i]);
    }
    input_fd_count = 0;
}

/*
 * Check if brightness was changed externally (e.g., Fn+Space hotkey).
 * Uses polling since the ChromeOS EC doesn't generate uevents.
 * Returns: 1 if turned on externally, 0 if turned off or no change, -1 if turned off externally.
 */
static int check_external_brightness_change(void) {
    int actual_brightness = read_int_from_file(config.brightness_path);
    if (actual_brightness < 0) return 0;

    /* Detect external change: brightness differs from what we last wrote */
    if (last_written_brightness >= 0 && actual_brightness != last_written_brightness) {
        int old_brightness = last_written_brightness;
        current_brightness = actual_brightness;
        last_written_brightness = actual_brightness;

        if (actual_brightness > 0) {
            /* User turned brightness ON or changed level */
            config.target_brightness = actual_brightness;
            fprintf(stderr, "External brightness change: %d -> %d (new target)\n",
                    old_brightness, actual_brightness);
            return 1;
        } else {
            /* User turned brightness OFF - respect their choice */
            fprintf(stderr, "External brightness off: %d -> 0 (user disabled)\n", old_brightness);
            return -1;
        }
    }

    return 0;
}

static char *trim(char *str) {
    /* Trim leading whitespace */
    while (*str == ' ' || *str == '\t') str++;

    /* Trim trailing whitespace */
    char *end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
        *end = '\0';
        end--;
    }
    return str;
}

static void load_config(void) {
    /* Set defaults */
    strncpy(config.brightness_path, DEFAULT_BRIGHTNESS_PATH, sizeof(config.brightness_path));
    strncpy(config.max_brightness_path, DEFAULT_MAX_BRIGHTNESS_PATH, sizeof(config.max_brightness_path));
    config.timeout_sec = DEFAULT_TIMEOUT_SEC;
    config.fade_steps = DEFAULT_FADE_STEPS;
    config.fade_interval_ms = DEFAULT_FADE_INTERVAL_MS;
    config.target_brightness = -1; /* -1 means use current */
    config.dim_brightness = 0;

    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        fprintf(stderr, "Config file not found at %s, using defaults\n", CONFIG_PATH);
        return;
    }

    fprintf(stderr, "Loading config from %s\n", CONFIG_PATH);

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        /* Skip comments and empty lines */
        char *trimmed = trim(line);
        if (trimmed[0] == '#' || trimmed[0] == '\0') continue;

        char *eq = strchr(trimmed, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = trim(trimmed);
        char *value = trim(eq + 1);

        if (strcmp(key, "brightness_path") == 0) {
            strncpy(config.brightness_path, value, sizeof(config.brightness_path) - 1);
        } else if (strcmp(key, "max_brightness_path") == 0) {
            strncpy(config.max_brightness_path, value, sizeof(config.max_brightness_path) - 1);
        } else if (strcmp(key, "timeout") == 0) {
            config.timeout_sec = atoi(value);
            fprintf(stderr, "  timeout=%d\n", config.timeout_sec);
        } else if (strcmp(key, "fade_steps") == 0) {
            config.fade_steps = atoi(value);
            fprintf(stderr, "  fade_steps=%d\n", config.fade_steps);
        } else if (strcmp(key, "fade_interval_ms") == 0) {
            config.fade_interval_ms = atoi(value);
            fprintf(stderr, "  fade_interval_ms=%d\n", config.fade_interval_ms);
        } else if (strcmp(key, "target_brightness") == 0) {
            config.target_brightness = atoi(value);
            fprintf(stderr, "  target_brightness=%d\n", config.target_brightness);
        } else if (strcmp(key, "dim_brightness") == 0) {
            config.dim_brightness = atoi(value);
            fprintf(stderr, "  dim_brightness=%d\n", config.dim_brightness);
        }
    }

    fclose(f);
}

static void daemonize(void) {
    pid_t pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);

    if (setsid() < 0) exit(EXIT_FAILURE);

    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);

    umask(0);
    chdir("/");

    /* Close standard file descriptors */
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
}

int main(int argc, char *argv[]) {
    int foreground = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--foreground") == 0) {
            foreground = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [OPTIONS]\n", argv[0]);
            printf("Options:\n");
            printf("  -f, --foreground  Run in foreground (don't daemonize)\n");
            printf("  -h, --help        Show this help message\n");
            return 0;
        }
    }

    load_config();

    /* Read max brightness */
    max_brightness = read_int_from_file(config.max_brightness_path);
    if (max_brightness <= 0) {
        fprintf(stderr, "Failed to read max brightness from %s\n", config.max_brightness_path);
        return 1;
    }

    /* Read current brightness as target if not configured */
    current_brightness = read_int_from_file(config.brightness_path);
    if (current_brightness < 0) {
        fprintf(stderr, "Failed to read current brightness from %s\n", config.brightness_path);
        return 1;
    }

    if (config.target_brightness < 0) {
        config.target_brightness = current_brightness > 0 ? current_brightness : max_brightness / 2;
    }

    fprintf(stderr, "kbd-backlight-daemon starting\n");
    fprintf(stderr, "Max brightness: %d, Target: %d, Timeout: %ds\n",
            max_brightness, config.target_brightness, config.timeout_sec);

    open_input_devices();

    if (input_fd_count == 0) {
        fprintf(stderr, "No keyboard/mouse/touchpad input devices found\n");
        return 1;
    }

    /* Setup signal handlers */
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    if (!foreground) {
        daemonize();
    }

    /* Initial state: brightness on */
    set_brightness(config.target_brightness);

    time_t last_activity = time(NULL);
    int is_dimmed = 0;
    int user_disabled = 0;  /* User explicitly turned off backlight */

    /*
     * Polling strategy for external brightness changes (Fn+Space):
     * - When active (not dimmed): poll every 200ms for responsive hotkey detection
     * - When dimmed/disabled: poll every 2 seconds (user is away, less urgent)
     */
    const int POLL_INTERVAL_ACTIVE_MS = 200;
    const int POLL_INTERVAL_IDLE_MS = 2000;

    while (running) {
        fd_set read_fds;
        FD_ZERO(&read_fds);

        int max_fd = 0;
        for (int i = 0; i < input_fd_count; i++) {
            FD_SET(input_fds[i], &read_fds);
            if (input_fds[i] > max_fd) max_fd = input_fds[i];
        }

        /* Use shorter timeout when active for responsive brightness polling */
        int poll_interval_ms = (is_dimmed || user_disabled) ? POLL_INTERVAL_IDLE_MS : POLL_INTERVAL_ACTIVE_MS;
        struct timeval tv = {
            .tv_sec = poll_interval_ms / 1000,
            .tv_usec = (poll_interval_ms % 1000) * 1000
        };

        int ret = select(max_fd + 1, &read_fds, NULL, NULL, &tv);

        time_t now = time(NULL);

        /* Poll for external brightness changes */
        int brightness_change = check_external_brightness_change();
        if (brightness_change == 1) {
            /* User turned ON or changed brightness */
            last_activity = now;
            user_disabled = 0;
            is_dimmed = 0;
        } else if (brightness_change == -1) {
            /* User turned OFF brightness - respect their choice */
            user_disabled = 1;
            is_dimmed = 0;
        }

        if (ret > 0) {
            /* Check for input activity */
            int input_activity = 0;
            for (int i = 0; i < input_fd_count; i++) {
                if (FD_ISSET(input_fds[i], &read_fds)) {
                    struct input_event ev;
                    while (read(input_fds[i], &ev, sizeof(ev)) == sizeof(ev)) {
                        /* Drain the buffer */
                    }
                    input_activity = 1;
                }
            }

            if (input_activity) {
                last_activity = now;

                /* Only restore brightness if not disabled by user */
                if (is_dimmed && !user_disabled) {
                    fade_brightness(current_brightness, config.target_brightness);
                    is_dimmed = 0;
                }
            }
        }

        /* Check for timeout (inactivity) - only if not already dimmed and not user-disabled */
        if (!is_dimmed && !user_disabled && (now - last_activity) >= config.timeout_sec) {
            fade_brightness(current_brightness, config.dim_brightness);
            is_dimmed = 1;
        }
    }

    /* Cleanup */
    close_input_devices();

    /* Restore brightness on exit */
    set_brightness(config.target_brightness);

    return 0;
}
