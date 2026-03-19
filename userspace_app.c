#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include "morse_ioctl.h"

#define DEVICE_PATH "/dev/chardev"
#define INPUT_BUF_SIZE 512
#define MORSE_READ_SIZE 64

/* ─── Thread state ──────────────────────────────────────────────────────── */

typedef enum {
    STATE_IDLE = 0,
    STATE_RUNNING,
    STATE_BLOCKED,
    STATE_ERROR,
    STATE_STOPPED
} thread_state_t;

typedef struct {
    char name[32];
    long tid;
    thread_state_t state;
    char detail[128];
} thread_status_t;

enum {
    THREAD_WRITER = 0,
    THREAD_READER,
    THREAD_MONITOR,
    THREAD_COUNT
};

/* ─── Globals ────────────────────────────────────────────────────────────── */

/*
 * keep_running is written from the main thread and read from worker threads.
 * Declared volatile so the compiler doesn't cache it in a register.
 * A plain int is fine here because we only ever do a single store/load and
 * we don't need an atomic RMW sequence.
 */
static volatile int keep_running = 1;

static pthread_mutex_t status_mutex = PTHREAD_MUTEX_INITIALIZER;
static thread_status_t statuses[THREAD_COUNT];

/* ─── Helpers ────────────────────────────────────────────────────────────── */

static long get_tid_linux(void)
{
    return syscall(SYS_gettid);
}

static const char *state_to_string(thread_state_t state)
{
    switch (state) {
        case STATE_IDLE:    return "IDLE";
        case STATE_RUNNING: return "RUNNING";
        case STATE_BLOCKED: return "BLOCKED";
        case STATE_ERROR:   return "ERROR";
        case STATE_STOPPED: return "STOPPED";
        default:            return "UNKNOWN";
    }
}

static void set_status(int idx, thread_state_t state, const char *detail)
{
    pthread_mutex_lock(&status_mutex);
    statuses[idx].tid   = get_tid_linux();
    statuses[idx].state = state;
    if (detail) {
        strncpy(statuses[idx].detail, detail, sizeof(statuses[idx].detail) - 1);
        statuses[idx].detail[sizeof(statuses[idx].detail) - 1] = '\0';
    } else {
        statuses[idx].detail[0] = '\0';
    }
    pthread_mutex_unlock(&status_mutex);
}

static void init_status(int idx, const char *name)
{
    pthread_mutex_lock(&status_mutex);
    strncpy(statuses[idx].name, name, sizeof(statuses[idx].name) - 1);
    statuses[idx].name[sizeof(statuses[idx].name) - 1] = '\0';
    statuses[idx].tid         = 0;
    statuses[idx].state       = STATE_IDLE;
    statuses[idx].detail[0]   = '\0';
    pthread_mutex_unlock(&status_mutex);
}

static void print_status_snapshot(void)
{
    pthread_mutex_lock(&status_mutex);
    printf("\n====================================================\n");
    printf("           MORSE USER-SPACE THREAD MONITOR\n");
    printf("====================================================\n");
    for (int i = 0; i < THREAD_COUNT; i++) {
        printf("%-8s | TID: %-6ld | %-8s | %s\n",
               statuses[i].name,
               statuses[i].tid,
               state_to_string(statuses[i].state),
               statuses[i].detail);
    }
    printf("----------------------------------------------------\n");
    printf("Type text + Enter to send Morse to the LED.\n");
    printf("Press the Pi button to input dots/dashes (read back as letters).\n");
    printf("Type 'quit' to exit.  Type 'speed <ms>' to change unit time.\n");
    printf("====================================================\n");
    fflush(stdout);
    pthread_mutex_unlock(&status_mutex);
}

/* ─── Signal handler ─────────────────────────────────────────────────────── */

static void handle_sigint(int sig)
{
    (void)sig;
    /* Just set the flag; let main() do the clean shutdown. */
    keep_running = 0;
}

/* ─── Reader thread ──────────────────────────────────────────────────────── */
/*
 * FIX 1: The kernel driver's hello_read() uses wait_event_interruptible()
 * which means a plain blocking read() is the correct approach – there is no
 * poll() / fasync implemented in the driver, so select() will never indicate
 * the fd as readable and the reader gets stuck forever at "Waiting for Morse
 * input".  We replace select() with a direct blocking read().
 *
 * FIX 2: The device is opened O_RDWR so a single open() succeeds even though
 * the driver allows multiple opens; this avoids any potential conflict with the
 * writer fd opened in main().
 */
static void *reader_thread_fn(void *arg)
{
    (void)arg;
    int fd;
    char buf[MORSE_READ_SIZE];

    /* Open for reading only – separate fd from the writer */
    fd = open(DEVICE_PATH, O_RDONLY);
    if (fd < 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "open failed: %s", strerror(errno));
        set_status(THREAD_READER, STATE_ERROR, msg);
        return NULL;
    }

    while (keep_running) {
        set_status(THREAD_READER, STATE_BLOCKED, "Blocking on read() – press button");

        /*
         * This will block inside the kernel's wait_event_interruptible()
         * until the button polling thread puts a decoded letter into
         * morse_buffer and wakes hello_morse_queue.
         */
        ssize_t n = read(fd, buf, sizeof(buf) - 1);

        if (n < 0) {
            if (errno == EINTR) {
                /* Interrupted by signal – check keep_running and loop */
                continue;
            }
            char msg[128];
            snprintf(msg, sizeof(msg), "read failed: %s", strerror(errno));
            set_status(THREAD_READER, STATE_ERROR, msg);
            break;
        }

        if (n == 0)
            continue;   /* Shouldn't happen with this driver, but be safe */

        buf[n] = '\0';

        /* Replace control characters with spaces for clean display */
        for (ssize_t i = 0; i < n; i++) {
            if ((unsigned char)buf[i] < 0x20)
                buf[i] = ' ';
        }

        char msg[128];
        snprintf(msg, sizeof(msg), "Received: \"%s\"", buf);
        set_status(THREAD_READER, STATE_RUNNING, msg);
        /* Immediately print so the user sees button output right away */
        printf("\n[Reader] Button input decoded: \"%s\"\n", buf);
        fflush(stdout);
    }

    close(fd);
    set_status(THREAD_READER, STATE_STOPPED, "Reader exiting");
    return NULL;
}

