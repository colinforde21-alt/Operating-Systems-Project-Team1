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
    THREAD_MONITOR,
    THREAD_COUNT
};

static volatile sig_atomic_t keep_running = 1;

static pthread_mutex_t status_mutex = PTHREAD_MUTEX_INITIALIZER;
static thread_status_t statuses[THREAD_COUNT];

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
    printf("Type text and press Enter to send Morse to the LED.\n");
    printf("Press the Pi button to generate '.' or '-' for read().\n");
    printf("Type 'quit' to exit.\n");
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

static void *input_thread_fn(void *arg)
{
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

#include <sys/select.h>

static void *reader_thread_fn(void *arg)
{
    (void)arg;
    int fd;
    char buf[64];

    fd = open("/dev/chardev", O_RDONLY);
    if (fd < 0) {
        perror("reader open failed");
        return NULL;
    }

    while (keep_running) {

        fd_set set;
        struct timeval timeout;

        FD_ZERO(&set);
        FD_SET(fd, &set);

        timeout.tv_sec = 2;
        timeout.tv_usec = 0;

        set_status(THREAD_READER, STATE_BLOCKED,
                   "Waiting for Morse input");

        int rv = select(fd + 1, &set, NULL, NULL, &timeout);

        if (!keep_running)
            break;

        if (rv == -1) {
            continue;
        }

        if (rv == 0) {
            // timeout → loop again
            continue;
        }

        ssize_t n = read(fd, buf, sizeof(buf) - 1);

        if (n > 0) {
            buf[n] = '\0';

            char msg[128];
            snprintf(msg, sizeof(msg),
                     "Received: %s", buf);

            set_status(THREAD_READER,
                       STATE_RUNNING,
                       msg);
        }
    }

    close(fd);
    set_status(THREAD_READER,
               STATE_STOPPED,
               "Reader exiting");

    return NULL;
}                         




                   

static void *monitor_thread_fn(void *arg)
{
    (void)arg;

    set_status(THREAD_MONITOR, STATE_RUNNING, "Printing thread states");

    while (keep_running) {
        print_status_snapshot();
        sleep(1);
    }

    print_status_snapshot();
    set_status(THREAD_MONITOR, STATE_STOPPED, "Monitor thread exiting");
    return NULL;
}

int main(void)
{
    pthread_t input_thread;
    pthread_t writer_thread;
    pthread_t reader_thread;
    pthread_t monitor_thread;

    signal(SIGINT, handle_sigint);

    init_status(THREAD_INPUT, "Input");
    init_status(THREAD_WRITER, "Writer");
    init_status(THREAD_READER, "Reader");
    init_status(THREAD_MONITOR, "Monitor");

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

    if (pthread_create(&monitor_thread, NULL, monitor_thread_fn, NULL) != 0) {
        perror("pthread_create monitor");
        keep_running = 0;
        pthread_cond_broadcast(&input_cond);
        pthread_join(input_thread, NULL);
        pthread_join(writer_thread, NULL);
        pthread_join(reader_thread, NULL);
        return 1;
    }

    pthread_join(input_thread, NULL);

    keep_running = 0;
    pthread_cond_broadcast(&input_cond);

    pthread_kill(reader_thread, SIGINT);
    pthread_kill(monitor_thread, SIGINT);
    pthread_kill(writer_thread, SIGINT);

    pthread_join(writer_thread, NULL);
    pthread_join(reader_thread, NULL);
    pthread_join(monitor_thread, NULL);

    printf("\nUser-space application exited.\n");
    return 0;
}
