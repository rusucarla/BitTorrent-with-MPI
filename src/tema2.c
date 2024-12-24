#include <mpi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100

// Tag-uri pentru MPI
// Client -> Tracker : "This is all i've got"
#define MSG_INIT_FILES 1
// Tracker -> Client : "Got the files, continue"
#define MSG_INIT_ACK 2
// Client -> Tracker : "I want this file, show me who has it (peers/seeds)"
#define MSG_REQUEST_SWARM 3
// Tracker -> Client : "Here's the swarm"
#define MSG_SWARM_LIST 4
// Client -> Clients in Swarm : "I'd like this chunk"
#define MSG_CHUNK_REQUEST 5
// Client -> Client who requested : "Here you go"
#define MSG_SEND_ACK 6
// Client -> Client who requested : "I don't have it"
#define MSG_SEND_NACK 7

// Struct for file -> owned and wanted
// (easier to transition to a seed from a peer)
typedef struct {
    char filename[MAX_FILENAME];
    int num_chunks;
    char chunk_hashes[MAX_CHUNKS][HASH_SIZE + 1];
    // 1 - have , 0 - don't have
    int have_chunk[MAX_CHUNKS];
    bool owned;
} ManagedFile;

// Struct for a client
typedef struct {
    int rank;
    int numtasks;
    int count_files;
    ManagedFile files[MAX_FILES];
} PeerState;

// Struct for information about a file in the tracker
typedef struct {
    char filename[MAX_FILENAME];
    int num_chunks;
    char chunk_hashes[MAX_CHUNKS][HASH_SIZE + 1];
    int swarm_size;
    int swarm_members[128];
} TrackerFile;

int read_input_file(int rank, ManagedFile *files, int *count_files) {
    char filename_in[64];
    sprintf(filename_in, "in%d.txt", rank);

    FILE *fin = fopen(filename_in, "r");
    if (!fin) {
        fprintf(stderr, "Eroare la deschiderea fisierului %s\n", filename_in);
        return -1;
    }

    // Format:
    // 1) numar_fisiere_detinute
    // 2) <nume_fisier> <numar_segmente>
    //    <hash1>
    //    <hash2>
    //    ...
    // (.....)
    // urmat de:
    // 3) numar_fisiere_dorite
    // 4) <nume_fisier_dorit_1>
    //    <nume_fisier_dorit_2>
    //    ...
    int count_owned;
    fscanf(fin, "%d", count_owned);

    for(int i = 0; i < *count_owned; i++) {
        fscanf(fin, "%s", files[i].filename);
        fscanf(fin, "%d", &files[i].num_chunks);
        // Citim hash-urile segmentelor
        for(int c = 0; c < files[i].num_chunks; c++) {
            fscanf(fin, "%s", files[i].chunk_hashes[c]);
        }
    }

    // Have to read the desired files as well
    fscanf(fin, "%d", count_wanted);
    for (int i = 0; i < *count_wanted; i++) {
        fscanf(fin, "%s", wanted_files[i].filename);
    }
    fclose(fin);
    return 0;
}

void *download_thread_func(void *arg) {
    // int rank = *(int *)arg;
    PeerState *peer_state = (PeerState *)arg;
    int rank = peer_state->rank;



    return NULL;
}

void *upload_thread_func(void *arg) {
    // int rank = *(int *)arg;

    return NULL;
}

void tracker(int numtasks, int rank) {
    int count_init = 0;
    int total_clients = numtasks - 1;
}

void peer(int numtasks, int rank) {
    pthread_t download_thread;
    pthread_t upload_thread;
    void *status;
    int r;

    // Create peer state
    PeerState *peer_state = (PeerState *)calloc(1, sizeof(PeerState));
    peer_state->rank = rank;
    peer_state->numtasks = numtasks;
    peer_state->count_files = 0;

    // r = pthread_create(&download_thread, NULL, download_thread_func, (void
    // *)&rank);
    r = pthread_create(&download_thread, NULL, download_thread_func, (void *)peer_state);
    if (r) {
        printf("Eroare la crearea thread-ului de download\n");
        exit(-1);
    }

    // r = pthread_create(&upload_thread, NULL, upload_thread_func, (void
    // *)&rank);
    r = pthread_create(&upload_thread, NULL, upload_thread_func, (void *)peer_state);
    if (r) {
        printf("Eroare la crearea thread-ului de upload\n");
        exit(-1);
    }

    r = pthread_join(download_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_join(upload_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de upload\n");
        exit(-1);
    }

    free(peer_state);
}

int main(int argc, char *argv[]) {
    int numtasks, rank;

    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE) {
        fprintf(stderr, "MPI nu are suport pentru multi-threading ! O NU !\n");
        exit(-1);
    }
    MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == TRACKER_RANK) {
        tracker(numtasks, rank);
    } else {
        peer(numtasks, rank);
    }

    MPI_Finalize();
}
