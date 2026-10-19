#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define HOSTS 5
#define BASE_PORT 5001
#define LOGGER_PORT 6000
#define TEXT_SIZE 256
#define WIRE_SIZE (3 * sizeof(uint32_t) + TEXT_SIZE)

typedef struct {
    int mittente;
    int destinatario;
    char testo[TEXT_SIZE];
} Messaggio;

typedef struct {
    Messaggio msg;
    int pieno;
    pthread_mutex_t mutex;
    pthread_cond_t non_pieno;
    pthread_cond_t non_vuoto;
} Buffer;

typedef struct {
    int id;
    int server;
    Buffer buffer;
    pthread_t ricevente;
    pthread_t elaboratore;
} Host;

static Host hosts[HOSTS];
static pthread_mutex_t console_mutex = PTHREAD_MUTEX_INITIALIZER;

static int invia_tutto(int fd, const void *data, size_t size) {
    const char *p = data;
    while (size > 0) {
        ssize_t n = send(fd, p, size, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        p += n;
        size -= (size_t)n;
    }
    return 0;
}

static int ricevi_tutto(int fd, void *data, size_t size) {
    char *p = data;
    while (size > 0) {
        ssize_t n = recv(fd, p, size, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        p += n;
        size -= (size_t)n;
    }
    return 0;
}

/* Protocollo TCP: 3 campi a 32 bit in network order + testo fisso. */
static void codifica(const Messaggio *m, unsigned char out[WIRE_SIZE]) {
    uint32_t header[3] = {htonl((uint32_t)m->mittente),
                          htonl((uint32_t)m->destinatario), htonl(TEXT_SIZE)};
    memcpy(out, header, sizeof(header));
    memcpy(out + sizeof(header), m->testo, TEXT_SIZE);
}

static int decodifica(const unsigned char in[WIRE_SIZE], Messaggio *m) {
    uint32_t header[3];
    memcpy(header, in, sizeof(header));
    if (ntohl(header[2]) != TEXT_SIZE)
        return -1;
    m->mittente = (int)ntohl(header[0]);
    m->destinatario = (int)ntohl(header[1]);
    memcpy(m->testo, in + sizeof(header), TEXT_SIZE);
    m->testo[TEXT_SIZE - 1] = '\0';
    return m->mittente >= 1 && m->mittente <= HOSTS ? 0 : -1;
}

static void invia_log(int id, const char *azione, const char *testo) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket UDP");
        return;
    }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(LOGGER_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    char line[512];
    snprintf(line, sizeof(line), "[LOGGER][host-%d]%s messaggio: %s", id,
             azione, testo);
    if (sendto(fd, line, strlen(line), 0, (struct sockaddr *)&addr,
               sizeof(addr)) < 0)
        perror("sendto logger");
    close(fd);
}

static void inserisci(Buffer *b, const Messaggio *m) {
    pthread_mutex_lock(&b->mutex);
    while (b->pieno)
        pthread_cond_wait(&b->non_pieno, &b->mutex);
    b->msg = *m;
    b->pieno = 1;
    pthread_cond_signal(&b->non_vuoto);
    pthread_mutex_unlock(&b->mutex);
}

static Messaggio preleva(Buffer *b) {
    pthread_mutex_lock(&b->mutex);
    while (!b->pieno)
        pthread_cond_wait(&b->non_vuoto, &b->mutex);
    Messaggio m = b->msg;
    b->pieno = 0;
    pthread_cond_signal(&b->non_pieno);
    pthread_mutex_unlock(&b->mutex);
    return m;
}

static int invia_tcp(int id, const Messaggio *m) {
    int successivo = id % HOSTS + 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket TCP");
        return -1;
    }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(BASE_PORT + successivo - 1);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect TCP");
        close(fd);
        return -1;
    }
    unsigned char wire[WIRE_SIZE];
    codifica(m, wire);
    int rc = invia_tutto(fd, wire, sizeof(wire));
    if (rc != 0)
        perror("send TCP");
    close(fd);
    return rc;
}

static void *ricevente(void *arg) {
    Host *h = arg;
    for (;;) {
        int client = accept(h->server, NULL, NULL);
        if (client < 0) {
            if (errno != EINTR)
                perror("accept");
            continue;
        }
        unsigned char wire[WIRE_SIZE];
        Messaggio m;
        if (ricevi_tutto(client, wire, sizeof(wire)) == 0 &&
            decodifica(wire, &m) == 0) {
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
            invia_log(h->id, "Riceve", m.testo);
        } else if (m.mittente == h->id) {
            pthread_mutex_lock(&console_mutex);
            printf("\n[host-%d] Scartato: destinatario %d inesistente (%s)\n",
                   h->id, m.destinatario, m.testo);
            fflush(stdout);
            pthread_mutex_unlock(&console_mutex);
        } else {
            invia_tcp(h->id, &m);
        }
    }
    return NULL;
}

static int crea_server(int id) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(BASE_PORT + id - 1);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 16) < 0) {
        perror("bind/listen");
        close(fd);
        return -1;
    }
    return fd;
}

int main(void) {
    /* Apriamo tutti i server prima di avviare i thread: niente sleep(1). */
    for (int i = 0; i < HOSTS; ++i) {
        hosts[i].id = i + 1;
        hosts[i].server = crea_server(i + 1);
        if (hosts[i].server < 0)
            return EXIT_FAILURE;
        Buffer *b = &hosts[i].buffer;
        pthread_mutex_init(&b->mutex, NULL);
        pthread_cond_init(&b->non_pieno, NULL);
        pthread_cond_init(&b->non_vuoto, NULL);
    }
    for (int i = 0; i < HOSTS; ++i) {
        if (pthread_create(&hosts[i].ricevente, NULL, ricevente, &hosts[i]) !=
                0 ||
            pthread_create(&hosts[i].elaboratore, NULL, elaboratore,
                           &hosts[i]) != 0) {
            fprintf(stderr, "Errore creazione thread\n");
            return EXIT_FAILURE;
        }
    }
    puts("Anello TCP pronto: 5 host (porte 5001-5005). Logger UDP: 6000.");
    char line[64];
    for (;;) {
        int mittente, destinatario;
        Messaggio m = {0};
        printf("\nMittente (1-5, 0 per uscire): ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin) ||
            sscanf(line, "%d", &mittente) != 1)
            break;
        if (mittente == 0)
            break;
        if (mittente < 1 || mittente > HOSTS) {
            puts("Mittente non valido");
            continue;
        }
        printf("Destinatario: ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin) ||
            sscanf(line, "%d", &destinatario) != 1)
            break;
        printf("Testo: ");
        fflush(stdout);
        if (!fgets(m.testo, sizeof(m.testo), stdin))
            break;
        m.testo[strcspn(m.testo, "\n")] = '\0';
        m.mittente = mittente;
        m.destinatario = destinatario;
        if (destinatario == mittente) {
            /* Un messaggio a se stessi e' consegnato localmente. */
            invia_log(mittente, "Invia", m.testo);
            invia_log(mittente, "Riceve", m.testo);
        } else {
            invia_log(mittente, "Invia", m.testo);
            if (invia_tcp(mittente, &m) != 0)
                fprintf(stderr, "Invio non riuscito\n");
        }
    }
    /* Il processo termina; il sistema operativo chiude i socket e i thread. */
    return 0;
}
