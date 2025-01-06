#include <mpi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100

// ------ Tags ---------
// Client -> Tracker : "I have these files"
#define MSG_INIT_FILES 1
// Tracker -> Client : "Ok ! Start downloading/uploading"
#define MSG_INIT_ACK 2
// Client -> Tracker : "What peers have this file?"
#define MSG_REQUEST_SWARM 3
// Tracker -> Client : "This is the swarm for the file"
// This includes the number of chunks, the hashes of the chunks, and the swarm
// members
// This is only for the first request, for subsequent requests, the tracker will
// send only the swarm members
#define MSG_SWARM_INFO 4
// Client -> Client(Peer/Seed) : "Give me this chunk, if you have it"
#define MSG_CHUNK_REQUEST 5
// Client(Peer/Seed) -> Client : "Here is the chunk"
#define MSG_SEND_ACK 6
// Client(Peer/Seed) -> Client : "I don't have the chunk"
#define MSG_SEND_NACK 7
// Client -> Tracker : "I want to refresh the swarm for this file"
#define MSG_REFRESH_SWARM 8
// Tracker -> Client : "Here is the swarm for the file"
#define MSG_SWARM_LIST 9
// Client -> Tracker : "I have finished downloading this file"
#define MSG_FILE_COMPLETE 10
// Client -> Tracker : "I have finished downloading all the files I wanted"
// This closes the download thread for the client, but the upload thread will
// continue to run until the tracker sends MSG_STOP_ALL
#define MSG_FINISHED_ALL 11
// Tracker -> Client (upload thread) : "I am stopping all uploads"
#define MSG_STOP_ALL 12

// -------- Structs --------
// for the files owned by a peer => seed
// as such it has all the chunks
typedef struct {
    char filename[MAX_FILENAME];
    int num_chunks;
    char chunk_hashes[MAX_CHUNKS][HASH_SIZE + 1];
} OwnedFile;

// for the files wanted by a peer => client (downloader)
// receives chunks from the the tracker
// and downloads them from the swarm
typedef struct {
    char filename[MAX_FILENAME];
    int num_chunks;
    int have_chunk[MAX_CHUNKS];
    char chunk_hashes[MAX_CHUNKS][HASH_SIZE + 1];
    int finished;
    int swarm_size;
    int swarm_members[128];
} WantedFile;

// for the state of a peer
// useful to keep track of the files owned and wanted
// and the mutex for the wanted files
// in both the download and upload threads
typedef struct {
    int rank;
    int numtasks;
    int owned_files_count;
    int wanted_files_count;
    OwnedFile owned_files[MAX_FILES];
    WantedFile wanted_files[MAX_FILES];
    // will protets the wanted files
    // mostly the have_chunk array
    pthread_mutex_t lockWanted;
} PeerState;

// for the requested chunk
typedef struct {
    char filename[MAX_FILENAME];
    int chunk_index;
} RequestedChunk;

// for the ack message
typedef struct {
    int chunk_index;
} AckMsg;

// for the tracked files
// will send to clients the first time they request a file
typedef struct {
    char filename[MAX_FILENAME];
    int num_chunks;
    char chunk_hashes[MAX_CHUNKS][HASH_SIZE + 1];
    int swarm_members[128];
    int swarm_size;
} TrackedFile;

// global array of tracked files
TrackedFile g_tracked_files[200];
int g_num_tracked_files = 0;

// Helper for timestamp
static double get_time() {
    return MPI_Wtime();
}

