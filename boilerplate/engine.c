/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
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
    CMD_STOP,
    CMD_WAIT /* Added for CLI blocking IPC polling */
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
    int stop_requested; /* Added to track graceful kills */
    int wait_fd;        /* Socket fd for blocking run/wait client */
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

static volatile sig_atomic_t g_sigchld = 0;
static volatile sig_atomic_t g_sigterm = 0;
static volatile sig_atomic_t g_client_sigint = 0;

static void sigchld_handler(int sig) { (void)sig; g_sigchld = 1; }
static void sigterm_handler(int sig) { (void)sig; g_sigterm = 1; }
static void client_sig_handler(int sig) { (void)sig; g_client_sigint = 1; }

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

/*
 * TODO:
 * Implement producer-side insertion into the bounded buffer.
 *
 * Requirements:
 *   - block or fail according to your chosen policy when the buffer is full
 *   - wake consumers correctly
 *   - stop cleanly if shutdown begins
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }
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

/*
 * TODO:
 * Implement consumer-side removal from the bounded buffer.
 *
 * Requirements:
 *   - wait correctly while the buffer is empty
 *   - return a useful status when shutdown is in progress
 *   - avoid races with producers and shutdown
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == 0 && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }
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

/*
 * TODO:
 * Implement the logging consumer thread.
 *
 * Suggested responsibilities:
 *   - remove log chunks from the bounded buffer
 *   - route each chunk to the correct per-container log file
 *   - exit cleanly when shutdown begins and pending work is drained
 */
#define MAX_FD_CACHE 128
typedef struct {
    char id[CONTAINER_ID_LEN];
    int fd;
} log_cache_item_t;

void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    log_cache_item_t cache[MAX_FD_CACHE];
    int cache_count = 0;
    
    mkdir(LOG_DIR, 0755);
    
    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        int fd = -1;
        int cache_idx = -1;
        
        for (int i = 0; i < cache_count; i++) {
            if (strcmp(cache[i].id, item.container_id) == 0) {
                fd = cache[i].fd;
                cache_idx = i;
                break;
            }
        }

        if (item.length == 0) {
            /* EOF marker, close the file and remove from cache */
            if (cache_idx >= 0) {
                close(fd);
                cache[cache_idx] = cache[cache_count - 1];
                cache_count--;
            }
            continue;
        }

        if (fd < 0) {
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);
            fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd >= 0 && cache_count < MAX_FD_CACHE) {
                strcpy(cache[cache_count].id, item.container_id);
                cache[cache_count].fd = fd;
                cache_count++;
            }
        }

        if (fd >= 0) {
            if (write(fd, item.data, item.length) < 0) {
                /* ignore */
            }
        }
    }
    
    // Shutting down, close remaining FDs
    for (int i = 0; i < cache_count; i++) {
        close(cache[i].fd);
    }
    return NULL;
}

/*
 * TODO:
 * Implement the clone child entrypoint.
 *
 * Required outcomes:
 *   - isolated PID / UTS / mount context
 *   - chroot or pivot_root into rootfs
 *   - working /proc inside container
 *   - stdout / stderr redirected to the supervisor logging path
 *   - configured command executed inside the container
 */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;
    dup2(cfg->log_write_fd, STDOUT_FILENO);
    dup2(cfg->log_write_fd, STDERR_FILENO);
    close(cfg->log_write_fd);

    if (cfg->nice_value != 0) {
        if (nice(cfg->nice_value) < 0) {
            /* ignore */
        }
    }

    if (mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL) != 0) {
        fprintf(stderr, "[Setup Error] mount('MS_PRIVATE') failed: %s\n", strerror(errno));
        return 1;
    }

    if (chroot(cfg->rootfs) != 0) {
        fprintf(stderr, "[Setup Error] chroot('%s') failed: %s\n", cfg->rootfs, strerror(errno));
        return 1;
    }
    
    if (chdir("/") != 0) {
        fprintf(stderr, "[Setup Error] chdir('/') failed: %s\n", strerror(errno));
        return 1;
    }
    
    // Sometimes /proc needs to be created or namespaces need a private mount flag.
    // We attempt generic mount first.
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        fprintf(stderr, "[Setup Error] mount('proc') failed: %s\n", strerror(errno));
        return 1;
    }

    // We use /bin/busybox sh because Windows-shared folders break Linux symlinks (like /bin/sh)
    char *args[] = {"/bin/busybox", "sh", "-c", cfg->command, NULL};
    execv("/bin/busybox", args);
    
    fprintf(stderr, "[Setup Error] execv('/bin/busybox') failed: %s\n", strerror(errno));
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

