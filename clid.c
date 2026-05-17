#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#ifdef __ANDROID__
#  include <sys/system_properties.h>
#else
static inline int __system_property_set(const char *name, const char *value)
{
    (void)name; (void)value;
    fprintf(stderr, "[stub] setprop %s = %s\n", name, value);
    return 0;
}
#endif

#define BASE_PATH "/sdcard/Android/media/"
#define PID_FILE_NAME "daemon_pid"
#define COM_FILE_NAME "command"
#define RET_FILE_NAME "result"

#define PID_FILE BASE_PATH PID_FILE_NAME
#define COM_FILE BASE_PATH COM_FILE_NAME
#define RET_FILE BASE_PATH RET_FILE_NAME
#define BUFFER_SIZE 4096
#define ANDROID_SOCKET_NAMESPACE_ABSTRACT 0
#define NO_ERR 0
#define CREATE_ERR -1
#define CONNECT_ERR -2
#define LINUX_MAKE_ADDRUN_ERROR -3
#define NO_LINUX_MAKE_ADDRUN_ERROR -4

int socket_local_client_connect(int fd, const char *name, int namespaceId, int type);
int socket_make_sockaddr_un(const char *name, int namespaceId, struct sockaddr_un *p_addr, socklen_t *socklen);

int socket_local_client(const char *name, int namespaceId, int type)
{
    int socketID = socket(AF_LOCAL, type, 0);
    if (socketID < 0) return CREATE_ERR;
    int ret = socket_local_client_connect(socketID, name, namespaceId, type);
    if (ret < 0) { close(socketID); return ret; }
    return socketID;
}

int socket_local_client_connect(int fd, const char *name, int namespaceId, int type)
{
    struct sockaddr_un addr;
    socklen_t socklen;
    int ret = socket_make_sockaddr_un(name, namespaceId, &addr, &socklen);
    if (ret < 0) return ret;
    if (connect(fd, (struct sockaddr *)&addr, socklen) < 0) return CONNECT_ERR;
    return fd;
}

int socket_make_sockaddr_un(const char *name, int namespaceId, struct sockaddr_un *p_addr, socklen_t *socklen)
{
    memset(p_addr, 0, sizeof(*p_addr));
    size_t namelen = strlen(name);
    if ((namelen + 1) > sizeof(p_addr->sun_path)) return LINUX_MAKE_ADDRUN_ERROR;
    p_addr->sun_path[0] = 0;
    memcpy(p_addr->sun_path + 1, name, namelen);
    p_addr->sun_family = AF_LOCAL;
    *socklen = namelen + offsetof(struct sockaddr_un, sun_path) + 1;
    return NO_ERR;
}

void init_file()
{
    FILE *f;
    f = fopen(COM_FILE, "w");
    if (!f) { perror("create COM_FILE failed"); exit(EXIT_FAILURE); }
    fclose(f);
    f = fopen(RET_FILE, "w");
    if (!f) { perror("create RET_FILE failed"); exit(EXIT_FAILURE); }
    fclose(f);
}

int wait_for_file(int fd, int timeout_ms)
{
    struct stat st, last_st;
    last_st.st_size = 0;
    int waited_ms = 0;
    while (waited_ms < timeout_ms)
    {
        if (fstat(fd, &st) != 0) return -1;
        if (last_st.st_size)
        {
            if (last_st.st_size == st.st_size) return 1;
            else waited_ms = 0;
        }
        last_st.st_size = st.st_size;
        usleep(100000);
        waited_ms += 100;
    }
    return 0;
}

/*
 * check_daemon_exists: reads the PID file and probes the process.
 *
 * kill(pid, 0) semantics:
 *   0     → process exists, same UID (or root)        → running
 *   EPERM → process exists, different UID              → running (cross-UID case)
 *   ESRCH → no such process                            → not running
 *
 * Previously EPERM was treated as "not running", which caused the untrusted
 * app to spuriously spawn a second daemon that couldn't connect to cmd_skt.
 */
pid_t check_daemon_exists()
{
    pid_t pid;
    FILE *pid_file = fopen(PID_FILE, "r");
    if (!pid_file) return -1;
    if (fscanf(pid_file, "%d", &pid) != 1) { fclose(pid_file); return -1; }
    fclose(pid_file);
    if (pid == -1) return -1;

    if (kill(pid, 0) == 0 || errno == EPERM)
        return pid;   /* exists — either same-UID or cross-UID */
    return -1;        /* ESRCH: truly gone */
}

