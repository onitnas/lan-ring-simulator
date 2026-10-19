# LAN ad anello in C — TCP, UDP, thread e mutex

## Obiettivo

L'utente sceglie un host mittente (1–5), un destinatario e un testo. Il messaggio attraversa l'anello in una sola direzione. Se raggiunge il destinatario viene consegnato; se completa il giro e ritorna al mittente senza essere stato consegnato viene scartato. Il logger registra l'invio iniziale e la ricezione finale.

## Architettura

```text
Host 1 (5001) -> Host 2 (5002) -> Host 3 (5003)
     ^                                  |
     |                                  v
Host 5 (5005) <- Host 4 (5004) <-------+

Host 1..5 -- UDP --> Logger (6000)
```

- **`host.c`**: un unico processo con cinque host logici, ciascuno con un thread ricevente TCP e un thread elaboratore (dieci thread in totale).
- **`logger.c`**: processo indipendente con server UDP in ascolto sulla porta 6000.
- Tutti i socket sono associati a **127.0.0.1**: la topologia è simulata sulla stessa macchina, ma la comunicazione utilizza veri socket.

## Buffer condiviso e mutua esclusione

Ogni host possiede un **buffer a uno slot**, condiviso tra i suoi due thread:

1. Il thread **ricevente** accetta una connessione TCP e inserisce il messaggio nel buffer.
2. Il thread **elaboratore** preleva il messaggio e controlla l'ID del destinatario.
3. Se il destinatario coincide con l'host corrente, il messaggio viene consegnato e registrato sul logger.
4. Se il messaggio ritorna all'host mittente, viene scartato.
5. Altrimenti viene inoltrato tramite TCP all'host successivo.

Il `pthread_mutex_t` protegge l'accesso concorrente al buffer. Le due variabili di condizione `non_pieno` e `non_vuoto` fanno attendere i thread rispettivamente quando il buffer è occupato o vuoto. `pthread_cond_wait()` rilascia temporaneamente il mutex mentre il thread attende e lo riacquisisce al risveglio. Un ulteriore mutex sincronizza alcune stampe sulla console del processo host.

**Nota:** non esiste un buffer unico per tutta la rete: ogni host ha il proprio buffer condiviso tra due thread. Possono essere in transito più messaggi contemporaneamente; i mutex non rendono l'intero anello a messaggio singolo.

## Formato del messaggio

Ogni messaggio contiene:

| Campo | Significato |
| --- | --- |
| `mittente` | ID dell'host che ha originato il messaggio |
| `destinatario` | ID dell'host destinatario |
| `testo` | Contenuto testuale (massimo 255 caratteri più terminatore) |

I campi numerici vengono codificati per la trasmissione TCP; l'invio e la ricezione gestiscono anche trasferimenti parziali.

## Requisiti

- Linux o ambiente POSIX compatibile
- Compilatore C (es. GCC)
- Libreria pthread e socket POSIX

## Compilazione

```bash
gcc  -pthread host.c -o host
gcc logger.c -o logger
```

## Esecuzione

Aprire **due terminali**. Avviare prima il logger:

```bash
./logger
```

Nel secondo terminale:

```bash
./host
```

Il programma chiede mittente, destinatario e testo. Inserire `0` come mittente per terminare il processo host.

### Esempio: consegna corretta

Input nel terminale degli host:

```text
Mittente (1-5, 0 per uscire): 1
Destinatario: 4
Testo: Ciao host 4
```

Il messaggio segue il percorso **1 → 2 → 3 → 4**. Output atteso nel terminale del logger:

```text
[LOGGER][host-1]Invia messaggio: Ciao host 4
[LOGGER][host-4]Riceve messaggio: Ciao host 4
```

### Esempio: destinatario inesistente

```text
Mittente (1-5, 0 per uscire): 2
Destinatario: 9
Testo: Messaggio di prova
```

Il messaggio compie il percorso **2 → 3 → 4 → 5 → 1 → 2** e viene scartato. Il logger registra l'invio; lo scarto viene stampato nel terminale degli host.

## Scelte progettuali e limiti

- TCP trasporta i messaggi tra host consecutivi; UDP trasporta i log senza garanzia di consegna.
- La simulazione è locale e non implementa cinque processi host indipendenti.
- Il buffer a uno slot introduce attesa quando è occupato, evitando sovrascritture.
- La chiusura del processo host non esegue un arresto ordinato dei thread.
- Non sono previsti ritrasmissione applicativa, persistenza dei messaggi o autenticazione.