// Read the input file in<rank>.txt
int read_input_file(int rank, OwnedFile *owned, int *count_owned,
                    WantedFile *wanted, int *count_wanted) {
    char fname_in[64];
    sprintf(fname_in, "in%d.txt", rank);
    FILE *fin = fopen(fname_in, "r");
    if (!fin) {
        fprintf(stderr, "[%.4f] [Peer %d] Eroare la deschiderea %s\n", get_time(), rank, fname_in);
        return -1;
    }
    fscanf(fin, "%d", count_owned);
    for (int i = 0; i < *count_owned; i++) {
        fscanf(fin, "%s", owned[i].filename);
        fscanf(fin, "%d", &owned[i].num_chunks);
        for (int c = 0; c < owned[i].num_chunks; c++) {
            fscanf(fin, "%s", owned[i].chunk_hashes[c]);
        }
    }

    fscanf(fin, "%d", count_wanted);
    for (int i = 0; i < *count_wanted; i++) {
        fscanf(fin, "%s", wanted[i].filename);
        wanted[i].finished = 0;
    }
    fclose(fin);
    return 0;
}

// Sends the owned files to the tracker
// Will use a big enough buffer to send all the files
// to use char instead of OwnedFile
void send_init_files_to_tracker(int rank, int count_owned, OwnedFile *owned) {
    char buffer[5000];
    int off = 0;
    memcpy(buffer + off, &count_owned, sizeof(int));
    off += sizeof(int);

    for (int i = 0; i < count_owned; i++) {
        memcpy(buffer + off, owned[i].filename, MAX_FILENAME);
        off += MAX_FILENAME;

        memcpy(buffer + off, &owned[i].num_chunks, sizeof(int));
        off += sizeof(int);

        for (int c = 0; c < owned[i].num_chunks; c++) {
            memcpy(buffer + off, owned[i].chunk_hashes[c], HASH_SIZE + 1);
            off += (HASH_SIZE + 1);
        }
    }

    // printf("[%.4f] [Peer %d] SEND MSG_INIT_FILES (owned=%d)\n", get_time(), rank, count_owned);
    MPI_Send(buffer, off, MPI_CHAR, TRACKER_RANK, MSG_INIT_FILES, MPI_COMM_WORLD);
}

static void refresh_swarm(PeerState *ps, int w) {
    WantedFile *wf = &ps->wanted_files[w];
    // Send refresh request to tracker for this file
    MPI_Send(wf->filename, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, MSG_REFRESH_SWARM, MPI_COMM_WORLD);

    // Wait for the new swarm info
    MPI_Status st;
    int swarm_size;
    // 1. swarm size
    MPI_Recv(&swarm_size, 1, MPI_INT, TRACKER_RANK, MSG_SWARM_LIST, MPI_COMM_WORLD, &st);

    wf->swarm_size = swarm_size;
    for (int i = 0; i < swarm_size; i++) {
        // 2. swarm members
        MPI_Recv(&wf->swarm_members[i], 1, MPI_INT, TRACKER_RANK, MSG_SWARM_LIST, MPI_COMM_WORLD, &st);
    }

    // printf("[%.4f] [Peer %d DOWNLOAD] REFRESH %s => swarm_size=%d => ",
    //        get_time(), ps->rank, wf->filename, wf->swarm_size);
    // for (int i = 0; i < swarm_size; i++) {
    //     printf("%d ", wf->swarm_members[i]);
    // }
    // printf("\n");
}

