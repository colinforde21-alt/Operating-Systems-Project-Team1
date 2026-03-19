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
    THREAD_INPUT = 0,
    THREAD_WRITER,
    THREAD_READER,
    THREAD_COUNT
};

static volatile sig_atomic_t keep_running = 1;

static pthread_mutex_t status_mutex = PTHREAD_MUTEX_INITIALIZER;
static thread_status_t statuses[THREAD_COUNT];

static pthread_barrier_t startup_barrier;

static pthread_mutex_t input_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t input_cond = PTHREAD_COND_INITIALIZER;
static char pending_input[INPUT_BUF_SIZE];
static int input_ready = 0;

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

    statuses[idx].tid = get_tid_linux();
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
    statuses[idx].tid = 0;
    statuses[idx].state = STATE_IDLE;
    statuses[idx].detail[0] = '\0';

    pthread_mutex_unlock(&status_mutex);
}

static void print_status_snapshot(void)
{
    pthread_mutex_lock(&status_mutex);

    printf("\n====================================================\n");
    printf("               MORSE DEVICE STATUS\n");
    printf("====================================================\n");
    for (int i = 0; i < THREAD_COUNT; i++) {
        printf("%-8s | TID: %-6ld | %-8s | %s\n",
               statuses[i].name,
               statuses[i].tid,
               state_to_string(statuses[i].state),
               statuses[i].detail);
    }
    printf("----------------------------------------------------\n");
    printf("  Device : " DEVICE_PATH "\n");
    printf("  Type 'help' for available commands.\n");
    printf("====================================================\n");
    fflush(stdout);

    pthread_mutex_unlock(&status_mutex);
}

static void handle_sigint(int sig)
{
    (void)sig;
    keep_running = 0;
    pthread_cond_broadcast(&input_cond);
}

static void set_unit(const char *arg) {
    unsigned int unit = (unsigned int)atoi(arg);
    int fd = open(DEVICE_PATH, O_RDWR);
    if (fd < 0) { perror("open"); return; }

    if (ioctl(fd, MORSE_SET_UNIT, &unit) < 0)
        perror("ioctl SET_UNIT");
    else
        printf("Unit set to %u ms\n", unit);

    close(fd);
}
static void get_unit(void) {
    unsigned int unit;
    int fd = open(DEVICE_PATH, O_RDWR);
    if (fd < 0) { perror("open"); return; }

    if (ioctl(fd, MORSE_GET_UNIT, &unit) < 0)
        perror("ioctl GET_UNIT");
    else
        printf("Current unit: %u ms\n", unit);

    close(fd);
}

static void print_help(void)
{
    printf("\nCommands:\n");
    printf("  status     - print thread status\n");
    printf("  unit=<ms>  - set morse unit size (50-2000)\n");
    printf("  unit?      - get current unit size\n");
    printf("  buffers    - show kernel buffer state\n");
    printf("  help       - show this message\n");
    printf("  quit       - exit\n\n");
    fflush(stdout);
}

static void *input_thread_fn(void *arg)
{
    pthread_barrier_wait(&startup_barrier);
    (void)arg;
    char line[INPUT_BUF_SIZE];

    set_status(THREAD_INPUT, STATE_RUNNING, "Waiting for terminal input");

    while (keep_running) {
        printf("\nEnter text> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            if (feof(stdin)) {
                keep_running = 0;
                pthread_cond_broadcast(&input_cond);
                break;
            }
            if (errno == EINTR) {
                clearerr(stdin);
                continue;
            }
            set_status(THREAD_INPUT, STATE_ERROR, "fgets() failed");
            keep_running = 0;
            pthread_cond_broadcast(&input_cond);
            break;
        }

        line[strcspn(line, "\n")] = '\0';

        if (strcmp(line, "quit") == 0) {
            set_status(THREAD_INPUT, STATE_RUNNING, "Quit requested");
            keep_running = 0;
            pthread_cond_broadcast(&input_cond);
            break;
        }

        if (line[0] == '\0') {
            set_status(THREAD_INPUT, STATE_IDLE, "Empty line ignored");
            continue;
        }
        
        if (strncmp(line, "unit=", 5) == 0) {
            set_unit(line + 5);
            continue; 
        }
        if (strcmp(line, "unit?") == 0) {
            get_unit();
            continue;
        }
        if (strcmp(line, "status") == 0) {
            print_status_snapshot();
            continue;
        }
        if (strcmp(line, "help") == 0) {
            print_help();
            continue;
        }

        pthread_mutex_lock(&input_mutex);
        while (input_ready && keep_running) {
            pthread_mutex_unlock(&input_mutex);
            usleep(100000);
            pthread_mutex_lock(&input_mutex);
        }

        if (!keep_running) {
            pthread_mutex_unlock(&input_mutex);
            break;
        }

        strncpy(pending_input, line, sizeof(pending_input) - 1);
        pending_input[sizeof(pending_input) - 1] = '\0';
        input_ready = 1;
        pthread_cond_signal(&input_cond);
        pthread_mutex_unlock(&input_mutex);

        set_status(THREAD_INPUT, STATE_RUNNING, "Queued text for writer thread");
    }

    set_status(THREAD_INPUT, STATE_STOPPED, "Input thread exiting");
    return NULL;
}

