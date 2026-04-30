/* engine.c - Supervised Multi-Container Runtime (User Space) */

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
#include <sys/resource.h>
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
    void *child_stack;          /* allocated for clone(), freed after waitpid */
    pthread_t producer_tid;     /* reads container pipe → bounded buffer */
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

typedef struct {
    bounded_buffer_t *log_buffer;
    int read_fd;
    char container_id[CONTAINER_ID_LEN];
} producer_args_t;

static volatile sig_atomic_t g_sigchld_flag = 0;
static volatile sig_atomic_t g_stop_flag    = 0;

static void handle_sigchld(int sig) { (void)sig; g_sigchld_flag = 1; }
static void handle_stop(int sig)    { (void)sig; g_stop_flag    = 1; }

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

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
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

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
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
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
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
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

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

/* block until space available; return -1 on shutdown */
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

/* block until item available; drain remaining items on shutdown, then return -1 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    if (buffer->count == 0) {  /* shutdown + empty */
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

void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    char log_path[PATH_MAX];
    int fd;

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        snprintf(log_path, sizeof(log_path), "%s/%s.log",
                 LOG_DIR, item.container_id);
        fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            write(fd, item.data, item.length);
            close(fd);
        }
    }
    return NULL;
}

static void *producer_thread(void *arg)
{
    producer_args_t *pargs = (producer_args_t *)arg;
    log_item_t item;
    ssize_t n;

    while (1) {
        memset(&item, 0, sizeof(item));
        strncpy(item.container_id, pargs->container_id, CONTAINER_ID_LEN - 1);

        n = read(pargs->read_fd, item.data, LOG_CHUNK_SIZE);
        if (n <= 0)
            break;

        item.length = (size_t)n;
        if (bounded_buffer_push(pargs->log_buffer, &item) < 0)
            break;
    }

    close(pargs->read_fd);
    free(pargs);
    return NULL;
}

int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;
    char cmd_copy[CHILD_COMMAND_LEN];
    char *exec_args[64];
    char *token;
    int argc = 0;

    /* keep /proc mount inside this namespace */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0)
        perror("child: mount --make-rprivate");

    if (sethostname(cfg->id, strlen(cfg->id)) != 0)
        perror("child: sethostname");

    if (chroot(cfg->rootfs) != 0) {
        perror("child: chroot");
        return 1;
    }
    if (chdir("/") != 0) {
        perror("child: chdir /");
        return 1;
    }

    mkdir("/proc", 0755); /* may already exist */
    if (mount("proc", "/proc", "proc", 0, NULL) != 0)
        perror("child: mount /proc");

    if (cfg->log_write_fd >= 0) {
        dup2(cfg->log_write_fd, STDOUT_FILENO);
        dup2(cfg->log_write_fd, STDERR_FILENO);
        close(cfg->log_write_fd);
    }

    if (cfg->nice_value != 0)
        setpriority(PRIO_PROCESS, 0, cfg->nice_value);

    strncpy(cmd_copy, cfg->command, sizeof(cmd_copy) - 1);
    cmd_copy[sizeof(cmd_copy) - 1] = '\0';

    token = strtok(cmd_copy, " ");
    while (token && argc < 63) {
        exec_args[argc++] = token;
        token = strtok(NULL, " ");
    }
    exec_args[argc] = NULL;

    if (argc == 0) {
        fprintf(stderr, "child: empty command\n");
        return 1;
    }

    execvp(exec_args[0], exec_args);
    perror("child: execvp");
    return 1;
}

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
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

/* caller must hold metadata_lock */
static container_record_t *find_container(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *c = ctx->containers;
    while (c) {
        if (strncmp(c->id, id, CONTAINER_ID_LEN) == 0)
            return c;
        c = c->next;
    }
    return NULL;
}

