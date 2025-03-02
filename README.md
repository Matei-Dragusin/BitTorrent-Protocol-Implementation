# Implementare Protocol BitTorrent

Aceasta tema implementeaza o versiune simplificata a protocolului BitTorrent folosind MPI si Pthreads. Implementarea urmeaza o arhitectura client-tracker, unde un proces actioneaza ca tracker, iar restul ca si clienti (peers).

## Arhitectura

### Tracker (Rank 0)

- Mentine o lista de fisiere si swarm-urile asociate
- Fiecare swarm contine o lista de clienti (seeds sau peers) care au segmente din fisier
- Nu stocheaza informatii despre segmentele detinute de fiecare client
- Gestioneaza cererile clientilor pentru:
  - Inregistrare initiala
  - Informatii despre swarm
  - Notificari de completare fisiere
  - Finalizare descarcari

### Clienti (Rank > 0)

Fiecare client ruleaza doua thread-uri:

1. Thread de Download:
   - Cere informatii despre swarm de la tracker
   - Descarca segmente de fisiere de la peers/seeds
   - Actualizeaza trackerul cand descarcarea unui fisier e completa
   - Salveaza fisierele complete

2. Thread de Upload:
   - Raspunde la cereri de segmente de la alti peers
   - Continua sa serveasca fisiere chiar si dupa ce si-a completat propriile descarcari

## Detalii de Implementare

### Protocol de Comunicare

- Foloseste diferite tipuri de mesaje (enum-uri) pentru diverse operatii
- Implementeaza operatiile de baza BitTorrent:
  - Descoperirea peer-ilor prin tracker
  - Schimbul de segmente intre peers
  - Actualizari ale swarm-ului

### Strategie de Descarcare

- Clientii variaza sursele de descarcare pentru a evita supraincarcarea unui peer
- Actualizeaza informatiile despre swarm la fiecare 10 segmente
- Fisierele sunt descarcate segment cu segment, progresul fiind urmarit folosind vectori de bool
- Segmentele pot fi descarcate in orice ordine de la peers disponibili

### Caracteristici de Eficienta

- Echilibrarea incarcarii prin rotirea peer-ilor in timpul descarcarilor
- Peers-ii devin seeds pentru fisierele complete, crescand disponibilitatea
- Implicare minima a trackerului in transferurile efective de fisiere
- Comunicare thread-safe folosind suport MPI_THREAD_MULTIPLE

### Gestionarea Fisierelor

- Fisierele sunt simulate folosind string-uri hash pentru segmente
- Fisierele descarcate sunt salvate in format: `client<rank>_<filename>`
- Foloseste structuri de date eficiente (vectori, map-uri) pentru urmarirea fisierelor si segmentelor

Implementarea se concentreaza pe mentinerea principiilor de baza BitTorrent, asigurand in acelasi timp o distributie eficienta si fiabila a fisierelor in retea.
