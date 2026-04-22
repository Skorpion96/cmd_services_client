#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#define BASE_PATH "/sdcard/Android/media/.clid"
#define RET_FILE_NAME "result"

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
    int socketID;
    int ret;
    socketID = socket(AF_LOCAL, type, 0);
    if (socketID < 0)
    {
        return CREATE_ERR;
    }
    ret = socket_local_client_connect(socketID, name, namespaceId, type);
    if (ret < 0)
    {
        close(socketID);
        return ret;
    }
    return socketID;
}

int socket_local_client_connect(int fd, const char *name, int namespaceId, int type)
{
    struct sockaddr_un addr;
    socklen_t socklen;
    size_t namelen;
    int ret;
    ret = socket_make_sockaddr_un(name, namespaceId, &addr, &socklen);
    if (ret < 0)
    {
        return ret;
    }
    if (connect(fd, (struct sockaddr *)&addr, socklen) < 0)
    {
        return CONNECT_ERR;
    }
    return fd;
}

int socket_make_sockaddr_un(const char *name, int namespaceId, struct sockaddr_un *p_addr, socklen_t *socklen)
{
    size_t namelen;
    memset(p_addr, 0, sizeof(*p_addr));
    namelen = strlen(name);
    if ((namelen + 1) > sizeof(p_addr->sun_path))
    {
        return LINUX_MAKE_ADDRUN_ERROR;
    }
    p_addr->sun_path[0] = 0;
    memcpy(p_addr->sun_path + 1, name, namelen);
    p_addr->sun_family = AF_LOCAL;
    *socklen = namelen + offsetof(struct sockaddr_un, sun_path) + 1;
    return NO_ERR;
}

int main(int argc, char *argv[])
{
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

    /* interactive mode */
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
