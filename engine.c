/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* Global supervisor context for signal handlers */
static supervisor_ctx_t *g_ctx = NULL;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag, const char *value, unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;
    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req, int argc, char *argv[], int start_index)
{
    int i;
    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;
        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr, "Invalid value for --nice (expected -20..19): %s\n", argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

/* ============================================================
 * Bounded Buffer Implementation
 * ============================================================ */

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;
    memset(buffer, 0, sizeof(*buffer));
    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0) return rc;
    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) { pthread_mutex_destroy(&buffer->mutex); return rc; }
    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/* ============================================================
 * Logging Thread
 * ============================================================ */

void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        /* Find log path for container */
        char log_path[PATH_MAX];
        snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, item.container_id);

        int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            write(fd, item.data, item.length);
            close(fd);
        }
    }

    /* Drain remaining items after shutdown */
    pthread_mutex_lock(&ctx->log_buffer.mutex);
    while (ctx->log_buffer.count > 0) {
        item = ctx->log_buffer.items[ctx->log_buffer.head];
        ctx->log_buffer.head = (ctx->log_buffer.head + 1) % LOG_BUFFER_CAPACITY;
        ctx->log_buffer.count--;
        pthread_mutex_unlock(&ctx->log_buffer.mutex);

        char log_path[PATH_MAX];
        snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, item.container_id);
        int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            write(fd, item.data, item.length);
            close(fd);
        }
        pthread_mutex_lock(&ctx->log_buffer.mutex);
    }
    pthread_mutex_unlock(&ctx->log_buffer.mutex);

    return NULL;
}

/* ============================================================
 * Log pipe reader thread per container
 * ============================================================ */

typedef struct {
    int read_fd;
    char container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *log_buffer;
} pipe_reader_args_t;

static void *pipe_reader_thread(void *arg)
{
    pipe_reader_args_t *args = (pipe_reader_args_t *)arg;
    log_item_t item;
    ssize_t n;

    memset(item.container_id, 0, sizeof(item.container_id));
    strncpy(item.container_id, args->container_id, CONTAINER_ID_LEN - 1);

    while ((n = read(args->read_fd, item.data, LOG_CHUNK_SIZE)) > 0) {
        item.length = (size_t)n;
        bounded_buffer_push(args->log_buffer, &item);
    }

    close(args->read_fd);
    free(args);
    return NULL;
}

/* ============================================================
 * Container Child Function
 * ============================================================ */

int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* Redirect stdout and stderr to log pipe */
    dup2(cfg->log_write_fd, STDOUT_FILENO);
    dup2(cfg->log_write_fd, STDERR_FILENO);
    close(cfg->log_write_fd);

    /* Set nice value */
    if (cfg->nice_value != 0)
        nice(cfg->nice_value);

    /* Set hostname to container ID */
    sethostname(cfg->id, strlen(cfg->id));

    /* Mount proc */
    char proc_path[PATH_MAX];
    snprintf(proc_path, sizeof(proc_path), "%s/proc", cfg->rootfs);
    mkdir(proc_path, 0755);

    /* chroot into rootfs */
    if (chroot(cfg->rootfs) != 0) {
        perror("chroot");
        return 1;
    }
    chdir("/");

    /* Mount /proc inside container */
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        /* Non-fatal, just warn */
        fprintf(stderr, "Warning: failed to mount /proc: %s\n", strerror(errno));
    }

    /* Execute the command */
    char *args[] = { "/bin/sh", "-c", cfg->command, NULL };
    execv("/bin/sh", args);

    /* If execv fails */
    perror("execv");
    return 1;
}

/* ============================================================
 * Monitor Registration
 * ============================================================ */

int register_with_monitor(int monitor_fd, const char *container_id, pid_t host_pid,
                          unsigned long soft_limit_bytes, unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;
    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;
    return 0;
}

/* ============================================================
 * Container Launch
 * ============================================================ */

