#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "Usage: %s <porta>\n", argv[0]); return 1; }
    char *end; errno = 0;
    long port = strtol(argv[1], &end, 10);
    if (errno || *end || port < 1 || port > 65535) {
        fprintf(stderr, "Usage: %s <porta> (1-65535)\n", argv[0]); return 1;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); return 1;
    }
    printf("Logger UDP in ascolto sulla porta %ld\n", port);
    fflush(stdout);
    for (;;) {
        char buffer[512];
        ssize_t n = recvfrom(fd, buffer, sizeof(buffer)-1, 0, NULL, NULL);
        if (n < 0) { if (errno == EINTR) continue; perror("recvfrom"); continue; }
        buffer[n] = '\0';
        puts(buffer);
        fflush(stdout);
    }
}
