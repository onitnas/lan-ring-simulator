#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>

#define N_HOST 5
#define BASE_PORT 5001
#define MAX_TEXT 256
#define WIRE_SIZE (12 + MAX_TEXT)

typedef struct {
    int mittente, destinatario;
    uint32_t id_messaggio;
    char testo[MAX_TEXT];
} Messaggio;

typedef struct {
    Messaggio msg;
    int pieno;
    pthread_mutex_t mutex;
    pthread_cond_t non_pieno, non_vuoto;
} Buffer;

typedef struct {
    int id, porta_logger;
    Buffer buffer;
} Host;

static pthread_mutex_t mutex_console = PTHREAD_MUTEX_INITIALIZER;

static int send_all(int fd, const void *buf, size_t len) {
    const unsigned char *p = buf;
    while (len) {
        ssize_t n = send(fd, p, len, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; len -= (size_t)n;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t len) {
    unsigned char *p = buf;
    while (len) {
        ssize_t n = recv(fd, p, len, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; len -= (size_t)n;
    }
    return 0;
}

static void serializza(const Messaggio *m, unsigned char wire[WIRE_SIZE]) {
    uint32_t fields[3] = {
        htonl((uint32_t)m->mittente), htonl((uint32_t)m->destinatario),
        htonl(m->id_messaggio)
    };
    memcpy(wire, fields, sizeof(fields));
    memcpy(wire + 12, m->testo, MAX_TEXT);
}

static void deserializza(const unsigned char wire[WIRE_SIZE], Messaggio *m) {
    uint32_t fields[3];
    memcpy(fields, wire, sizeof(fields));
    m->mittente = (int)ntohl(fields[0]);
    m->destinatario = (int)ntohl(fields[1]);
    m->id_messaggio = ntohl(fields[2]);
    memcpy(m->testo, wire + 12, MAX_TEXT);
    m->testo[MAX_TEXT-1] = '\0';
}

static void invia_log(const Host *h, const char *azione, const char *testo) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket UDP"); return; }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)h->porta_logger);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    char linea[512];
    snprintf(linea, sizeof(linea), "[LOGGER][host-%d]%s messaggio: %s",
             h->id, azione, testo);
    if (sendto(fd, linea, strlen(linea), 0, (struct sockaddr *)&addr,
               sizeof(addr)) < 0) perror("sendto");
    close(fd);
}

static int invia_tcp(int id, const Messaggio *m) {
    int successivo = id % N_HOST + 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket TCP"); return -1; }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(BASE_PORT + successivo - 1);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Host %d: impossibile contattare host %d: %s\n",
                id, successivo, strerror(errno));
        close(fd); return -1;
    }
    unsigned char wire[WIRE_SIZE];
    serializza(m, wire);
    int rc = send_all(fd, wire, sizeof(wire));
    if (rc < 0) perror("send TCP");
    close(fd);
    return rc;
}

static void inserisci(Buffer *b, const Messaggio *m) {
    pthread_mutex_lock(&b->mutex);
    while (b->pieno) pthread_cond_wait(&b->non_pieno, &b->mutex);
    b->msg = *m;
    b->pieno = 1;
    pthread_cond_signal(&b->non_vuoto);
    pthread_mutex_unlock(&b->mutex);
}

static Messaggio preleva(Buffer *b) {
    pthread_mutex_lock(&b->mutex);
    while (!b->pieno) pthread_cond_wait(&b->non_vuoto, &b->mutex);
    Messaggio m = b->msg;
    b->pieno = 0;
    pthread_cond_signal(&b->non_pieno);
    pthread_mutex_unlock(&b->mutex);
    return m;
}