typedef struct {
    int read_fd;
    bounded_buffer_t *buffer;
    char container_id[CONTAINER_ID_LEN];
} pipe_reader_args_t;

void *pipe_reader_thread(void *arg) {
    pipe_reader_args_t *args = (pipe_reader_args_t *)arg;
    log_item_t item;
    strncpy(item.container_id, args->container_id, CONTAINER_ID_LEN - 1);
    while (1) {
        ssize_t n = read(args->read_fd, item.data, LOG_CHUNK_SIZE);
        if (n <= 0) break;
        item.length = n;
        if (bounded_buffer_push(args->buffer, &item) < 0) break;
    }
    
    /* Push EOF marker to close the file descriptor */
    item.length = 0;
    bounded_buffer_push(args->buffer, &item);
    
    close(args->read_fd);
    free(args);
    return NULL;
}

static void reap_children(supervisor_ctx_t *ctx) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        container_record_t *rec = ctx->containers;
        while (rec) {
            if (rec->host_pid == pid && rec->state == CONTAINER_RUNNING) {
                if (WIFEXITED(status)) {
                    rec->exit_code = WEXITSTATUS(status);
                    if (rec->stop_requested) rec->state = CONTAINER_STOPPED;
                    else rec->state = CONTAINER_EXITED;
                } else if (WIFSIGNALED(status)) {
                    rec->exit_signal = WTERMSIG(status);
                    if (rec->stop_requested) rec->state = CONTAINER_STOPPED;
                    else if (rec->exit_signal == SIGKILL) rec->state = CONTAINER_KILLED;
                    else rec->state = CONTAINER_EXITED;
                }
                if (ctx->monitor_fd >= 0) unregister_from_monitor(ctx->monitor_fd, rec->id, pid);
                
                if (rec->wait_fd >= 0) {
                    control_response_t resp;
                    memset(&resp, 0, sizeof(resp));
                    resp.status = 0;
                    snprintf(resp.message, sizeof(resp.message), "Ended with code %d signal %d", rec->exit_code, rec->exit_signal);
                    send(rec->wait_fd, &resp, sizeof(resp), 0);
                    close(rec->wait_fd);
                    rec->wait_fd = -1;
                }
                break;
            }
            rec = rec->next;
        }
    }
}