void start_daemon()
{
    pid_t pid = fork();
    if (pid < 0) { perror("fork failed"); exit(EXIT_FAILURE); }
    if (pid > 0) return;   /* parent returns immediately */

    if (setsid() < 0) { perror("[CHILD] setsid failed"); exit(EXIT_FAILURE); }
    if (chdir("/") < 0)  { perror("[CHILD] chdir failed");  exit(EXIT_FAILURE); }

    int client_fd = socket_local_client("cmd_skt", ANDROID_SOCKET_NAMESPACE_ABSTRACT, SOCK_STREAM);
    if (client_fd < 0) exit(EXIT_FAILURE);

    struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };
    if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
    {
        perror("[CHILD] setsockopt failed");
        exit(EXIT_FAILURE);
    }

    FILE *pid_file = fopen(PID_FILE, "w");
    if (!pid_file) { perror("[CHILD] create PID_FILE failed"); exit(EXIT_FAILURE); }
    fprintf(pid_file, "%d\n", getpid());
    fclose(pid_file);

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    char buffer[BUFFER_SIZE];
    while (1)
    {
        int com_fd = open(COM_FILE, O_RDONLY);
        if (com_fd < 0) exit(EXIT_FAILURE);

        int ret = wait_for_file(com_fd, 1000);
        if (ret < 0) exit(EXIT_FAILURE);
        if (ret == 0) { close(com_fd); sleep(1); continue; }

        ssize_t n = read(com_fd, buffer, BUFFER_SIZE);
        close(com_fd);
        com_fd = open(COM_FILE, O_WRONLY | O_TRUNC);
        close(com_fd);

        if (!strcmp(buffer, "kill-server\n"))
        {
            shutdown(client_fd, SHUT_WR);
            close(client_fd);
            unlink(PID_FILE);
            unlink(COM_FILE);
            unlink(RET_FILE);
            exit(EXIT_SUCCESS);
        }

        if (write(client_fd, buffer, n) < 0) exit(EXIT_FAILURE);

        int ret_fd = open(RET_FILE, O_WRONLY | O_TRUNC);
        if (ret_fd < 0) exit(EXIT_FAILURE);

        ssize_t recv_bytes;
        while ((recv_bytes = read(client_fd, buffer, BUFFER_SIZE)) > 0)
        {
            if (write(ret_fd, buffer, recv_bytes) != recv_bytes)
            {
                close(ret_fd);
                exit(EXIT_FAILURE);
            }
        }
        close(ret_fd);
    }
}

/*
 * build_command: assembles argv[start..argc-1] into buffer with spaces,
 * terminated by '\n'.  Returns bytes written, or -1 on overflow.
 */
static ssize_t build_command(char *buf, size_t bufsz, int argc, char *argv[], int start)
{
    size_t len = 0;
    for (int i = start; i < argc; i++)
    {
        size_t arglen = strlen(argv[i]);
        if (len + arglen + 2 > bufsz)   /* +2: space/newline + NUL guard */
        {
            fprintf(stderr, "Command too long.\n");
            return -1;
        }
        memcpy(buf + len, argv[i], arglen);
        len += arglen;
        buf[len++] = (i < argc - 1) ? ' ' : '\n';
    }
    buf[len] = '\0';
    return (ssize_t)len;
}

/*
 * run_as_client: pure file-IPC client path (-c flag).
 *
 * Does NOT check the daemon PID, does NOT fork anything.
 * Simply writes the command to COM_FILE and waits for RET_FILE.
 * Safe to call from any UID as long as the files are world-writable
 * (they live under /sdcard so that's guaranteed on most devices).
 */