static void *ricevente(void *arg) {
    Host *h = arg;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket server"); exit(EXIT_FAILURE); }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(BASE_PORT + h->id - 1);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 16) < 0) {
        perror("bind/listen"); exit(EXIT_FAILURE);
    }
    for (;;) {
        int client = accept(fd, NULL, NULL);
        if (client < 0) { if (errno != EINTR) perror("accept"); continue; }
        unsigned char wire[WIRE_SIZE];
        if (recv_all(client, wire, sizeof(wire)) == 0) {
            Messaggio m;
            deserializza(wire, &m);
            if (m.mittente >= 1 && m.mittente <= N_HOST)
                inserisci(&h->buffer, &m);
        }
        close(client);
    }
    return NULL;
}

static void *elaboratore(void *arg) {
    Host *h = arg;
    for (;;) {
        Messaggio m = preleva(&h->buffer);
        if (m.destinatario == h->id) {
            invia_log(h, "Riceve", m.testo);
            pthread_mutex_lock(&mutex_console);
            printf("\n[host-%d] Ricevuto da host-%d: %s\n", h->id, m.mittente, m.testo);
            fflush(stdout);
            pthread_mutex_unlock(&mutex_console);
        } else if (m.mittente == h->id) {
            pthread_mutex_lock(&mutex_console);
            printf("\n[host-%d] Messaggio scartato: destinatario %d non trovato\n",
                   h->id, m.destinatario);
            fflush(stdout);
            pthread_mutex_unlock(&mutex_console);
        } else {
            invia_tcp(h->id, &m);
        }
    }
    return NULL;
}

static int porta_valida(const char *s) {
    char *end;
    errno = 0;
    long n = strtol(s, &end, 10);
    if (errno || !*s || *end || n < 1 || n > 65535) return -1;
    return (int)n;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <id_host:1-5> <porta_logger>\n", argv[0]);
        return 1;
    }
    int id = porta_valida(argv[1]);
    int porta = porta_valida(argv[2]);
    if (id < 1 || id > N_HOST || porta < 0) {
        fprintf(stderr, "Usage: %s <id_host:1-5> <porta_logger>\n", argv[0]);
        return 1;
    }
    Host h = { .id = id, .porta_logger = porta };
    pthread_mutex_init(&h.buffer.mutex, NULL);
    pthread_cond_init(&h.buffer.non_pieno, NULL);
    pthread_cond_init(&h.buffer.non_vuoto, NULL);
    pthread_t t_rx, t_work;
    if (pthread_create(&t_rx, NULL, ricevente, &h) ||
        pthread_create(&t_work, NULL, elaboratore, &h)) {
        fprintf(stderr, "Errore creazione thread\n"); return 1;
    }
    printf("Host %d avviato su TCP %d (successivo: %d, logger UDP: %d)\n",
           id, BASE_PORT + id - 1, id % N_HOST + 1, porta);
    printf("Invio: <destinatario> <testo>  |  exit per uscire\n");
    char line[512];
    uint32_t seq = 0;
    while (fgets(line, sizeof(line), stdin)) {
        if (!strcmp(line, "exit\n") || !strcmp(line, "exit")) break;
        char *p = line;
        errno = 0;
        char *end;
        long dest = strtol(p, &end, 10);
        if (errno || end == p || (*end != ' ' && *end != '\t')) {
            puts("Formato: <destinatario> <testo>"); continue;
        }
        while (*end == ' ' || *end == '\t') end++;
        end[strcspn(end, "\n")] = '\0';
        if (!*end || dest < 1 || dest > 65535) {
            puts("Destinatario/testo non valido"); continue;
        }
        Messaggio m = {0};
        m.mittente = id;
        m.destinatario = (int)dest;
        m.id_messaggio = ++seq;
        snprintf(m.testo, sizeof(m.testo), "%s", end);
        if (dest == id) {
            invia_log(&h, "Invia", m.testo);
            invia_log(&h, "Riceve", m.testo);
            continue;
        }
        if (invia_tcp(id, &m) == 0) invia_log(&h, "Invia", m.testo);
    }
    puts("Chiusura del processo host.");
    return 0;
}