static int handle_client(supervisor_ctx_t *ctx, int client_fd) {
    control_request_t req;
    control_response_t resp;
    memset(&resp, 0, sizeof(resp));

    if (recv(client_fd, &req, sizeof(req), 0) <= 0) return 0;

    pthread_mutex_lock(&ctx->metadata_lock);

    if (req.kind == CMD_START || req.kind == CMD_RUN) {
        container_record_t *rec = ctx->containers;
        int exists = 0;
        while (rec) {
            if (strcmp(rec->id, req.container_id) == 0 && rec->state == CONTAINER_RUNNING) exists = 1;
            rec = rec->next;
        }
        if (exists) {
            resp.status = 1;
            strcpy(resp.message, "Container already running");
        } else {
            int pipefd[2];
            if (pipe(pipefd) == 0) {
                child_config_t *cfg = malloc(sizeof(child_config_t));
                char *stack = malloc(STACK_SIZE);
                if (cfg && stack) {
                    strncpy(cfg->id, req.container_id, CONTAINER_ID_LEN - 1);
                    strncpy(cfg->rootfs, req.rootfs, PATH_MAX - 1);
                    strncpy(cfg->command, req.command, CHILD_COMMAND_LEN - 1);
                    cfg->nice_value = req.nice_value;
                    cfg->log_write_fd = pipefd[1];

                    pid_t pid = clone(child_fn, stack + STACK_SIZE, CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD, cfg);
                    close(pipefd[1]);

                    if (pid > 0) {
                        if (ctx->monitor_fd >= 0)
                            register_with_monitor(ctx->monitor_fd, req.container_id, pid, req.soft_limit_bytes, req.hard_limit_bytes);
                        
                        container_record_t *new_rec = malloc(sizeof(container_record_t));
                        memset(new_rec, 0, sizeof(*new_rec));
                        strncpy(new_rec->id, req.container_id, CONTAINER_ID_LEN - 1);
                        new_rec->host_pid = pid;
                        new_rec->started_at = time(NULL);
                        new_rec->state = CONTAINER_RUNNING;
                        new_rec->soft_limit_bytes = req.soft_limit_bytes;
                        new_rec->hard_limit_bytes = req.hard_limit_bytes;
                        snprintf(new_rec->log_path, sizeof(new_rec->log_path), "%s/%s.log", LOG_DIR, new_rec->id);
                        new_rec->wait_fd = -1;
                        new_rec->next = ctx->containers;
                        ctx->containers = new_rec;

                        pipe_reader_args_t *pargs = malloc(sizeof(pipe_reader_args_t));
                        pargs->read_fd = pipefd[0];
                        pargs->buffer = &ctx->log_buffer;
                        strncpy(pargs->container_id, req.container_id, CONTAINER_ID_LEN - 1);
                        pthread_t th;
                        pthread_create(&th, NULL, pipe_reader_thread, pargs);
                        pthread_detach(th);

                        resp.status = 0;
                        strcpy(resp.message, "Started successfully");
                    } else {
                        free(stack);
                        free(cfg);
                        close(pipefd[0]);
                        resp.status = 1;
                        strcpy(resp.message, "Clone failed");
                    }
                } else {
                    if (stack) free(stack);
                    if (cfg) free(cfg);
                    resp.status = 1;
                    strcpy(resp.message, "Memory allocation failed");
                }
            }
        }
    } else if (req.kind == CMD_PS) {
        container_record_t *rec = ctx->containers;
        char buffer[CONTROL_MESSAGE_LEN] = "ID | PID | STATE | LIMITS (SOFT/HARD)\n";
        while(rec) {
            char line[128];
            snprintf(line, sizeof(line), "%s | %d | %s | %lu/%lu\n",
                rec->id, rec->host_pid, state_to_string(rec->state), rec->soft_limit_bytes, rec->hard_limit_bytes);
            if (strlen(buffer) + strlen(line) < sizeof(buffer)) strcat(buffer, line);
            rec = rec->next;
        }
        resp.status = 0;
        strncpy(resp.message, buffer, sizeof(resp.message) - 1);
    } else if (req.kind == CMD_STOP) {
        container_record_t *rec = ctx->containers;
        int found = 0;
        while(rec) {
            if (strcmp(rec->id, req.container_id) == 0 && rec->state == CONTAINER_RUNNING) {
                rec->stop_requested = 1;
                kill(rec->host_pid, SIGTERM);
                found = 1;
                break;
            }
            rec = rec->next;
        }
        resp.status = !found;
        strcpy(resp.message, found ? "Stop signal sent" : "Container not found/running");
    } else if (req.kind == CMD_LOGS) {
        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message), "Logs are inside %s/%s.log", LOG_DIR, req.container_id);
    } else if (req.kind == CMD_WAIT) {
        container_record_t *rec = ctx->containers;
        int found = 0;
        while(rec) {
            if (strcmp(rec->id, req.container_id) == 0) {
                found = 1;
                if (rec->state == CONTAINER_RUNNING) {
                    if (rec->wait_fd >= 0) {
                        resp.status = 1;
                        strcpy(resp.message, "Another client is already waiting on this container");
                        break;
                    }
                    rec->wait_fd = client_fd;
                    pthread_mutex_unlock(&ctx->metadata_lock);
                    return 1; /* Return 1 to tell supervisor NOT to close client_fd */
                } else {
                    resp.status = 0;
                    snprintf(resp.message, sizeof(resp.message), "Ended with code %d signal %d", rec->exit_code, rec->exit_signal);
                }
                break;
            }
            rec = rec->next;
        }
        if (!found) {
            resp.status = 0;
            strcpy(resp.message, "Ended dynamically or missing");
        }
    }

    pthread_mutex_unlock(&ctx->metadata_lock);
    send(client_fd, &resp, sizeof(resp), 0);
    return 0;
}


/*
 * TODO:
 * Implement the long-running supervisor process.
 *
 * Suggested responsibilities:
 *   - create and bind the control-plane IPC endpoint
 *   - initialize shared metadata and the bounded buffer
 *   - start the logging thread
 *   - accept control requests and update container state
 *   - reap children and respond to signals
 */
