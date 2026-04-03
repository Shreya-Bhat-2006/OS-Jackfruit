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
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 4096
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
    int stop_requested;
    void *stack_ptr;
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
    int pipe_read_fd;
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
    int pipe_fd;
    char container_id[CONTAINER_ID_LEN];
    supervisor_ctx_t *ctx;
} pipe_reader_args_t;

static supervisor_ctx_t g_ctx;
static int g_signal_received = 0;

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
    case CONTAINER_RUNNING: return "running";
    case CONTAINER_STOPPED: return "stopped";
    case CONTAINER_KILLED: return "killed";
    case CONTAINER_EXITED: return "exited";
    default: return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));
    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0) return rc;
    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) { pthread_mutex_destroy(&buffer->mutex); return rc; }
    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) { pthread_cond_destroy(&buffer->not_empty); pthread_mutex_destroy(&buffer->mutex); return rc; }
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

static int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
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

static int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
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

void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    FILE *log_file = NULL;
    char log_path[PATH_MAX];
    
    while (1) {
        if (bounded_buffer_pop(&ctx->log_buffer, &item) != 0) {
            break;
        }
        
        snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, item.container_id);
        log_file = fopen(log_path, "a");
        if (log_file) {
            fwrite(item.data, 1, item.length, log_file);
            fclose(log_file);
        }
    }
    
    return NULL;
}

void *pipe_reader_thread(void *arg)
{
    pipe_reader_args_t *args = (pipe_reader_args_t *)arg;
    char buffer[LOG_CHUNK_SIZE];
    ssize_t bytes;
    
    while ((bytes = read(args->pipe_fd, buffer, LOG_CHUNK_SIZE)) > 0) {
        log_item_t item;
        strncpy(item.container_id, args->container_id, sizeof(item.container_id) - 1);
        item.length = bytes;
        memcpy(item.data, buffer, bytes);
        bounded_buffer_push(&args->ctx->log_buffer, &item);
    }
    
    close(args->pipe_fd);
    free(args);
    return NULL;
}

static int child_fn(void *arg)
{
    child_config_t *config = (child_config_t *)arg;
    
    if (setpriority(PRIO_PROCESS, 0, config->nice_value) != 0) {
        perror("setpriority");
    }
    
    if (chroot(config->rootfs) != 0) {
        perror("chroot");
        exit(1);
    }
    
    if (chdir("/") != 0) {
        perror("chdir");
        exit(1);
    }
    
    // Create /proc directory if it doesn't exist
    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("mount proc");
        exit(1);
    }
    
    // Fix: Use log_write_fd for stdout/stderr (write end of pipe)
    if (config->log_write_fd != -1) {
        dup2(config->log_write_fd, STDOUT_FILENO);
        dup2(config->log_write_fd, STDERR_FILENO);
        close(config->log_write_fd);
    }
    
    execlp("/bin/sh", "sh", "-c", config->command, NULL);
    perror("execlp");
    exit(1);
}

static int register_with_monitor(int monitor_fd, const char *container_id, pid_t host_pid,
                                  unsigned long soft_limit_bytes, unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    
    if (monitor_fd < 0) return 0;
    
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    
    return ioctl(monitor_fd, MONITOR_REGISTER, &req);
}

static int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;
    
    if (monitor_fd < 0) return 0;
    
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    
    return ioctl(monitor_fd, MONITOR_UNREGISTER, &req);
}

static void add_container_record(supervisor_ctx_t *ctx, const char *id, pid_t pid,
                                  unsigned long soft, unsigned long hard, const char *rootfs,
                                  void *stack_ptr)
{
    container_record_t *record = malloc(sizeof(container_record_t));
    if (!record) return;
    
    (void)rootfs;
    
    strncpy(record->id, id, CONTAINER_ID_LEN - 1);
    record->host_pid = pid;
    record->started_at = time(NULL);
    record->state = CONTAINER_RUNNING;
    record->soft_limit_bytes = soft;
    record->hard_limit_bytes = hard;
    record->exit_code = 0;
    record->exit_signal = 0;
    record->stop_requested = 0;
    record->stack_ptr = stack_ptr;
    snprintf(record->log_path, PATH_MAX, "%s/%s.log", LOG_DIR, id);
    record->next = NULL;
    
    pthread_mutex_lock(&ctx->metadata_lock);
    record->next = ctx->containers;
    ctx->containers = record;
    pthread_mutex_unlock(&ctx->metadata_lock);
}

