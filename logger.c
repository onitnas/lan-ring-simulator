#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define LOGGER_PORT 6000
#define LOG_SIZE 512

int main(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(LOGGER_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind logger");
        close(fd);
        return EXIT_FAILURE;
    }
    printf("Logger UDP in ascolto sulla porta %d\n", LOGGER_PORT);
    for (;;) {
        char buf[LOG_SIZE];
        ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0, NULL, NULL);
        if (n < 0) {
            perror("recvfrom");
            continue;
        }
        buf[n] = '\0';
        puts(buf);
        fflush(stdout);
    }
}