static pid_t launch_container(supervisor_ctx_t *ctx, const control_request_t *req)
{
    /* Create log pipe */
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        perror("pipe");
        return -1;
    }

    /* Set up child config */
    child_config_t *cfg = malloc(sizeof(child_config_t));
    if (!cfg) { close(pipefd[0]); close(pipefd[1]); return -1; }
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->id, req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs, req->rootfs, PATH_MAX - 1);
    strncpy(cfg->command, req->command, CHILD_COMMAND_LEN - 1);
    cfg->nice_value = req->nice_value;
    cfg->log_write_fd = pipefd[1];

    /* Allocate stack for clone */
    char *stack = malloc(STACK_SIZE);
    if (!stack) { free(cfg); close(pipefd[0]); close(pipefd[1]); return -1; }
    char *stack_top = stack + STACK_SIZE;

    int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid_t pid = clone(child_fn, stack_top, flags, cfg);

    close(pipefd[1]); /* Parent closes write end */
    free(stack);

    if (pid < 0) {
        perror("clone");
        free(cfg);
        close(pipefd[0]);
        return -1;
    }

    /* Register with kernel monitor */
    if (ctx->monitor_fd >= 0)
        register_with_monitor(ctx->monitor_fd, req->container_id, pid,
                              req->soft_limit_bytes, req->hard_limit_bytes);

    /* Create container record */
    container_record_t *record = malloc(sizeof(container_record_t));
    if (record) {
        memset(record, 0, sizeof(*record));
        strncpy(record->id, req->container_id, CONTAINER_ID_LEN - 1);
        record->host_pid = pid;
        record->started_at = time(NULL);
        record->state = CONTAINER_RUNNING;
        record->soft_limit_bytes = req->soft_limit_bytes;
        record->hard_limit_bytes = req->hard_limit_bytes;
        snprintf(record->log_path, PATH_MAX, "%s/%s.log", LOG_DIR, req->container_id);

        pthread_mutex_lock(&ctx->metadata_lock);
        record->next = ctx->containers;
        ctx->containers = record;
        pthread_mutex_unlock(&ctx->metadata_lock);
    }

    /* Start pipe reader thread */
    pipe_reader_args_t *pargs = malloc(sizeof(pipe_reader_args_t));
    if (pargs) {
        pargs->read_fd = pipefd[0];
        strncpy(pargs->container_id, req->container_id, CONTAINER_ID_LEN - 1);
        pargs->log_buffer = &ctx->log_buffer;
        pthread_t t;
        pthread_create(&t, NULL, pipe_reader_thread, pargs);
        pthread_detach(t);
    } else {
        close(pipefd[0]);
    }

    free(cfg);
    return pid;
}

/* ============================================================
 * Signal Handlers
 * ============================================================ */

static void sigchld_handler(int sig)
{
    (void)sig;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (!g_ctx) continue;
        pthread_mutex_lock(&g_ctx->metadata_lock);
        container_record_t *c = g_ctx->containers;
        while (c) {
            if (c->host_pid == pid) {
                if (WIFEXITED(status)) {
                    c->state = CONTAINER_EXITED;
                    c->exit_code = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    c->state = CONTAINER_KILLED;
                    c->exit_signal = WTERMSIG(status);
                }
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&g_ctx->metadata_lock);
    }
}

static void sigterm_handler(int sig)
{
    (void)sig;
    if (g_ctx) g_ctx->should_stop = 1;
}

/* ============================================================
 * Supervisor Handle Requests
 * ============================================================ */