static int request_chunk_from_peer(PeerState *ps, int w, int c, int peer_r) {
    WantedFile *wf = &ps->wanted_files[w];
    RequestedChunk rc;
    strcpy(rc.filename, wf->filename);
    rc.chunk_index = c;

    // double now = get_time();
    // printf("[%.4f] [Peer %d DOWNLOAD] -> [Peer %d] CHUNK_REQUEST %s#%d\n",
    //        now, ps->rank, peer_r, wf->filename, c);

    // Send request to peer for chunk c of file wf->filename
    MPI_Send(&rc, sizeof(rc), MPI_CHAR, peer_r, MSG_CHUNK_REQUEST, MPI_COMM_WORLD);

    MPI_Status st;
    // I used a buffer to receive the ACK/NACK to be able to discard if i don't
    // receive a ACK/NACK message (had some issues with receiving only the tag)
    char buffer[100];
    MPI_Recv(buffer, sizeof(buffer), MPI_CHAR, peer_r, MPI_ANY_TAG, MPI_COMM_WORLD, &st);

    if (st.MPI_TAG == MSG_SEND_ACK) {
        int ack_chunk_idx = *(int *)buffer;
        // Should'nt happen, but just in case
        if (ack_chunk_idx != c) {
            // printf("[%.4f] [Peer %d DOWNLOAD] chunk #%d of %s -> ACK from %d, but wrong chunk idx %d\n",
            //        get_time(), ps->rank, c, wf->filename, peer_r, ack_chunk_idx);
            return 0;
        }
        // Protect access to have_chunk
        pthread_mutex_lock(&ps->lockWanted);
        wf->have_chunk[c] = 1;
        pthread_mutex_unlock(&ps->lockWanted);
        // printf("[%.4f] [Peer %d DOWNLOAD] chunk #%d of %s -> GOT IT from %d\n",
        //        get_time(), ps->rank, c, wf->filename, peer_r);
        return 1;
    } else {
        // int nack_chunk_idx = *(int *)buffer; // not used
        // printf("[%.4f] [Peer %d DOWNLOAD] chunk #%d of %s -> NACK from %d\n",
        //        get_time(), ps->rank, c, wf->filename, peer_r);
        return 0;
    }
}