static int run_as_client(int argc, char *argv[], int cmd_start)
{
    if (cmd_start >= argc)
    {
        fprintf(stderr, "Usage: clid -c <command> [args...]\n");
        return 1;
    }

    /* Verify the IPC files exist — if not, the server was never started */
    struct stat st;
    if (stat(COM_FILE, &st) != 0 || stat(RET_FILE, &st) != 0)
    {
        fprintf(stderr, "IPC files missing — has 'clid start-server' been run from engineermode?\n");
        return 1;
    }

    char buffer[BUFFER_SIZE];
    ssize_t cmd_len = build_command(buffer, sizeof(buffer), argc, argv, cmd_start);
    if (cmd_len < 0) return 1;

    /* Truncate result file before writing the command */
    int ret_fd = open(RET_FILE, O_WRONLY | O_TRUNC);
    if (ret_fd < 0) { perror("open RET_FILE failed"); return 1; }
    close(ret_fd);

    int com_fd = open(COM_FILE, O_WRONLY | O_TRUNC);
    if (com_fd < 0) { perror("open COM_FILE failed"); return 1; }
    if (write(com_fd, buffer, cmd_len) < 0) { perror("write COM_FILE failed"); close(com_fd); return 1; }
    close(com_fd);

    ret_fd = open(RET_FILE, O_RDONLY);
    if (ret_fd < 0) { perror("open RET_FILE failed"); return 1; }

    int ret = wait_for_file(ret_fd, 5000);
    if (ret < 1)
    {
        fprintf(stderr, ret == 0 ? "Timeout waiting for response.\n" : "fstat error.\n");
        close(ret_fd);
        return 1;
    }

    ssize_t bytes_read;
    while ((bytes_read = read(ret_fd, buffer, BUFFER_SIZE)) > 0)
        fwrite(buffer, 1, bytes_read, stdout);

    if (bytes_read < 0) { perror("read RET_FILE failed"); close(ret_fd); return 1; }
    fwrite("\n", 1, 1, stdout);
    close(ret_fd);
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage:\n"
                        "  clid start-server          (from engineermode)\n"
                        "  clid kill-server\n"
                        "  clid -c <command> [args]   (from untrusted app)\n"
                        "  clid <command> [args]       (same-UID path)\n");
        return 1;
    }

    /* ── Untrusted-app client path ──────────────────────────────────────── */
    if (strcmp(argv[1], "-c") == 0)
        return run_as_client(argc, argv, 2);

    /* ── Privileged paths (engineermode / same-UID) ─────────────────────── */
    if (strcmp(argv[1], "start-server") == 0)
    {
        if (check_daemon_exists() != -1)
        {
            printf("Daemon already running.\n");
            return 0;
        }
        printf("Starting new daemon...\n");
        if (__system_property_set("persist.sys.cmdservice.enable", "enable") != 0)
        {
            fprintf(stderr, "setprop failed — run from engineermode context\n");
            return EXIT_FAILURE;
        }
        usleep(500000);
        init_file();
        start_daemon();
        return 0;
    }

    /* ── Same-UID command path (kill-server or direct command) ──────────── */
    pid_t g_pid = check_daemon_exists();
    if (g_pid == -1)
    {
        printf("No daemon running — starting one...\n");
        init_file();
        start_daemon();
        printf("New daemon started.\n");
        g_pid = check_daemon_exists();
    }

    char buffer[BUFFER_SIZE];
    ssize_t cmd_len = build_command(buffer, sizeof(buffer), argc, argv, 1);
    if (cmd_len < 0) return 1;

    int ret_fd = open(RET_FILE, O_WRONLY | O_TRUNC);
    if (ret_fd < 0) { perror("open RET_FILE failed"); return 1; }
    close(ret_fd);

    int com_fd = open(COM_FILE, O_WRONLY | O_TRUNC);
    if (com_fd < 0) { perror("open COM_FILE failed"); return 1; }
    if (write(com_fd, buffer, cmd_len) < 0) perror("write COM_FILE failed");
    close(com_fd);

    if (strcmp(argv[1], "kill-server") == 0)
    {
        while (check_daemon_exists() != -1) { /* spin */ }
        printf("Daemon (PID %d) exited.\n", g_pid);
        return 0;
    }

    ret_fd = open(RET_FILE, O_RDONLY);
    if (ret_fd < 0) { perror("open RET_FILE failed"); return 1; }

    int ret = wait_for_file(ret_fd, 5000);
    if (ret < 1) { fprintf(stderr, "Timeout or error.\n"); close(ret_fd); return 1; }

    ssize_t bytes_read;
    while ((bytes_read = read(ret_fd, buffer, BUFFER_SIZE)) > 0)
        fwrite(buffer, 1, bytes_read, stdout);
    if (bytes_read < 0) { perror("read RET_FILE failed"); close(ret_fd); return 1; }
    fwrite("\n", 1, 1, stdout);
    close(ret_fd);
    return 0;
}