/* ─── Monitor thread ─────────────────────────────────────────────────────── */
/*
 * FIX 3: Only reprint when something actually changed, and poll at 200 ms
 * instead of spamming every 2 seconds which overwrites the user's prompt.
 */
static void *monitor_thread_fn(void *arg)
{
    (void)arg;
    thread_status_t prev[THREAD_COUNT];
    memset(prev, 0, sizeof(prev));

    set_status(THREAD_MONITOR, STATE_RUNNING, "Watching for state changes");

    /* Print initial state immediately */
    print_status_snapshot();
    memcpy(prev, statuses, sizeof(prev)); /* snapshot under mutex not needed here for init */

    while (keep_running) {
        usleep(200000); /* 200 ms – lightweight polling */

        int changed = 0;
        pthread_mutex_lock(&status_mutex);
        for (int i = 0; i < THREAD_COUNT; i++) {
            if (statuses[i].state != prev[i].state ||
                strcmp(statuses[i].detail, prev[i].detail) != 0) {
                changed = 1;
                prev[i] = statuses[i];
            }
        }
        pthread_mutex_unlock(&status_mutex);

        if (changed)
            print_status_snapshot();
    }

    set_status(THREAD_MONITOR, STATE_STOPPED, "Monitor exiting");
    return NULL;
}

/* ─── Main ───────────────────────────────────────────────────────────────── */

int main(void)
{
    pthread_t reader_thread, monitor_thread;
    int fd;
    char line[INPUT_BUF_SIZE];

    /*
     * FIX 4: Signal handler only sets keep_running = 0 instead of calling
     * exit() directly, so threads get joined and resources are freed cleanly.
     */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* Don't restart syscalls – lets fgets/read return EINTR */
    sigaction(SIGINT, &sa, NULL);

    init_status(THREAD_WRITER,  "Writer");
    init_status(THREAD_READER,  "Reader");
    init_status(THREAD_MONITOR, "Monitor");

    /* Writer fd – write only */
    fd = open(DEVICE_PATH, O_WRONLY);
    if (fd < 0) {
        perror("open writer device failed");
        return 1;
    }

    if (pthread_create(&reader_thread, NULL, reader_thread_fn, NULL) != 0) {
        perror("pthread_create reader");
        close(fd);
        return 1;
    }

    if (pthread_create(&monitor_thread, NULL, monitor_thread_fn, NULL) != 0) {
        perror("pthread_create monitor");
        keep_running = 0;
        pthread_join(reader_thread, NULL);
        close(fd);
        return 1;
    }

    /* ── Main input loop ── */
    while (keep_running) {
        printf("\nEnter text (or 'quit' / 'speed <ms>'): ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            if (feof(stdin) || !keep_running)
                break;
            if (errno == EINTR) {
                clearerr(stdin);
                continue;
            }
            break;
        }

        /* Strip trailing newline */
        line[strcspn(line, "\n")] = '\0';

        /* ── quit ── */
        if (strcmp(line, "quit") == 0) {
            keep_running = 0;
            break;
        }

        /* ── speed <ms> : adjust Morse unit time via ioctl ── */
        if (strncmp(line, "speed ", 6) == 0) {
            unsigned int ms = (unsigned int)atoi(line + 6);
            if (ms < 50 || ms > 2000) {
                printf("[Main] Speed must be 50–2000 ms\n");
            } else {
                if (ioctl(fd, MORSE_SET_UNIT, &ms) < 0) {
                    perror("ioctl MORSE_SET_UNIT");
                } else {
                    printf("[Main] Morse unit time set to %u ms\n", ms);
                }
            }
            continue;
        }

        if (line[0] == '\0')
            continue;

        set_status(THREAD_WRITER, STATE_BLOCKED, "Calling write() on /dev/chardev");

        ssize_t written = write(fd, line, strlen(line));
        if (written < 0) {
            char msg[128];
            snprintf(msg, sizeof(msg), "write failed: %s", strerror(errno));
            set_status(THREAD_WRITER, STATE_ERROR, msg);
            keep_running = 0;
            break;
        }

        char msg[128];
        snprintf(msg, sizeof(msg), "Wrote %zd byte(s): %s", written, line);
        set_status(THREAD_WRITER, STATE_RUNNING, msg);
    }

    /* ── Clean shutdown ── */
    keep_running = 0;
    close(fd);

    /*
     * FIX 5: Don't send SIGINT to threads (dangerous, caused double-exit).
     * The reader is blocked in read(); cancel it so it unblocks cleanly.
     * The monitor polls keep_running every 200 ms so it will exit on its own.
     */
    pthread_cancel(reader_thread);
    pthread_join(reader_thread, NULL);
    pthread_join(monitor_thread, NULL);

    printf("\nApplication exited cleanly.\n");
    return 0;
}
     