void *download_thread_func(void *arg) {
    PeerState *ps = (PeerState *)arg;
    int rank = ps->rank;

    // 1. in<rank>.txt
    int rc = read_input_file(rank,
                             ps->owned_files, &ps->owned_files_count,
                             ps->wanted_files, &ps->wanted_files_count);
    if (rc < 0) {
        pthread_exit(NULL);
    }
    // printf("[%.4f] [Peer %d] owns=%d, wants=%d\n", get_time(), rank, ps->owned_files_count, ps->wanted_files_count);

    // 2. MSG_INIT_FILES
    send_init_files_to_tracker(rank, ps->owned_files_count, ps->owned_files);

    // 3. Wait MSG_INIT_ACK
    MPI_Status st;
    MPI_Recv(NULL, 0, MPI_CHAR, TRACKER_RANK, MSG_INIT_ACK, MPI_COMM_WORLD, &st);

    // printf("[%.4f] [Peer %d] GOT MSG_INIT_ACK. Start downloading.\n", get_time(), rank);

    // printf("[%.4f] [Peer %d] read_input: owned_count=%d, wanted_count=%d\n",
    //        get_time(), rank, ps->owned_files_count, ps->wanted_files_count);
    // for (int i = 0; i < ps->owned_files_count; i++) {
    //     printf("   Owned: %s (num_chunks=%d)\n", ps->owned_files[i].filename, ps->owned_files[i].num_chunks);
    // }
    // for (int i = 0; i < ps->wanted_files_count; i++) {
    //     printf("   Wanted: %s\n", ps->wanted_files[i].filename);
    // }

    // 4. Get swarm info for wanted files from tracker (MSG_REQUEST_SWARM)
    for (int i = 0; i < ps->wanted_files_count; i++) {
        WantedFile *wf = &ps->wanted_files[i];
        // printf("[%.4f] [Peer %d DOWNLOAD] Requesting SWARM for %s\n", get_time(), rank, wf->filename);

        MPI_Send(wf->filename, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, MSG_REQUEST_SWARM, MPI_COMM_WORLD);

        // Recv swarm info in multiple steps
        int n_chunks;
        MPI_Recv(&n_chunks, 1, MPI_INT, TRACKER_RANK, MSG_SWARM_INFO, MPI_COMM_WORLD, &st);
        wf->num_chunks = n_chunks;
        // Just in case the file is not known by the tracker
        if (n_chunks == 0) {
            // printf("[%.4f] [Peer %d DOWNLOAD] file=%s not known by tracker.\n", get_time(), rank, wf->filename);
            wf->swarm_size = 0;
            continue;
        }
        pthread_mutex_lock(&ps->lockWanted);
        for (int c = 0; c < n_chunks; c++) {
            MPI_Recv(wf->chunk_hashes[c], HASH_SIZE + 1, MPI_CHAR, TRACKER_RANK, MSG_SWARM_INFO, MPI_COMM_WORLD, &st);
            
            wf->have_chunk[c] = 0;
            
        }
        pthread_mutex_unlock(&ps->lockWanted);
        MPI_Recv(&wf->swarm_size, 1, MPI_INT, TRACKER_RANK, MSG_SWARM_INFO, MPI_COMM_WORLD, &st);
        for (int s = 0; s < wf->swarm_size; s++) {
            MPI_Recv(&wf->swarm_members[s], 1, MPI_INT, TRACKER_RANK, MSG_SWARM_INFO, MPI_COMM_WORLD, &st);
        }
        // printf("[%.4f] [Peer %d DOWNLOAD] %s => num_chunks=%d, swarm_size=%d => ",
        //        get_time(), rank, wf->filename, wf->num_chunks, wf->swarm_size);
        // for (int s = 0; s < wf->swarm_size; s++) {
        //     printf("%d ", wf->swarm_members[s]);
        // }
        // printf("\n");
    }

    // Want to randomize the order of chunks to download
    srand(time(NULL) ^ (rank * 13)); // seed with rank

    // 5. Downloading through the swarm
    for (int w = 0; w < ps->wanted_files_count; w++) {
        WantedFile *wf = &ps->wanted_files[w];
        // printf("[%.4f] [Peer %d DOWNLOAD] File %s has %d chunks\n", get_time(), rank, wf->filename, wf->num_chunks);
        if (wf->num_chunks == 0) continue;

        int downloaded = 0;
        int total = wf->num_chunks;

        int segs_since_refresh = 0;

        int no_progress_rounds = 0;

        // array to shuffle the chunks
        int *chunk_order = (int *)malloc(total * sizeof(int));
        for (int i = 0; i < total; i++) {
            chunk_order[i] = i;
        }
        for (int i = 0; i < total - 1; i++){
            int j = i + rand() % (total - i);
            int aux = chunk_order[i];
            chunk_order[i] = chunk_order[j];
            chunk_order[j] = aux;
        }

        // I want to try to have a exponential backoff for the refresh
        int backoff_time = 100000;  // 0.1s in ms


        while (downloaded < total) {
            int progress = 0;

            // Try to download all chunks
            for (int i = 0; i < total; i++) {
                // Get the chunk index from the shuffled array
                int c = chunk_order[i];
                // Check if we have the chunk so we don't request it again
                int have_it = 0;
                pthread_mutex_lock(&ps->lockWanted);
                have_it = wf->have_chunk[c];
                pthread_mutex_unlock(&ps->lockWanted);
                if (!have_it) {
                    // We contact all peers in the swarm to get the chunk
                    // Until we get it using the round robin approach to make
                    // sure we don't overload a peer
                    for (int attempt = 0; attempt < wf->swarm_size; attempt++) {
                        // Round robin
                        int idx = (c + attempt) % wf->swarm_size;
                        int peer_r = wf->swarm_members[idx];
                        // 1 - ACK (we put have_chuck[c] = 1), 0 - NACK
                        int ok = request_chunk_from_peer(ps, w, c, peer_r);
                        if (ok) {
                            downloaded++;
                            progress++;
                            segs_since_refresh++;

                            // after 10 segments => refresh
                            if (segs_since_refresh == 10 && downloaded < total) {
                                refresh_swarm(ps, w);
                                segs_since_refresh = 0;
                            }
                            break;  // chunk c is downloaded, we go to the next one
                        }
                    }
                }
            }
            // After a round we check if we made any progress
            if (progress == 0) {
                no_progress_rounds++;
                // If we didn't make any progress for 3 rounds we refresh the
                // swarm and we sleep for 0.1s to give the other peers a chance
                // to download the chunks they need => this maybe helps us with
                // future downloads
                // 3 and 0.1 are arbitrary values, but small enough to not make
                // a big impact on the download time
                if (no_progress_rounds >= 3) {
                    refresh_swarm(ps, w);
                    usleep(backoff_time);  // 0.1s for now
                    backoff_time *= 2;
                    // make sure we don't sleep too much
                    if (backoff_time > 1000000) {
                        backoff_time = 1000000; // max 1s
                    }
                }
            } else {
                no_progress_rounds = 0;
                // reset the backoff time
                backoff_time = 100000; // 0.1s
            }
        }
        free(chunk_order);
        // Signal the tracker that we finished downloading the file
        wf->finished = 1;
        // printf("[%.4f] [Peer %d DOWNLOAD] File %s fully downloaded!\n",
        //        get_time(), rank, wf->filename);

        MPI_Send(wf->filename, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, MSG_FILE_COMPLETE, MPI_COMM_WORLD);

        // Save the file client<rank>_<filename>
        char out_name[64];
        sprintf(out_name, "client%d_%s", rank, wf->filename);
        FILE *fout = fopen(out_name, "w");
        // I know the buffer is big enough to hold all the hashes
        // this is maybe faster than writing them one by one
        char buffer[8192];
        int offset = 0;
        for (int c = 0; c < wf->num_chunks; c++) {
            // Add newline except for the last chunk
            if (c < wf->num_chunks - 1) {
                offset += sprintf(buffer + offset, "%s\n", wf->chunk_hashes[c]);
            } else {
                offset += sprintf(buffer + offset, "%s", wf->chunk_hashes[c]);
            }
        }

        fwrite(buffer, 1, offset, fout);
        fclose(fout);
    }
    // Signal the tracker that we finished downloading all the files => close the download thread
    MPI_Send(NULL, 0, MPI_CHAR, TRACKER_RANK, MSG_FINISHED_ALL, MPI_COMM_WORLD);
    pthread_exit(NULL);
}