static void handle_ps(supervisor_ctx_t *ctx, int client_fd)
{
    control_response_t resp;
    char buf[4096];
    int offset = 0;

    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = ctx->containers;

    offset += snprintf(buf + offset, sizeof(buf) - offset,
                       "%-16s %-8s %-10s %-8s %-12s %-12s\n",
                       "ID", "PID", "STATE", "EXIT", "SOFT_MB", "HARD_MB");
    offset += snprintf(buf + offset, sizeof(buf) - offset,
                       "%-16s %-8s %-10s %-8s %-12s %-12s\n",
                       "----------------", "--------", "----------",
                       "--------", "------------", "------------");

    while (c && offset < (int)sizeof(buf) - 128) {
        offset += snprintf(buf + offset, sizeof(buf) - offset,
                           "%-16s %-8d %-10s %-8d %-12lu %-12lu\n",
                           c->id, c->host_pid, state_to_string(c->state),
                           c->exit_code,
                           c->soft_limit_bytes >> 20,
                           c->hard_limit_bytes >> 20);
        c = c->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    memset(&resp, 0, sizeof(resp));
    resp.status = 0;
    strncpy(resp.message, buf, sizeof(resp.message) - 1);
    write(client_fd, &resp, sizeof(resp));
}

static void handle_logs(supervisor_ctx_t *ctx, int client_fd, const char *container_id)
{
    control_response_t resp;
    char log_path[PATH_MAX];
    snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, container_id);

    int fd = open(log_path, O_RDONLY);
    memset(&resp, 0, sizeof(resp));
    if (fd < 0) {
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "Log not found: %s", log_path);
        write(client_fd, &resp, sizeof(resp));
        return;
    }

    resp.status = 0;
    snprintf(resp.message, sizeof(resp.message), "Log: %s", log_path);
    write(client_fd, &resp, sizeof(resp));

    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(client_fd, buf, n);
    close(fd);

    (void)ctx;
}

static void handle_stop(supervisor_ctx_t *ctx, int client_fd, const char *container_id)
{
    control_response_t resp;
    memset(&resp, 0, sizeof(resp));

    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = ctx->containers;
    while (c) {
        if (strcmp(c->id, container_id) == 0) {
            if (c->state == CONTAINER_RUNNING) {
                kill(c->host_pid, SIGTERM);
                c->state = CONTAINER_STOPPED;
                if (ctx->monitor_fd >= 0)
                    unregister_from_monitor(ctx->monitor_fd, c->id, c->host_pid);
                resp.status = 0;
                snprintf(resp.message, sizeof(resp.message), "Stopped container %s", container_id);
            } else {
                resp.status = -1;
                snprintf(resp.message, sizeof(resp.message), "Container %s not running", container_id);
            }
            break;
        }
        c = c->next;
    }
    if (!c) {
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "Container not found: %s", container_id);
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    write(client_fd, &resp, sizeof(resp));
}