static int do_spawn_container(supervisor_ctx_t *ctx,
                               const control_request_t *req,
                               int wait_for_exit)
{
    int pipefd[2];
    child_config_t *cfg;
    void *stack;
    pid_t pid;
    container_record_t *record;
    producer_args_t *pargs;
    int clone_flags;

    if (pipe(pipefd) < 0)
        return -1;

    cfg = malloc(sizeof(*cfg));
    if (!cfg)
        goto err_close_pipe;

    stack = malloc(STACK_SIZE);
    if (!stack)
        goto err_free_cfg;

    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->id,       req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs,   req->rootfs,       PATH_MAX - 1);
    strncpy(cfg->command,  req->command,      CHILD_COMMAND_LEN - 1);
    cfg->nice_value   = req->nice_value;
    cfg->log_write_fd = pipefd[1];

    clone_flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid = clone(child_fn, (char *)stack + STACK_SIZE, clone_flags, cfg);

    close(pipefd[1]); /* parent doesn't write */

    if (pid < 0) {
        perror("clone");
        free(stack);
        free(cfg);
        close(pipefd[0]);
        return -1;
    }

    free(cfg); /* child has its own CoW copy */

    record = malloc(sizeof(*record));
    if (!record) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        free(stack);
        close(pipefd[0]);
        return -1;
    }

    memset(record, 0, sizeof(*record));
    strncpy(record->id, req->container_id, CONTAINER_ID_LEN - 1);
    record->host_pid          = pid;
    record->started_at        = time(NULL);
    record->state             = CONTAINER_RUNNING;
    record->soft_limit_bytes  = req->soft_limit_bytes;
    record->hard_limit_bytes  = req->hard_limit_bytes;
    record->child_stack       = stack;
    snprintf(record->log_path, sizeof(record->log_path),
             "%s/%s.log", LOG_DIR, req->container_id);

    if (ctx->monitor_fd >= 0)
        register_with_monitor(ctx->monitor_fd, req->container_id, pid,
                              req->soft_limit_bytes, req->hard_limit_bytes);

    pargs = malloc(sizeof(*pargs));
    if (pargs) {
        pargs->log_buffer = &ctx->log_buffer;
        pargs->read_fd    = pipefd[0];
        strncpy(pargs->container_id, req->container_id, CONTAINER_ID_LEN - 1);
        if (pthread_create(&record->producer_tid, NULL, producer_thread, pargs) != 0) {
            free(pargs);
            close(pipefd[0]);
            record->producer_tid = 0;
        }
    } else {
        close(pipefd[0]);
    }

    pthread_mutex_lock(&ctx->metadata_lock);
    record->next   = ctx->containers;
    ctx->containers = record;
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (wait_for_exit) { /* CMD_RUN: synchronous */
        int wstatus;
        waitpid(pid, &wstatus, 0);

        pthread_mutex_lock(&ctx->metadata_lock);
        if (WIFEXITED(wstatus)) {
            record->state     = CONTAINER_EXITED;
            record->exit_code = WEXITSTATUS(wstatus);
        } else if (WIFSIGNALED(wstatus)) {
            record->state       = CONTAINER_KILLED;
            record->exit_signal = WTERMSIG(wstatus);
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (ctx->monitor_fd >= 0)
            unregister_from_monitor(ctx->monitor_fd, req->container_id, pid);

        if (record->producer_tid)
            pthread_join(record->producer_tid, NULL);

        free(record->child_stack);
        record->child_stack = NULL;
    }

    return 0;

err_free_cfg:
    free(cfg);
err_close_pipe:
    close(pipefd[0]);
    close(pipefd[1]);
    return -1;
}

