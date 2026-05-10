#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>

/* On Android this comes from <sys/system_properties.h>.
   On Linux/x86 we stub it out so the code compiles cleanly. */
#ifdef __ANDROID__
#  include <sys/system_properties.h>
#else
static inline int __system_property_set(const char *name, const char *value)
{
    /* stub: on device this will be the real libc symbol */
    (void)name; (void)value;
    fprintf(stderr, "[stub] setprop %s = %s\n", name, value);
    return 0;
}
#endif

#define BASE_PATH "/sdcard/Download/"
#define RET_FILE_NAME "result"
#define RET_FILE BASE_PATH RET_FILE_NAME
#define BUFFER_SIZE 4096
#define ANDROID_SOCKET_NAMESPACE_ABSTRACT 0
#define NO_ERR 0
#define CREATE_ERR -1
#define CONNECT_ERR -2
#define LINUX_MAKE_ADDRUN_ERROR -3

/* Forward declarations */
int socket_local_client_connect(int fd, const char *name, int namespaceId, int type);
int socket_make_sockaddr_un(const char *name, int namespaceId, struct sockaddr_un *p_addr, socklen_t *socklen);

int socket_local_client(const char *name, int namespaceId, int type)
{
    int socketID = socket(AF_LOCAL, type, 0);
    if (socketID < 0)
        return CREATE_ERR;

    int ret = socket_local_client_connect(socketID, name, namespaceId, type);
    if (ret < 0) {
        close(socketID);
        return ret;
    }
    return socketID;
}

int socket_local_client_connect(int fd, const char *name, int namespaceId, int type)
{
    struct sockaddr_un addr;
    socklen_t socklen;

    int ret = socket_make_sockaddr_un(name, namespaceId, &addr, &socklen);
    if (ret < 0)
        return ret;

    if (connect(fd, (struct sockaddr *)&addr, socklen) < 0)
        return CONNECT_ERR;

    return fd;
}

int socket_make_sockaddr_un(const char *name, int namespaceId, struct sockaddr_un *p_addr, socklen_t *socklen)
{
    memset(p_addr, 0, sizeof(*p_addr));
    size_t namelen = strlen(name);

    if ((namelen + 1) > sizeof(p_addr->sun_path))
        return LINUX_MAKE_ADDRUN_ERROR;

    p_addr->sun_path[0] = 0;
    memcpy(p_addr->sun_path + 1, name, namelen);
    p_addr->sun_family = AF_LOCAL;
    *socklen = namelen + offsetof(struct sockaddr_un, sun_path) + 1;
    return NO_ERR;
}

int main(int argc, char *argv[])
{
    /* Enable the command service before anything else */
    if (__system_property_set("persist.sys.cmdservice.enable", "enable") != 0) {
        fprintf(stderr, "setprop failed — run this from com.sprd.engineermode context\n");
        return EXIT_FAILURE;
    }
    usleep(500000); /* give the service time to bring up the socket */

    int client_fd;
    char buffer[BUFFER_SIZE];

    client_fd = socket_local_client("cmd_skt", ANDROID_SOCKET_NAMESPACE_ABSTRACT, SOCK_STREAM);
    if (client_fd < 0) {
        perror("socket creation failed");
        return EXIT_FAILURE;
    }

    /* -c <command>: send once, print response, exit */
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        memset(buffer, 0, BUFFER_SIZE);
        for (int i = 2; i < argc; i++) {
            strncat(buffer, argv[i], BUFFER_SIZE - strlen(buffer) - 1);
            if (i < argc - 1)
                strncat(buffer, " ", BUFFER_SIZE - strlen(buffer) - 1);
        }

        if (write(client_fd, buffer, strlen(buffer)) < 0) {
            perror("write failed");
            close(client_fd);
            return EXIT_FAILURE;
        }

        memset(buffer, 0, BUFFER_SIZE);
        ssize_t n = read(client_fd, buffer, BUFFER_SIZE - 1);
        if (n < 0) {
            perror("read failed");
            close(client_fd);
            return EXIT_FAILURE;
        }
        buffer[n] = '\0';
        printf("%s\n", buffer);
        fflush(stdout);

        close(client_fd);
        return EXIT_SUCCESS;
    }

    /* Interactive mode */
    printf("Connected to the server.\n");
    fflush(stdout);

    while (1) {
        printf("Enter a command (type 'exit' to quit): ");
        fflush(stdout);
        memset(buffer, 0, BUFFER_SIZE);

        if (fgets(buffer, BUFFER_SIZE, stdin) == NULL) {
            printf("Error reading input.\n");
            fflush(stdout);
            break;
        }
        buffer[strcspn(buffer, "\n")] = 0;

        if (strcmp(buffer, "exit") == 0) {
            printf("Exiting.\n");
            fflush(stdout);
            break;
        }

        if (write(client_fd, buffer, strlen(buffer)) < 0) {
            perror("write failed");
            break;
        }

        memset(buffer, 0, BUFFER_SIZE);
        if (read(client_fd, buffer, BUFFER_SIZE - 1) < 0) {
            perror("read failed");
            break;
        }
        printf("Server response: %s\n", buffer);
        fflush(stdout);
    }

    if (close(client_fd) < 0) {
        perror("close client failed");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