static int run_supervisor(const char *rootfs)
{
    (void)rootfs;
    supervisor_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0) perror("Warning: /dev/container_monitor missing");

    if (pthread_mutex_init(&ctx.metadata_lock, NULL) != 0) return 1;
    if (bounded_buffer_init(&ctx.log_buffer) != 0) return 1;

    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    unlink(CONTROL_PATH);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(ctx.server_fd, 10);

    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = sigterm_handler;
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);

    printf("Supervisor listening on %s (PID: %d)\n", CONTROL_PATH, getpid());

    while (!g_sigterm) {
        if (g_sigchld) {
            g_sigchld = 0;
            pthread_mutex_lock(&ctx.metadata_lock);
            reap_children(&ctx);
            pthread_mutex_unlock(&ctx.metadata_lock);
        }

        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (handle_client(&ctx, client_fd) == 0) {
            close(client_fd);
        }
    }

    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    bounded_buffer_destroy(&ctx.log_buffer);
    
    container_record_t *rec = ctx.containers;
    while(rec) {
        if(rec->state == CONTAINER_RUNNING) kill(rec->host_pid, SIGKILL);
        container_record_t *tmp = rec;
        rec = rec->next;
        free(tmp);
    }
    
    pthread_mutex_destroy(&ctx.metadata_lock);
    unlink(CONTROL_PATH);
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    printf("Supervisor gracefully terminated.\n");
    return 0;
}

/*
 * TODO:
 * Implement the client-side control request path.
 *
 * The CLI commands should use a second IPC mechanism distinct from the
 * logging pipe. A UNIX domain socket is the most direct option, but a
 * FIFO or shared memory design is also acceptable if justified.
 */
static int send_control_request(const control_request_t *req, control_response_t *resp)
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(sock);
        return -1;
    }
    
    if (send(sock, req, sizeof(*req), 0) <= 0) { close(sock); return -1; }
    if (recv(sock, resp, sizeof(*resp), 0) <= 0) { close(sock); return -1; }
    
    close(sock);
    return 0;
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
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    control_response_t resp;
    if (send_control_request(&req, &resp) != 0) {
        fprintf(stderr, "Failed to connect. Is supervisor running?\n");
        return 1;
    }
    
    printf("%s\n", resp.message);
    return resp.status;
}

static int cmd_run(int argc, char *argv[])
{
    if (cmd_start(argc, argv) != 0) return 1;

    struct sigaction sa;
    sa.sa_handler = client_sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int wait_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (wait_sock < 0) return 1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    
    if (connect(wait_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(wait_sock);
        fprintf(stderr, "Wait failed: cannot connect to supervisor.\n");
        return 1;
    }

    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_WAIT;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    if (send(wait_sock, &req, sizeof(req), 0) <= 0) {
        close(wait_sock);
        return 1;
    }

    control_response_t resp;
    while (1) {
        if (g_client_sigint) {
            g_client_sigint = 0;
            printf("\nForwarding termination directly to container...\n");
            
            control_request_t stop_req;
            control_response_t stop_resp;
            memset(&stop_req, 0, sizeof(stop_req));
            stop_req.kind = CMD_STOP;
            strncpy(stop_req.container_id, argv[2], sizeof(stop_req.container_id) - 1);
            if (send_control_request(&stop_req, &stop_resp) != 0) {
                fprintf(stderr, "[Error] Failed to forward stop request.\n");
            }
        }

        int n = recv(wait_sock, &resp, sizeof(resp), 0);
        if (n > 0) {
            if (resp.status == 1) {
                printf("%s\n", resp.message);
                close(wait_sock);
                return 1;
            }
            printf("%s\n", resp.message);
            close(wait_sock);
            return 0;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            fprintf(stderr, "Disconnected from supervisor.\n");
            break;
        }
    }
    close(wait_sock);
    return 1;
}

static int cmd_ps(void)
{
    control_request_t req;
    control_response_t resp;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;

    if (send_control_request(&req, &resp) == 0) {
        printf("%s", resp.message);
        return 0;
    }
    return 1;
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;
    control_response_t resp;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    if (send_control_request(&req, &resp) == 0) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, argv[2]);
        FILE *f = fopen(path, "r");
        if (f) {
            char buf[1024];
            while (fgets(buf, sizeof(buf), f)) {
                fputs(buf, stdout);
            }
            fclose(f);
        } else {
            fprintf(stderr, "Log file for %s doesn't exist yet.\n", argv[2]);
        }
        return 0;
    }
    return 1;
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;
    control_response_t resp;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    if (send_control_request(&req, &resp) == 0) {
        printf("%s\n", resp.message);
        return 0;
    }
    return 1;
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
