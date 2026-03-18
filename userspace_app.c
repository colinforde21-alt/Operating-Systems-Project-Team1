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
#include <sys/select.h>

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
    THREAD_WRITER = 0,
    THREAD_READER,
    THREAD_MONITOR,
    THREAD_COUNT
};

static volatile sig_atomic_t keep_running = 1;

static pthread_mutex_t status_mutex = PTHREAD_MUTEX_INITIALIZER;
static thread_status_t statuses[THREAD_COUNT];

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
	printf("\nCtrl+c pressed. Exiting program...\n");
	fflush(stdout);
    	exit(0);
}

static void *reader_thread_fn(void *arg)
{
    (void)arg;
    int fd;
    char buf[MORSE_READ_SIZE];

    fd = open(DEVICE_PATH, O_RDONLY);
    if (fd < 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "open failed: %s", strerror(errno));
        set_status(THREAD_READER, STATE_ERROR, msg);
        return NULL;
    }

    while (keep_running) {
        fd_set set;
        struct timeval timeout;

        FD_ZERO(&set);
        FD_SET(fd, &set);

        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        set_status(THREAD_READER, STATE_BLOCKED, "Waiting for Morse input");

        int rv = select(fd + 1, &set, NULL, NULL, &timeout);

        if (!keep_running)
            break;

        if (rv < 0) {
            if (errno == EINTR)
                continue;

            char msg[128];
            snprintf(msg, sizeof(msg), "select failed: %s", strerror(errno));
            set_status(THREAD_READER, STATE_ERROR, msg);
            break;
        }

        if (rv == 0)
            continue;

        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n < 0) {
            if (errno == EINTR)
                continue;

            char msg[128];
            snprintf(msg, sizeof(msg), "read failed: %s", strerror(errno));
            set_status(THREAD_READER, STATE_ERROR, msg);
            break;
        }

        if (n > 0) {
            buf[n] = '\0';

            for (ssize_t i = 0; i < n; i++) {
                if (buf[i] == '\n' || buf[i] == '\r')
                    buf[i] = ' ';
            }

            char msg[128];
            snprintf(msg, sizeof(msg), "Received: %s", buf);
            set_status(THREAD_READER, STATE_RUNNING, msg);
        }
    }

    close(fd);
    set_status(THREAD_READER, STATE_STOPPED, "Reader exiting");
    return NULL;
}

static void *monitor_thread_fn(void *arg)
{
    (void)arg;
    set_status(THREAD_MONITOR, STATE_RUNNING, "Printing thread states");

    while (keep_running) {
        print_status_snapshot();
        sleep(2);
    }

    set_status(THREAD_MONITOR, STATE_STOPPED, "Monitor exiting");
    return NULL;
}

int main(void)
{
    pthread_t reader_thread, monitor_thread;
    int fd;
    char line[INPUT_BUF_SIZE];

    signal(SIGINT, handle_sigint);

    init_status(THREAD_WRITER, "Writer");
    init_status(THREAD_READER, "Reader");
    init_status(THREAD_MONITOR, "Monitor");

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

    while (keep_running) {
        printf("\nEnter text (or 'quit'): ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            if (feof(stdin))
                break;

            if (errno == EINTR) {
                clearerr(stdin);
                continue;
            }

            break;
        }

        line[strcspn(line, "\n")] = '\0';

        if (strcmp(line, "quit") == 0) {
            keep_running = 0;
	    close(fd);
		printf("\nExiting program...\n");
		fflush(stdout);
		exit(0);	
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

    keep_running = 0;
    close(fd);

    pthread_kill(reader_thread, SIGINT);
    pthread_kill(monitor_thread, SIGINT);

    pthread_join(reader_thread, NULL);
    pthread_join(monitor_thread, NULL);

    printf("\nApplication exited cleanly.\n");
    return 0;
}