static void handle_client(int client_fd, supervisor_ctx_t *ctx)
{
    control_request_t req;
    control_response_t resp;
    ssize_t n;

    n = read(client_fd, &req, sizeof(req));
    if (n != (ssize_t)sizeof(req))
        return;

    memset(&resp, 0, sizeof(resp));

    switch (req.kind) {

    case CMD_START:
    case CMD_RUN: {
        int wait = (req.kind == CMD_RUN);

        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *existing = find_container(ctx, req.container_id);
        int already_running = (existing && existing->state == CONTAINER_RUNNING);
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (already_running) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "container '%s' is already running", req.container_id);
        } else if (do_spawn_container(ctx, &req, wait) == 0) {
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message),
                     "container '%s' started", req.container_id);
        } else {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "failed to start '%s': %s", req.container_id, strerror(errno));
        }
        write(client_fd, &resp, sizeof(resp));
        break;
    }

    case CMD_STOP: {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = find_container(ctx, req.container_id);
        if (c && (c->state == CONTAINER_RUNNING || c->state == CONTAINER_STARTING)) {
            kill(c->host_pid, SIGTERM);
            c->state  = CONTAINER_STOPPED;
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message),
                     "sent SIGTERM to '%s' (pid %d)", c->id, c->host_pid);
        } else if (c) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "container '%s' not running (state: %s)",
                     req.container_id, state_to_string(c->state));
        } else {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "container '%s' not found", req.container_id);
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        write(client_fd, &resp, sizeof(resp));
        break;
    }

    case CMD_PS: {
        char line[256];
        int len;

        len = snprintf(line, sizeof(line), "%-16s %-8s %-10s %s\n",
                       "ID", "PID", "STATE", "STARTED");
        write(client_fd, line, len);

        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        while (c) {
            char started[32];
            struct tm *tm_info = localtime(&c->started_at);
            strftime(started, sizeof(started), "%Y-%m-%dT%H:%M:%S", tm_info);
            len = snprintf(line, sizeof(line), "%-16s %-8d %-10s %s\n",
                           c->id, c->host_pid, state_to_string(c->state), started);
            write(client_fd, line, len);
            c = c->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        break;
    }

    case CMD_LOGS: {
        char log_path[PATH_MAX];
        char buf[4096];
        ssize_t nr;

        snprintf(log_path, sizeof(log_path), "%s/%s.log",
                 LOG_DIR, req.container_id);
        int log_fd = open(log_path, O_RDONLY);
        if (log_fd < 0) {
            char msg[256];
            int len = snprintf(msg, sizeof(msg),
                               "no logs for container '%s'\n", req.container_id);
            write(client_fd, msg, len);
        } else {
            while ((nr = read(log_fd, buf, sizeof(buf))) > 0)
                write(client_fd, buf, (size_t)nr);
            close(log_fd);
        }
        break;
    }

    default:
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "unknown command %d", req.kind);
        write(client_fd, &resp, sizeof(resp));
        break;
    }
}

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    struct sockaddr_un addr;
    struct sigaction sa;
    int rc;

    (void)rootfs;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    mkdir(LOG_DIR, 0755);

    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr,
                "Warning: /dev/container_monitor unavailable (%s); "
                "memory monitoring disabled\n", strerror(errno));

    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        goto cleanup;
    }

    unlink(CONTROL_PATH);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        goto cleanup;
    }
    if (listen(ctx.server_fd, 8) < 0) {
        perror("listen");
        goto cleanup;
    }

    signal(SIGPIPE, SIG_IGN); /* ignore broken pipe on client disconnect */

    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);

    sa.sa_handler = handle_sigchld;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = handle_stop;
    sa.sa_flags   = 0;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) {
        errno = rc;
        perror("pthread_create logger_thread");
        goto cleanup;
    }

    fprintf(stderr, "[supervisor] ready on %s\n", CONTROL_PATH);

    while (!g_stop_flag) {
        fd_set rfds;
        struct timeval tv = {0, 100000}; /* 100 ms timeout */

        FD_ZERO(&rfds);
        FD_SET(ctx.server_fd, &rfds);

        int ret = select(ctx.server_fd + 1, &rfds, NULL, NULL, &tv);
        int select_errno = errno; /* save before waitpid/anything else overwrites it */

        if (g_sigchld_flag) {
            g_sigchld_flag = 0;
            int wstatus;
            pid_t pid;
            while ((pid = waitpid(-1, &wstatus, WNOHANG)) > 0) {
                pthread_mutex_lock(&ctx.metadata_lock);
                container_record_t *c = ctx.containers;
                while (c) {
                    if (c->host_pid == pid) {
                        if (WIFEXITED(wstatus)) {
                            c->state     = CONTAINER_EXITED;
                            c->exit_code = WEXITSTATUS(wstatus);
                        } else if (WIFSIGNALED(wstatus)) {
                            c->state       = CONTAINER_KILLED;
                            c->exit_signal = WTERMSIG(wstatus);
                        }
                        if (ctx.monitor_fd >= 0)
                            unregister_from_monitor(ctx.monitor_fd, c->id, pid);
                        free(c->child_stack);
                        c->child_stack = NULL;
                        break;
                    }
                    c = c->next;
                }
                pthread_mutex_unlock(&ctx.metadata_lock);
            }
        }

        if (ret < 0 && select_errno != EINTR)
            break;

        if (ret > 0 && FD_ISSET(ctx.server_fd, &rfds)) {
            int client_fd = accept(ctx.server_fd, NULL, NULL);
            if (client_fd >= 0) {
                handle_client(client_fd, &ctx);
                close(client_fd);
            }
        }
    }

    fprintf(stderr, "[supervisor] shutting down...\n");

    pthread_mutex_lock(&ctx.metadata_lock); /* SIGTERM all live containers */
    {
        container_record_t *c = ctx.containers;
        while (c) {
            if (c->state == CONTAINER_RUNNING || c->state == CONTAINER_STARTING)
                kill(c->host_pid, SIGTERM);
            c = c->next;
        }
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    sleep(2);

    pthread_mutex_lock(&ctx.metadata_lock); /* SIGKILL survivors */
    {
        container_record_t *c = ctx.containers;
        while (c) {
            if (c->state == CONTAINER_RUNNING || c->state == CONTAINER_STARTING ||
                c->state == CONTAINER_STOPPED) {
                kill(c->host_pid, SIGKILL);
            }
            c = c->next;
        }
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    {
        pid_t p;
        do { p = waitpid(-1, NULL, 0); } while (p > 0);
    }

    { /* join producer threads - they exit once their pipes close */
        container_record_t *c = ctx.containers;
        while (c) {
            if (c->producer_tid) {
                pthread_join(c->producer_tid, NULL);
                c->producer_tid = 0;
            }
            c = c->next;
        }
    }

    bounded_buffer_begin_shutdown(&ctx.log_buffer); /* drain remaining log chunks */
    pthread_join(ctx.logger_thread, NULL);

cleanup:
    if (ctx.server_fd >= 0)
        close(ctx.server_fd);
    unlink(CONTROL_PATH);
    if (ctx.monitor_fd >= 0)
        close(ctx.monitor_fd);

    pthread_mutex_lock(&ctx.metadata_lock); /* free container records */
    {
        container_record_t *c = ctx.containers;
        while (c) {
            container_record_t *next = c->next;
            free(c->child_stack);
            free(c);
            c = next;
        }
        ctx.containers = NULL;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    return 0;
}

static int send_control_request(const control_request_t *req)
{
    int sockfd;
    struct sockaddr_un addr;
    ssize_t n;
    int ret = 0;

    sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect (is the supervisor running?)");
        close(sockfd);
        return 1;
    }

    if (write(sockfd, req, sizeof(*req)) != (ssize_t)sizeof(*req)) {
        perror("write request");
        close(sockfd);
        return 1;
    }

    shutdown(sockfd, SHUT_WR);

    if (req->kind == CMD_PS || req->kind == CMD_LOGS) {
        char buf[4096];
        while ((n = read(sockfd, buf, sizeof(buf) - 1)) > 0) {
            buf[n] = '\0';
            printf("%s", buf);
        }
    } else {
        control_response_t resp;
        n = read(sockfd, &resp, sizeof(resp));
        if (n == (ssize_t)sizeof(resp)) {
            if (resp.status == 0)
                printf("%s\n", resp.message);
            else {
                fprintf(stderr, "error: %s\n", resp.message);
                ret = 1;
            }
        } else if (n < 0) {
            perror("read response");
            ret = 1;
        }
    }

    close(sockfd);
    return ret;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,       argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command,      argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,       argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command,      argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

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

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
