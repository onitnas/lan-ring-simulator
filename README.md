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

## Compilazione

```bash
gcc -pthread host.c -o host
gcc logger.c -o logger
```

## Avvio

In sei terminali distinti:

```bash
./logger 6000
./host 1 6000
./host 2 6000
./host 3 6000
./host 4 6000
./host 5 6000
```

Il logger richiede la porta di ascolto. Senza argomenti stampa `Usage: ./logger <porta>` e termina. Ogni host richiede il proprio ID (1-5) e la porta UDP del logger. Le porte TCP sono 5001-5005 e l'anello segue l'ordine 1→2→3→4→5→1.

In un terminale host digitare, per esempio:

```text
4 Ciao host 4!
```

Il logger stampa:

```text
[LOGGER][host-1]Invia messaggio: Ciao host 4!
[LOGGER][host-4]Riceve messaggio: Ciao host 4!
```

Un destinatario inesistente (es. `9`) fa percorrere al messaggio tutto l'anello; quando torna al mittente, viene scartato.

## Thread, buffer e mutex

Ogni processo host contiene un thread ricevente TCP e un thread elaboratore. I due thread condividono un buffer di **un messaggio**. Il ricevente inserisce un messaggio nel buffer; l'elaboratore lo preleva e decide se consegnarlo, scartarlo o inoltrarlo. Un mutex protegge il buffer e due variabili di condizione permettono di attendere quando è pieno o vuoto senza consumare CPU inutilmente.

I mutex sono **locali a ciascun processo**: non esiste un mutex globale condiviso tra tutti i cinque host. Possono circolare più messaggi contemporaneamente.

## Note

- I processi devono essere avviati tutti prima di iniziare gli invii; altrimenti una connessione TCP può fallire.
- La comunicazione usa `127.0.0.1`: i sei processi sono indipendenti ma girano sulla stessa macchina.
- TCP è uno stream di byte: il codice gestisce letture e scritture parziali e serializza i campi numerici in network byte order.
- UDP non garantisce la consegna degli eventi al logger.
- Questa è una simulazione didattica: non implementa ritrasmissioni applicative, arresto coordinato o autenticazione dei messaggi.