static container_record_t *find_container(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *curr;
    
    pthread_mutex_lock(&ctx->metadata_lock);
    curr = ctx->containers;
    while (curr) {
        if (strcmp(curr->id, id) == 0) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            return curr;
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
    return NULL;
}

static void update_container_state(supervisor_ctx_t *ctx, pid_t pid, int exit_code, int exit_signal)
{
    container_record_t *curr;
    
    pthread_mutex_lock(&ctx->metadata_lock);
    curr = ctx->containers;
    while (curr) {
        if (curr->host_pid == pid) {
            if (curr->stop_requested && exit_signal == SIGKILL) {
                curr->state = CONTAINER_STOPPED;
            } else if (exit_signal == SIGKILL && !curr->stop_requested) {
                curr->state = CONTAINER_KILLED;
            } else {
                curr->state = CONTAINER_EXITED;
            }
            curr->exit_code = exit_code;
            curr->exit_signal = exit_signal;

            if (curr->stack_ptr) {
                free(curr->stack_ptr);
                curr->stack_ptr = NULL;
            }
            break;
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
}

static void reap_children(supervisor_ctx_t *ctx)
{
    int status;
    pid_t pid;
    
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        int exit_code = 0;
        int exit_signal = 0;
        
        if (WIFEXITED(status)) {
            exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            exit_signal = WTERMSIG(status);
        }
        
        update_container_state(ctx, pid, exit_code, exit_signal);
        unregister_from_monitor(ctx->monitor_fd, "", pid);
    }
}

static void signal_handler(int sig)
{
    g_signal_received = sig;

    if (sig == SIGINT || sig == SIGTERM) {
        if (g_ctx.should_stop == 0) {
            g_ctx.should_stop = 1;
            bounded_buffer_begin_shutdown(&g_ctx.log_buffer);
        }
    } else if (sig == SIGCHLD) {
        // Just notify main loop to reap children; do not stop supervisor.
    }
}

static int run_supervisor(const char *base_rootfs)
{
    struct sockaddr_un addr;
    int rc;
    
    (void)base_rootfs;
    
    mkdir(LOG_DIR, 0755);
    
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.server_fd = -1;
    g_ctx.monitor_fd = -1;
    g_ctx.should_stop = 0;
    g_ctx.containers = NULL;
    
    pthread_mutex_init(&g_ctx.metadata_lock, NULL);
    bounded_buffer_init(&g_ctx.log_buffer);
    
    g_ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (g_ctx.monitor_fd < 0) {
        fprintf(stderr, "Warning: Could not open /dev/container_monitor\n");
    }
    
    unlink(CONTROL_PATH);
    g_ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_ctx.server_fd < 0) {
        perror("socket");
        return 1;
    }
    
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    
    if (bind(g_ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    
    if (listen(g_ctx.server_fd, 5) < 0) {
        perror("listen");
        return 1;
    }
    
    pthread_create(&g_ctx.logger_thread, NULL, logging_thread, &g_ctx);
    
    signal(SIGCHLD, signal_handler);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    printf("Supervisor started with base rootfs: %s\n", base_rootfs);
    
    fd_set read_fds;
    int max_fd = g_ctx.server_fd;
    
    while (!g_ctx.should_stop) {
        FD_ZERO(&read_fds);
        FD_SET(g_ctx.server_fd, &read_fds);
        
        struct timeval tv = {1, 0};
        rc = select(max_fd + 1, &read_fds, NULL, NULL, &tv);
        
        reap_children(&g_ctx);
        
        if (rc > 0 && FD_ISSET(g_ctx.server_fd, &read_fds)) {
            int client_fd = accept(g_ctx.server_fd, NULL, NULL);
            if (client_fd >= 0) {
                control_request_t req;
                control_response_t resp = {0};
                
                if (read(client_fd, &req, sizeof(req)) == sizeof(req)) {
                    if (req.kind == CMD_START || req.kind == CMD_RUN) {
                        int pipe_fds[2];
                        void *stack = malloc(STACK_SIZE);
                        child_config_t config;
                        pid_t pid;
                        int wait_status = 0;
                        int is_run = (req.kind == CMD_RUN);

                        if (!stack) {
                            resp.status = -1;
                            snprintf(resp.message, sizeof(resp.message), "stack allocation failed");
                        } else if (find_container(&g_ctx, req.container_id) != NULL) {
                            resp.status = -1;
                            snprintf(resp.message, sizeof(resp.message), "container id already exists");
                            free(stack);
                        } else if (pipe(pipe_fds) != 0) {
                            resp.status = -1;
                            snprintf(resp.message, sizeof(resp.message), "pipe failed");
                            free(stack);
                        } else {
                            memset(&config, 0, sizeof(config));
                            strncpy(config.id, req.container_id, sizeof(config.id) - 1);
                            strncpy(config.rootfs, req.rootfs, sizeof(config.rootfs) - 1);
                            strncpy(config.command, req.command, sizeof(config.command) - 1);
                            config.nice_value = req.nice_value;
                            config.pipe_read_fd = pipe_fds[0];
                            config.log_write_fd = pipe_fds[1];

                            pid = clone(child_fn, stack + STACK_SIZE,
                                        CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD,
                                        &config);

                            if (pid < 0) {
                                resp.status = -1;
                                snprintf(resp.message, sizeof(resp.message), "clone failed: %s", strerror(errno));
                                close(pipe_fds[0]);
                                close(pipe_fds[1]);
                                free(stack);
                            } else {
                                add_container_record(&g_ctx, req.container_id, pid,
                                                   req.soft_limit_bytes, req.hard_limit_bytes,
                                                   req.rootfs, stack);
                                register_with_monitor(g_ctx.monitor_fd, req.container_id, pid,
                                                    req.soft_limit_bytes, req.hard_limit_bytes);

                                close(pipe_fds[1]);

                                pthread_t pipe_thread;
                                pipe_reader_args_t *args = malloc(sizeof(pipe_reader_args_t));
                                if (args) {
                                    args->pipe_fd = pipe_fds[0];
                                    strncpy(args->container_id, req.container_id, sizeof(args->container_id) - 1);
                                    args->ctx = &g_ctx;
                                    pthread_create(&pipe_thread, NULL, pipe_reader_thread, args);
                                    pthread_detach(pipe_thread);
                                } else {
                                    close(pipe_fds[0]);
                                }

                                if (is_run) {
                                    if (waitpid(pid, &wait_status, 0) > 0) {
                                        int exit_code = 0;
                                        int exit_signal = 0;

                                        if (WIFEXITED(wait_status)) {
                                            exit_code = WEXITSTATUS(wait_status);
                                            resp.status = exit_code;
                                        } else if (WIFSIGNALED(wait_status)) {
                                            exit_signal = WTERMSIG(wait_status);
                                            resp.status = 128 + exit_signal;
                                        } else {
                                            resp.status = -1;
                                        }

                                        update_container_state(&g_ctx, pid, exit_code, exit_signal);

                                        snprintf(resp.message, sizeof(resp.message),
                                                "container %s ended status=%d signal=%d",
                                                req.container_id, exit_code, exit_signal);
                                    } else {
                                        resp.status = -1;
                                        snprintf(resp.message, sizeof(resp.message), "waitpid failed: %s", strerror(errno));
                                    }
                                } else {
                                    resp.status = 0;
                                    snprintf(resp.message, sizeof(resp.message), "started with PID %d", pid);
                                }
                            }
                        }
                    } else if (req.kind == CMD_STOP) {
                        container_record_t *record = find_container(&g_ctx, req.container_id);
                        if (record) {
                            record->stop_requested = 1;
                            kill(record->host_pid, SIGTERM);
                            resp.status = 0;
                            snprintf(resp.message, sizeof(resp.message), "stop signal sent");
                        } else {
                            resp.status = -1;
                            snprintf(resp.message, sizeof(resp.message), "container not found");
                        }
                    } else if (req.kind == CMD_PS) {
                        container_record_t *curr;
                        resp.status = 0;
                        snprintf(resp.message, sizeof(resp.message), "Container list:\n");
                        
                        pthread_mutex_lock(&g_ctx.metadata_lock);
                        curr = g_ctx.containers;
                        while (curr) {
                            char line[128];
                            snprintf(line, sizeof(line), "  %s - PID:%d State:%s\n",
                                   curr->id, curr->host_pid, state_to_string(curr->state));
                            strncat(resp.message, line, sizeof(resp.message) - strlen(resp.message) - 1);
                            curr = curr->next;
                        }
                        pthread_mutex_unlock(&g_ctx.metadata_lock);
                    } else if (req.kind == CMD_LOGS) {
                        char log_path[PATH_MAX];
                        snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, req.container_id);
                        FILE *log_file = fopen(log_path, "r");
                        if (log_file) {
                            resp.status = 0;
                            // Read entire file into message buffer
                            size_t bytes_read = fread(resp.message, 1, sizeof(resp.message) - 1, log_file);
                            resp.message[bytes_read] = '\0';
                            fclose(log_file);
                        } else {
                            resp.status = -1;
                            snprintf(resp.message, sizeof(resp.message), "No logs found");
                        }
                    }
                }
                
                ssize_t write_result = write(client_fd, &resp, sizeof(resp));
                (void)write_result;
                close(client_fd);
            }
        }
    }
    
    bounded_buffer_begin_shutdown(&g_ctx.log_buffer);
    pthread_join(g_ctx.logger_thread, NULL);
    
    close(g_ctx.server_fd);
    unlink(CONTROL_PATH);
    if (g_ctx.monitor_fd >= 0) close(g_ctx.monitor_fd);
    
    bounded_buffer_destroy(&g_ctx.log_buffer);
    pthread_mutex_destroy(&g_ctx.metadata_lock);
    
    return 0;
}

static int send_control_request(const control_request_t *req)
{
    int sock;
    struct sockaddr_un addr;
    control_response_t resp;
    
    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }
    
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect (is supervisor running?)");
        close(sock);
        return 1;
    }
    
    if (write(sock, req, sizeof(*req)) != sizeof(*req)) {
        perror("write");
        close(sock);
        return 1;
    }
    
    if (read(sock, &resp, sizeof(resp)) != sizeof(resp)) {
        perror("read");
        close(sock);
        return 1;
    }
    
    printf("%s\n", resp.message);
    close(sock);
    return resp.status;
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
    req.nice_value = 0;
    
    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;
    
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
    req.nice_value = 0;
    
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
    
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run") == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps") == 0) return cmd_ps();
    if (strcmp(argv[1], "logs") == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop") == 0) return cmd_stop(argc, argv);
    
    usage(argv[0]);
    return 1;
}