// UPLOAD
void *upload_thread_func(void *arg) {
    PeerState *ps = (PeerState *)arg;
    int rank = ps->rank;

    while (1) {
        MPI_Status st;
        int flag;
        // Check if we have any messages
        MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &flag, &st);
        if (!flag) {
            // sleep for 1ms
            usleep(1000);
            continue;
        }

        int src = st.MPI_SOURCE;
        int tag = st.MPI_TAG;

        if (tag == MSG_CHUNK_REQUEST) {
            RequestedChunk rc;
            MPI_Recv(&rc, sizeof(rc), MPI_CHAR, src, MSG_CHUNK_REQUEST, MPI_COMM_WORLD, &st);

            // Since we are accessing the wanted files we need to protect them
            // So we don't have the case where we are reading from them while we
            // are writing to them
            pthread_mutex_lock(&ps->lockWanted);
            // double now = get_time();
            // printf("[%.4f] [Peer %d UPLOAD] => [Peer %d] CHUNK_REQUEST %s#%d\n",
            //        now, rank, src, rc.filename, rc.chunk_index);

            int have_it = 0;
            // in owned_files => Seed so we have all the chunks => check by filename
            for (int i = 0; i < ps->owned_files_count; i++) {
                if (strcmp(rc.filename, ps->owned_files[i].filename) == 0) {
                    have_it = 1;
                    break;
                }
            }
            // in wanted_files => partial
            if (!have_it) {
                for (int i = 0; i < ps->wanted_files_count; i++) {
                    if (strcmp(rc.filename, ps->wanted_files[i].filename) == 0) {
                        if (ps->wanted_files[i].have_chunk[rc.chunk_index] == 1) {
                            have_it = 1;
                        }
                        break;
                    }
                }
            }
            // now = get_time();
            // Send ACK/NACK to the client with a little struct
            // (I know in the receving function I used a buffer, but I wanted to
            // have clarity when sending the ACK/NACK)
            if (have_it) {
                struct {
                    int chunk_idx;
                } ack;
                ack.chunk_idx = rc.chunk_index;
                // printf("[%.4f] [Peer %d UPLOAD] => [Peer %d] ACK chunk %d of %s\n",
                //        now, rank, src, rc.chunk_index, rc.filename);
                // Trimitem EXACT sizeof(ack) bytes
                MPI_Send(&ack, sizeof(ack), MPI_CHAR, src, MSG_SEND_ACK, MPI_COMM_WORLD);
            } else {
                // printf("[%.4f] [Peer %d UPLOAD] => [Peer %d] NACK chunk %d of %s\n",
                //        now, rank, src, rc.chunk_index, rc.filename);
                struct {
                    int chunk_idx;
                } nack;
                nack.chunk_idx = rc.chunk_index;
                MPI_Send(&nack, sizeof(nack), MPI_CHAR, src, MSG_SEND_NACK, MPI_COMM_WORLD);
            }
            pthread_mutex_unlock(&ps->lockWanted);
        } else if (tag == MSG_STOP_ALL) {
            // We received the signal from the tracker to stop all uploads => close the upload thread
            MPI_Recv(NULL, 0, MPI_CHAR, src, MSG_STOP_ALL, MPI_COMM_WORLD, &st);
            // printf("[Peer %d] STOP_ALL received from tracker -> closing upload.\n", rank);
            break;
        }
    }
    pthread_exit(NULL);
}