static void *writer_thread_fn(void *arg)
{
    pthread_barrier_wait(&startup_barrier);
    (void)arg;
    int fd;
    char local_buf[INPUT_BUF_SIZE];

    fd = open(DEVICE_PATH, O_WRONLY);
    if (fd < 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "open(%s) failed: %s", DEVICE_PATH, strerror(errno));
        set_status(THREAD_WRITER, STATE_ERROR, msg);
        keep_running = 0;
        pthread_cond_broadcast(&input_cond);
        return NULL;
    }

    set_status(THREAD_WRITER, STATE_IDLE, "Waiting for queued text");

    while (keep_running) {
        pthread_mutex_lock(&input_mutex);
        while (!input_ready && keep_running) {
            set_status(THREAD_WRITER, STATE_IDLE, "No pending text");
            pthread_cond_wait(&input_cond, &input_mutex);
        }

        if (!keep_running) {
            pthread_mutex_unlock(&input_mutex);
            break;
        }

        strncpy(local_buf, pending_input, sizeof(local_buf) - 1);
        local_buf[sizeof(local_buf) - 1] = '\0';
        input_ready = 0;
        pthread_mutex_unlock(&input_mutex);

        set_status(THREAD_WRITER, STATE_BLOCKED, "Calling write() on /dev/chardev");

        ssize_t written = write(fd, local_buf, strlen(local_buf));

        if (written < 0) {
            if (errno == EINTR && keep_running) {
                set_status(THREAD_WRITER, STATE_IDLE, "write() interrupted, retry later");
                continue;
            }
            char msg[128];
            snprintf(msg, sizeof(msg), "write() failed: %s", strerror(errno));
            set_status(THREAD_WRITER, STATE_ERROR, msg);
            keep_running = 0;
            pthread_cond_broadcast(&input_cond);
            break;
        }

        char msg[128];
        snprintf(msg, sizeof(msg), "write() returned %zd for \"%s\"", written, local_buf);
        set_status(THREAD_WRITER, STATE_RUNNING, msg);
    }

    close(fd);
    set_status(THREAD_WRITER, STATE_STOPPED, "Writer thread exiting");
    return NULL;
}

static void *reader_thread_fn(void *arg)
{
    pthread_barrier_wait(&startup_barrier);
    (void)arg;
    int fd;
    char buf[MORSE_READ_SIZE];

    fd = open(DEVICE_PATH, O_RDONLY);
    if (fd < 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "open(%s) failed: %s", DEVICE_PATH, strerror(errno));
        set_status(THREAD_READER, STATE_ERROR, msg);
        keep_running = 0;
        pthread_cond_broadcast(&input_cond);
        return NULL;
    }

    set_status(THREAD_READER, STATE_BLOCKED, "Waiting in blocking read()");

    while (keep_running) {
        ssize_t n;

        set_status(THREAD_READER, STATE_BLOCKED, "Calling blocking read() on /dev/chardev");
        n = read(fd, buf, sizeof(buf) - 1);

        if (!keep_running) {
            break;
        }

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            char msg[128];
            snprintf(msg, sizeof(msg), "read() failed: %s", strerror(errno));
            set_status(THREAD_READER, STATE_ERROR, msg);
            keep_running = 0;
            pthread_cond_broadcast(&input_cond);
            break;
        }

        if (n == 0) {
            set_status(THREAD_READER, STATE_IDLE, "read() returned 0");
            continue;
        }

        buf[n] = '\0';

        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n' || buf[i] == '\r') {
                buf[i] = ' ';
            }
        }

        char msg[128];
        snprintf(msg, sizeof(msg), "Received Morse input: \"%s\"", buf);
        set_status(THREAD_READER, STATE_RUNNING, msg);

    }

    close(fd);
    set_status(THREAD_READER, STATE_STOPPED, "Reader thread exiting");
    return NULL;
}


int main(void)
{
    pthread_t input_thread;
    pthread_t writer_thread;
    pthread_t reader_thread;

    signal(SIGINT, handle_sigint);

    init_status(THREAD_INPUT, "Input");
    init_status(THREAD_WRITER, "Writer");
    init_status(THREAD_READER, "Reader");

    if (pthread_create(&input_thread, NULL, input_thread_fn, NULL) != 0) {
        perror("pthread_create input");
        return 1;
    }

    if (pthread_create(&writer_thread, NULL, writer_thread_fn, NULL) != 0) {
        perror("pthread_create writer");
        keep_running = 0;
        pthread_cond_broadcast(&input_cond);
        pthread_join(input_thread, NULL);
        return 1;
    }

    if (pthread_create(&reader_thread, NULL, reader_thread_fn, NULL) != 0) {
        perror("pthread_create reader");
        keep_running = 0;
        pthread_cond_broadcast(&input_cond);
        pthread_join(input_thread, NULL);
        pthread_join(writer_thread, NULL);
        return 1;
    }

    pthread_barrier_init(&startup_barrier, NULL, 4);
    pthread_barrier_wait(&startup_barrier); // blocks until all 3 threads are ready
    print_status_snapshot();

    pthread_join(input_thread, NULL);

    keep_running = 0;
    pthread_cond_broadcast(&input_cond);

    pthread_cancel(reader_thread);
    pthread_cancel(writer_thread);

    pthread_join(writer_thread, NULL);
    pthread_join(reader_thread, NULL);

    pthread_barrier_destroy(&startup_barrier);

    printf("\nUser-space application exited.\n");
    return 0;
}