/* ============================================================
 * Supervisor Main Loop
 * ============================================================ */

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) { errno = rc; perror("pthread_mutex_init"); return 1; }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) { errno = rc; perror("bounded_buffer_init"); pthread_mutex_destroy(&ctx.metadata_lock); return 1; }

    /* Create log directory */
    mkdir(LOG_DIR, 0755);

    /* Open kernel monitor device */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "Warning: could not open /dev/container_monitor: %s\n", strerror(errno));

    /* Create UNIX domain socket */
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); return 1; }

    unlink(CONTROL_PATH);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(ctx.server_fd); return 1;
    }
    if (listen(ctx.server_fd, 16) < 0) {
        perror("listen"); close(ctx.server_fd); return 1;
    }

    /* Install signal handlers */
    struct sigaction sa_chld, sa_term;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = sigchld_handler;
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa_term, NULL);
    sigaction(SIGINT, &sa_term, NULL);

    /* Start logger thread */
    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) { errno = rc; perror("pthread_create"); return 1; }

    fprintf(stderr, "Supervisor started. rootfs=%s socket=%s\n", rootfs, CONTROL_PATH);

    /* Event loop */
    fd_set readfds;
    struct timeval tv;

    while (!ctx.should_stop) {
        FD_ZERO(&readfds);
        FD_SET(ctx.server_fd, &readfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int sel = select(ctx.server_fd + 1, &readfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (sel == 0) continue;

        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            break;
        }

        control_request_t req;
        ssize_t n = read(client_fd, &req, sizeof(req));
        if (n != sizeof(req)) { close(client_fd); continue; }

        switch (req.kind) {
        case CMD_START: {
            pid_t pid = launch_container(&ctx, &req);
            control_response_t resp;
            memset(&resp, 0, sizeof(resp));
            if (pid > 0) {
                resp.status = 0;
                snprintf(resp.message, sizeof(resp.message), "Started container %s pid=%d", req.container_id, pid);
            } else {
                resp.status = -1;
                snprintf(resp.message, sizeof(resp.message), "Failed to start container %s", req.container_id);
            }
            write(client_fd, &resp, sizeof(resp));
            break;
        }
        case CMD_RUN: {
            pid_t pid = launch_container(&ctx, &req);
            control_response_t resp;
            memset(&resp, 0, sizeof(resp));
            if (pid > 0) {
                resp.status = 0;
                snprintf(resp.message, sizeof(resp.message), "Running container %s pid=%d (foreground)", req.container_id, pid);
                write(client_fd, &resp, sizeof(resp));
                /* Wait for this container to finish */
                int status;
                waitpid(pid, &status, 0);
            } else {
                resp.status = -1;
                snprintf(resp.message, sizeof(resp.message), "Failed to run container %s", req.container_id);
                write(client_fd, &resp, sizeof(resp));
            }
            break;
        }
        case CMD_PS:
            handle_ps(&ctx, client_fd);
            break;
        case CMD_LOGS:
            handle_logs(&ctx, client_fd, req.container_id);
            break;
        case CMD_STOP:
            handle_stop(&ctx, client_fd, req.container_id);
            break;
        default: {
            control_response_t resp;
            memset(&resp, 0, sizeof(resp));
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message), "Unknown command");
            write(client_fd, &resp, sizeof(resp));
            break;
        }
        }
        close(client_fd);
    }

    /* Cleanup */
    fprintf(stderr, "Supervisor shutting down...\n");

    /* Kill all running containers */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *c = ctx.containers;
    while (c) {
        if (c->state == CONTAINER_RUNNING) {
            kill(c->host_pid, SIGTERM);
            c->state = CONTAINER_STOPPED;
        }
        c = c->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Wait for children */
    while (waitpid(-1, NULL, WNOHANG) > 0);
    sleep(1);
    while (waitpid(-1, NULL, WNOHANG) > 0);

    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

    /* Free container records */
    pthread_mutex_lock(&ctx.metadata_lock);
    c = ctx.containers;
    while (c) {
        container_record_t *next = c->next;
        free(c);
        c = next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    close(ctx.server_fd);
    unlink(CONTROL_PATH);
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);

    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);

    fprintf(stderr, "Supervisor exited cleanly.\n");
    return 0;
}

/* ============================================================
 * Client-side control request
 * ============================================================ */

static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Could not connect to supervisor at %s: %s\n", CONTROL_PATH, strerror(errno));
        close(fd);
        return 1;
    }

    if (write(fd, req, sizeof(*req)) != sizeof(*req)) {
        perror("write");
        close(fd);
        return 1;
    }

    /* Read response */
    control_response_t resp;
    ssize_t n = read(fd, &resp, sizeof(resp));
    if (n == sizeof(resp)) {
        printf("%s\n", resp.message);
        /* For logs command, stream remaining data */
        if (req->kind == CMD_LOGS && resp.status == 0) {
            char buf[4096];
            ssize_t nr;
            while ((nr = read(fd, buf, sizeof(buf))) > 0)
                write(STDOUT_FILENO, buf, nr);
        }
    }

    close(fd);
    return (n == sizeof(resp)) ? resp.status : 1;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr, "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr, "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) { fprintf(stderr, "Usage: %s logs <id>\n", argv[0]); return 1; }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) { fprintf(stderr, "Usage: %s stop <id>\n", argv[0]); return 1; }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]); return 1; }
        return run_supervisor(argv[2]);
    }
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run") == 0)   return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps") == 0)    return cmd_ps();
    if (strcmp(argv[1], "logs") == 0)  return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop") == 0)  return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