// Have 2 options:
// 1. Put a seed in the global tracker along with all the info
// 2. Mark a peer to a certain file
void add_file_to_global_tracker(const char *fname, int client_rank, int num_chunks,
                                char chunk_hashes[MAX_CHUNKS][HASH_SIZE + 1]) {
    int found = -1;
    for (int i = 0; i < g_num_tracked_files; i++) {
        if (strcmp(g_tracked_files[i].filename, fname) == 0) {
            found = i;
            break;
        }
    }

    if (found == -1) {
        found = g_num_tracked_files++;
        strcpy(g_tracked_files[found].filename, fname);
        g_tracked_files[found].swarm_size = 0;
        g_tracked_files[found].num_chunks = 0;
    }

    int s = g_tracked_files[found].swarm_size;
    int already_in_swarm = 0;
    for (int k = 0; k < s; k++) {
        if (g_tracked_files[found].swarm_members[k] == client_rank) {
            already_in_swarm = 1;
            break;
        }
    }
    // To avoid duplicates in the swarm
    if (!already_in_swarm) {
        g_tracked_files[found].swarm_members[s] = client_rank;
        g_tracked_files[found].swarm_size++;
    }

    // We only add the chunks if we are a seed
    if (num_chunks > 0 && chunk_hashes != NULL) {
        g_tracked_files[found].num_chunks = num_chunks;
        for (int c = 0; c < num_chunks; c++) {
            strcpy(g_tracked_files[found].chunk_hashes[c], chunk_hashes[c]);
        }
    }
}

