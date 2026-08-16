#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <spawn.h>
#include <unistd.h>

extern char **environ;

int main(void) {
    int receiver = socket(AF_INET, SOCK_DGRAM, 0);
    int sender = socket(AF_INET, SOCK_DGRAM, 0);
    if (receiver < 0 || sender < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(0);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

    if (bind(receiver, (struct sockaddr *)&address, sizeof(address)) != 0) {
        perror("bind");
        return 1;
    }

    socklen_t address_length = sizeof(address);
    if (getsockname(receiver, (struct sockaddr *)&address, &address_length) != 0) {
        perror("getsockname");
        return 1;
    }

    struct timeval timeout = {.tv_sec = 2, .tv_usec = 0};
    setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    char payload[74];
    memset(payload, 0xAB, sizeof(payload));

    ssize_t sent = sendto(
        sender,
        payload,
        sizeof(payload),
        0,
        (struct sockaddr *)&address,
        address_length
    );
    if (sent != (ssize_t)sizeof(payload)) {
        perror("sendto");
        return 1;
    }

    int packets = 0;
    for (;;) {
        unsigned char buffer[65536];
        ssize_t received = recv(receiver, buffer, sizeof(buffer), 0);
        if (received < 0) {
            if (packets == 0) {
                perror("recv");
                return 1;
            }
            break;
        }

        packets++;
        printf("packet %d: %zd bytes", packets, received);
        if (received == 1) {
            printf(" value=%u", buffer[0]);
        }
        printf("\n");
    }

    close(sender);
    close(receiver);

    if (packets < 3) {
        fprintf(stderr, "expected at least 3 packets (00, 01, original), got %d\n", packets);
        return 1;
    }

    pid_t pid = 0;
    char *args[] = { "/usr/bin/true", NULL };
    if (posix_spawn(&pid, args[0], NULL, NULL, args, environ) != 0) {
        perror("posix_spawn");
        return 1;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "posix_spawn child failed\n");
        return 1;
    }

    printf("received %d packets before timeout\n", packets);
    return 0;
}