// TRACKER
void tracker(int numtasks, int rank) {
    int total = numtasks - 1;
    int count_init = 0;
    int finished_peers = 0;
    int total_peers = numtasks - 1;

    while (count_init < total) {
        MPI_Status st;
        // printf("[%.4f] [Tracker] waiting MSG_INIT_FILES...\n", get_time());
        MPI_Probe(MPI_ANY_SOURCE, MSG_INIT_FILES, MPI_COMM_WORLD, &st);

        int src = st.MPI_SOURCE;
        int sz;
        MPI_Get_count(&st, MPI_CHAR, &sz);
        char *buf = malloc(sz);
        MPI_Recv(buf, sz, MPI_CHAR, src, MSG_INIT_FILES, MPI_COMM_WORLD, &st);

        int off = 0;
        int c_owned;
        memcpy(&c_owned, buf + off, sizeof(int));
        off += sizeof(int);

        for (int i = 0; i < c_owned; i++) {
            char fname[MAX_FILENAME];
            memcpy(fname, buf + off, MAX_FILENAME);
            off += MAX_FILENAME;
            int n;
            memcpy(&n, buf + off, sizeof(int));
            off += sizeof(int);

            char tmp[MAX_CHUNKS][HASH_SIZE + 1];
            for (int c = 0; c < n; c++) {
                memcpy(tmp[c], buf + off, HASH_SIZE + 1);
                off += (HASH_SIZE + 1);
            }
            add_file_to_global_tracker(fname, src, n, tmp);
        }

        free(buf);
        count_init++;
    }
    // Tell all peers to start downloading
    for (int c = 1; c < numtasks; c++) {
        MPI_Send(NULL, 0, MPI_CHAR, c, MSG_INIT_ACK, MPI_COMM_WORLD);
    }

    // printf("[%.4f] [Tracker] toti au trimis info. Start main loop.\n", get_time());
    // for (int i = 0; i < g_num_tracked_files; i++) {
    //     printf("[%.4f] [Tracker] %s => chunks=%d, swarm_size=%d\n",
    //            get_time(), g_tracked_files[i].filename,
    //            g_tracked_files[i].num_chunks,
    //            g_tracked_files[i].swarm_size);
    // }

    while (1) {
        // Check for messages
        MPI_Status st;
        // MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &st);
        int flag;
        // Check if we have any messages
        MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &flag, &st);
        if (!flag) {
            // sleep for 1ms
            usleep(1000);
            continue;
        }

        int src = st.MPI_SOURCE;
        int tag = st.MPI_TAG;

        if (tag == MSG_REQUEST_SWARM) {
            char fname[MAX_FILENAME];
            MPI_Recv(fname, MAX_FILENAME, MPI_CHAR, src, MSG_REQUEST_SWARM, MPI_COMM_WORLD, &st);
            // printf("[%.4f] [Tracker] MSG_REQUEST_SWARM for %s from %d\n",
            //        get_time(), fname, src);

            int found = -1;
            for (int i = 0; i < g_num_tracked_files; i++) {
                if (strcmp(g_tracked_files[i].filename, fname) == 0) {
                    found = i;
                    break;
                }
            }
            // If the file is not known by the tracker
            if (found == -1) {
                // Send 0 to signal that the file is not known
                int zero = 0;
                MPI_Send(&zero, 1, MPI_INT, src, MSG_SWARM_INFO, MPI_COMM_WORLD);
            } else {
                // Send the swarm info to the client for the first time
                // Order: num_chunks, chunk_hashes, swarm_size, swarm_members
                int n = g_tracked_files[found].num_chunks;
                int s = g_tracked_files[found].swarm_size;
                MPI_Send(&n, 1, MPI_INT, src, MSG_SWARM_INFO, MPI_COMM_WORLD);
                for (int c = 0; c < n; c++) {
                    MPI_Send(g_tracked_files[found].chunk_hashes[c], HASH_SIZE + 1, MPI_CHAR,
                             src, MSG_SWARM_INFO, MPI_COMM_WORLD);
                }
                MPI_Send(&s, 1, MPI_INT, src, MSG_SWARM_INFO, MPI_COMM_WORLD);
                for (int i = 0; i < s; i++) {
                    int pr = g_tracked_files[found].swarm_members[i];
                    MPI_Send(&pr, 1, MPI_INT, src, MSG_SWARM_INFO, MPI_COMM_WORLD);
                }
                // Add the client to the swarm (it's a peer)
                // I won't differentiate between seeds and peers in the swarm
                // I will just add the peer to the swarm and the clients will
                // have to deal with it
                add_file_to_global_tracker(fname, src, 0, NULL);
            }
        } else if (tag == MSG_REFRESH_SWARM) {
            // A peer wants to refresh the swarm for a file => send the swarm
            // members
            // Same as MSG_REQUEST_SWARM, but we don't send the chunks
            char fname[MAX_FILENAME];
            MPI_Recv(fname, MAX_FILENAME, MPI_CHAR, src, MSG_REFRESH_SWARM, MPI_COMM_WORLD, &st);
            // printf("[%.4f] [Tracker] MSG_REFRESH_SWARM for %s from %d\n", get_time(), fname, src);

            int found = -1;
            for (int i = 0; i < g_num_tracked_files; i++) {
                if (strcmp(g_tracked_files[i].filename, fname) == 0) {
                    found = i;
                    break;
                }
            }
            if (found == -1) {
                int zero = 0;
                MPI_Send(&zero, 1, MPI_INT, src, MSG_SWARM_LIST, MPI_COMM_WORLD);
            } else {
                int s = g_tracked_files[found].swarm_size;
                MPI_Send(&s, 1, MPI_INT, src, MSG_SWARM_LIST, MPI_COMM_WORLD);
                for (int i = 0; i < s; i++) {
                    int pr = g_tracked_files[found].swarm_members[i];
                    MPI_Send(&pr, 1, MPI_INT, src, MSG_SWARM_LIST, MPI_COMM_WORLD);
                }
                // Should already be in the swarm, but just in case
                add_file_to_global_tracker(fname, src, 0, NULL);
            }
        } else if (tag == MSG_FILE_COMPLETE) {
            // Client -> Tracker : "I have finished downloading this file"
            char fname[MAX_FILENAME];
            MPI_Recv(fname, MAX_FILENAME, MPI_CHAR, src, MSG_FILE_COMPLETE, MPI_COMM_WORLD, &st);
            // printf("[%.4f] [Tracker] MSG_FILE_COMPLETE from %d for %s\n", get_time(), src, fname);
        } else if (tag == MSG_FINISHED_ALL) {
            // Client -> Tracker : "I have finished downloading all the files I wanted"
            MPI_Recv(NULL, 0, MPI_CHAR, src, MSG_FINISHED_ALL, MPI_COMM_WORLD, &st);
            finished_peers++;
            // printf("[Tracker] Peer %d has finished all. finished_peers=%d\n", src, finished_peers);

            // Check if all peers have finished
            if (finished_peers == total_peers) {
                // Stop all activity
                for (int c = 1; c < numtasks; c++) {
                    MPI_Send(NULL, 0, MPI_CHAR, c, MSG_STOP_ALL, MPI_COMM_WORLD);
                }
                break;
            }
        }
    }
    // printf("[%.4f] [Tracker] All peers have finished. Exiting.\n", get_time());
}

void *download_thread_func(void *);
void *upload_thread_func(void *);

void peer(int numtasks, int rank) {
    PeerState *ps = calloc(1, sizeof(PeerState));
    ps->rank = rank;
    ps->numtasks = numtasks;
    pthread_mutex_init(&ps->lockWanted, NULL);

    pthread_t download_thread;
    pthread_t upload_thread;

    int r = pthread_create(&download_thread, NULL, download_thread_func, (void *) ps);
    if (r) {
        exit(-1);
    }

    r = pthread_create(&upload_thread, NULL, upload_thread_func, (void *) ps);
    if (r) {
        exit(-1);
    }

    r = pthread_join(download_thread, NULL);
    if (r) {
        exit(-1);
    }

    r = pthread_join(upload_thread, NULL);
    if (r) {
        exit(-1);
    }

    free(ps);
}

int main(int argc, char *argv[]) {
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE) {
        fprintf(stderr, "MPI nu suporta THREAD_MULTIPLE!\n");
        exit(-1);
    }
    int numtasks, rank;
    MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == TRACKER_RANK) {
        tracker(numtasks, rank);
    } else {
        peer(numtasks, rank);
    }

    MPI_Finalize();
    return 0;
}
