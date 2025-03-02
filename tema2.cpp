#include <mpi.h>
#include <cstring>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100
#define RESPONSE_SIZE 8

using namespace std;

struct seed_file
{
    string filename;
    int chunks;
    vector<string> hash;
};

struct client_info
{
    int rank;
    bool has_fragments;
};

struct tracker_data
{
    vector<seed_file> files;
    map<string, vector<client_info>> clients;
};

struct thread_data
{
    int rank;
    int num_files_owned;
    vector<seed_file> files_owned;
    int num_files_wanted;
    vector<string> files_wanted;
    map<string, vector<bool>> downloading_files;
    map<string, vector<string>> tracker_files;
};

enum MessageType
{
    INIT_SEEDER = 0,
    INIT_ACK = 1,
    REQUEST_SWARM = 2,
    SWARM_RESPONSE = 3,
    REQUEST_SEGMENT = 4,
    SEGMENT_RESPONSE = 5,
    FILE_COMPLETE = 6,
    REQUEST_FILE_INFO = 7,
    FILE_INFO_RESPONSE = 8,
    SEGMENT_CHECK_NAME = 9,
    SEGMENT_CHECK_RESPONSE = 10,
    SEGMENT_CHECK_HASH = 11,
    FILES_DOWNLOADED_COMPLETE = 12,
    ALL_DONE = 13
};

int get_file_chunks(string filename, int rank)
{
    MPI_Send(filename.c_str(), MAX_FILENAME, MPI_CHAR, TRACKER_RANK, REQUEST_FILE_INFO, MPI_COMM_WORLD);

    int chunks;
    MPI_Recv(&chunks, 1, MPI_INT, TRACKER_RANK, FILE_INFO_RESPONSE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    return chunks;
}

bool is_file_complete(string filename, thread_data *data)
{
    vector<bool> &chunks = data->downloading_files[filename];
    for (bool chunk : chunks)
    {
        if (!chunk)
        {
            return false;
        }
    }
    return true;
}

void savefile(string filename, thread_data *data)
{
    string output_file = "client" + to_string(data->rank) + "_" + filename;
    ofstream fout(output_file);
    if (!fout.is_open())
    {
        printf("Eroare la deschiderea fisierului de output\n");
        exit(-1);
    }

    for (const auto &hash : data->tracker_files[filename])
    {
        fout << hash << endl;
    }

    fout.close();
}

void *download_thread_func(void *arg)
{
    thread_data *data = (thread_data *)arg;

    for (int i = 0; i < data->num_files_wanted; i++)
    {
        int downloaded_segments = 0;
        string filename = data->files_wanted[i];
        data->tracker_files[filename] = vector<string>();
        int chunks = get_file_chunks(filename, data->rank);
        data->downloading_files[filename] = vector<bool>(chunks, false);

        MPI_Send(filename.c_str(), MAX_FILENAME, MPI_CHAR, TRACKER_RANK, REQUEST_SWARM, MPI_COMM_WORLD);

        int num_peers;
        MPI_Recv(&num_peers, 1, MPI_INT, TRACKER_RANK, SWARM_RESPONSE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        vector<int> peers(num_peers);
        MPI_Recv(peers.data(), num_peers, MPI_INT, TRACKER_RANK, SWARM_RESPONSE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        MPI_Send(filename.c_str(), MAX_FILENAME, MPI_CHAR, TRACKER_RANK, REQUEST_SEGMENT, MPI_COMM_WORLD);

        for (int j = 0; j < chunks; j++)
        {
            char hash[HASH_SIZE];
            MPI_Recv(hash, HASH_SIZE, MPI_CHAR, TRACKER_RANK, SEGMENT_RESPONSE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            data->tracker_files[filename].push_back(string(hash));
        }

        int peer_index = 0;
        int peer_rank = peers[peer_index];

        while (downloaded_segments < chunks)
        {
            MPI_Status status;
            MPI_Send(filename.c_str(), MAX_FILENAME, MPI_CHAR, peer_rank, SEGMENT_CHECK_NAME, MPI_COMM_WORLD);

            MPI_Send(&downloaded_segments, 1, MPI_INT, peer_rank, SEGMENT_CHECK_HASH, MPI_COMM_WORLD);

            char response[RESPONSE_SIZE];
            MPI_Recv(response, RESPONSE_SIZE, MPI_CHAR, peer_rank, SEGMENT_CHECK_RESPONSE, MPI_COMM_WORLD, &status);

            if (strcmp(response, "D") == 0)
            {
                data->downloading_files[filename][downloaded_segments] = true;
                downloaded_segments++;

                // Dupa ce primeste primul segment, este adaugat in swarm
                if (downloaded_segments == 1)
                {
                    MPI_Send(filename.c_str(), MAX_FILENAME, MPI_CHAR, TRACKER_RANK, REQUEST_SWARM, MPI_COMM_WORLD);
                    int num_peers;
                    MPI_Recv(&num_peers, 1, MPI_INT, TRACKER_RANK, SWARM_RESPONSE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                    vector<int> peers(num_peers);
                    MPI_Recv(peers.data(), num_peers, MPI_INT, TRACKER_RANK, SWARM_RESPONSE, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                }
            }
            peer_index = (peer_index + 1) % num_peers;
            peer_rank = peers[peer_index];
        }
        if (is_file_complete(filename, data))
        {
            MPI_Send(filename.c_str(), MAX_FILENAME, MPI_CHAR, TRACKER_RANK, FILE_COMPLETE, MPI_COMM_WORLD);
            savefile(filename, data);
            // Adaug fisierul in files_owned pentru a putea raspunde la cereri
            seed_file new_file;
            new_file.filename = filename;
            new_file.chunks = chunks;
            new_file.hash = data->tracker_files[filename];
            data->files_owned.push_back(new_file);
        }
    }

    MPI_Send(&data->rank, 1, MPI_INT, TRACKER_RANK, FILES_DOWNLOADED_COMPLETE, MPI_COMM_WORLD);
    pthread_exit(NULL);

    return NULL;
}

void *upload_thread_func(void *arg)
{
    thread_data *data = (thread_data *)arg;
    MPI_Status status;

    while (1)
    {
        MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        if (status.MPI_TAG == ALL_DONE)
        {
            int all_done;
            MPI_Recv(&all_done, 1, MPI_INT, MPI_ANY_SOURCE, ALL_DONE, MPI_COMM_WORLD, &status);
            break;
        }
        if (status.MPI_TAG == SEGMENT_CHECK_NAME)
        {
            char filename[MAX_FILENAME];
            MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, MPI_ANY_SOURCE, SEGMENT_CHECK_NAME, MPI_COMM_WORLD, &status);

            int segment;
            MPI_Recv(&segment, 1, MPI_INT, MPI_ANY_SOURCE, SEGMENT_CHECK_HASH, MPI_COMM_WORLD, &status);

            int gasit = 0;
            string filename_str = string(filename);

            char response[RESPONSE_SIZE];
            memset(response, 0, RESPONSE_SIZE);

            for (const auto &file : data->files_owned)
            {
                if (file.filename == filename_str)
                {
                    if (segment >= 0 && segment < file.chunks)
                    {
                        gasit = 1;
                        break;
                    }
                }
            }

            strcpy(response, gasit ? "D" : "N");
            MPI_Send(response, RESPONSE_SIZE, MPI_CHAR, status.MPI_SOURCE, SEGMENT_CHECK_RESPONSE, MPI_COMM_WORLD);
        }
    }

    return NULL;
}

void tracker(int numtasks, int rank)
{
    tracker_data data;
    MPI_Status status;
    int counter_clients_done = 0;
    vector<bool> clients_finished(numtasks, false);

    for (int i = 1; i < numtasks; i++)
    {
        int num_files;
        MPI_Recv(&num_files, 1, MPI_INT, i, INIT_SEEDER, MPI_COMM_WORLD, &status);

        for (int j = 0; j < num_files; j++)
        {
            seed_file file;
            char filename[MAX_FILENAME];
            int chunks;
            MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, i, INIT_SEEDER, MPI_COMM_WORLD, &status);
            MPI_Recv(&chunks, 1, MPI_INT, i, INIT_SEEDER, MPI_COMM_WORLD, &status);

            file.filename = string(filename);
            file.chunks = chunks;

            for (int k = 0; k < chunks; k++)
            {
                char hash_str[HASH_SIZE + 1];
                MPI_Recv(hash_str, HASH_SIZE, MPI_CHAR, i, INIT_SEEDER, MPI_COMM_WORLD, &status);
                file.hash.push_back(string(hash_str));
            }

            data.files.push_back(file);
            client_info client = {i, true};
            data.clients[file.filename].push_back(client);
        }

        char ack[RESPONSE_SIZE];
        memset(ack, 0, RESPONSE_SIZE);
        strcpy(ack, "ACK");
        MPI_Send(ack, RESPONSE_SIZE, MPI_CHAR, i, INIT_ACK, MPI_COMM_WORLD);
    }

    while (1)
    {
        MPI_Status status;
        MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);

        if (status.MPI_TAG == FILES_DOWNLOADED_COMPLETE)
        {
            int client_rank;
            MPI_Recv(&client_rank, 1, MPI_INT, MPI_ANY_SOURCE, FILES_DOWNLOADED_COMPLETE, MPI_COMM_WORLD, &status);
            if (!clients_finished[client_rank])
            {
                clients_finished[client_rank] = true;
                counter_clients_done++;
                if (counter_clients_done == numtasks - 1)
                {
                    int all_done = 1;
                    for (int i = 1; i < numtasks; i++)
                    {
                        MPI_Send(&all_done, 1, MPI_INT, i, ALL_DONE, MPI_COMM_WORLD);
                    }
                    break;
                }
            }
        }
        else if (status.MPI_TAG == REQUEST_SWARM)
        {
            char filename[MAX_FILENAME];
            MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, MPI_ANY_SOURCE, REQUEST_SWARM, MPI_COMM_WORLD, &status);

            string filename_str = string(filename);
            vector<client_info> &clients = data.clients[filename_str];

            // Verificam daca clientul exista deja in swarm
            bool client_exists = false;
            for (auto &client : clients)
            {
                if (client.rank == status.MPI_SOURCE)
                {
                    client.has_fragments = true; // Actualizam ca are fragmente
                    client_exists = true;
                    break;
                }
            }

            // Daca clientul nu exista in swarm, il adaugam ca peer
            if (!client_exists)
            {
                client_info new_client = {status.MPI_SOURCE, true};
                clients.push_back(new_client);
            }

            // Trimitem lista actualizata de peers
            int num_peers = clients.size();
            vector<int> peers(num_peers);
            for (int i = 0; i < num_peers; i++)
            {
                peers[i] = clients[i].rank;
            }
            MPI_Send(&num_peers, 1, MPI_INT, status.MPI_SOURCE, SWARM_RESPONSE, MPI_COMM_WORLD);
            MPI_Send(peers.data(), num_peers, MPI_INT, status.MPI_SOURCE, SWARM_RESPONSE, MPI_COMM_WORLD);
        }
        else if (status.MPI_TAG == REQUEST_FILE_INFO)
        {
            char filename[MAX_FILENAME];
            MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, MPI_ANY_SOURCE, REQUEST_FILE_INFO, MPI_COMM_WORLD, &status);

            int chunks = 0;
            for (const auto &file : data.files)
            {
                if (file.filename == string(filename))
                {
                    chunks = file.chunks;
                    break;
                }
            }
            MPI_Send(&chunks, 1, MPI_INT, status.MPI_SOURCE, FILE_INFO_RESPONSE, MPI_COMM_WORLD);
        }
        else if (status.MPI_TAG == REQUEST_SEGMENT)
        {
            char filename[MAX_FILENAME];
            MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, MPI_ANY_SOURCE, REQUEST_SEGMENT, MPI_COMM_WORLD, &status);

            for (const auto &file : data.files)
            {
                if (file.filename == string(filename))
                {
                    for (auto &chunk : file.hash)
                    {
                        MPI_Send(chunk.c_str(), HASH_SIZE, MPI_CHAR, status.MPI_SOURCE, SEGMENT_RESPONSE, MPI_COMM_WORLD);
                    }
                    break;
                }
            }
        }
        else if (status.MPI_TAG == FILE_COMPLETE)
        {
            char filename[MAX_FILENAME];
            MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, MPI_ANY_SOURCE, FILE_COMPLETE, MPI_COMM_WORLD, &status);
            string completed_file = string(filename);

            bool client_exists = false;
            for (auto &client : data.clients[completed_file])
            {
                if (client.rank == status.MPI_SOURCE)
                {
                    client.has_fragments = true;
                    client_exists = true;
                    break;
                }
            }

            if (!client_exists)
            {
                client_info new_client = {status.MPI_SOURCE, true};
                data.clients[completed_file].push_back(new_client);
            }
        }
    }
}

void peer(int numtasks, int rank)
{
    // Citirea fisierelor
    // string input_file = "in" + to_string(rank) + ".txt"; // pentru rulare cu checker
    string input_file = "../checker/tests/test1/in" + to_string(rank) + ".txt"; // pentru rulare locala
    ifstream fin(input_file);
    if (!fin.is_open())
    {
        printf("Eroare la deschiderea fisierului de input\n");
        exit(-1);
    }

    int num_files_owned;
    fin >> num_files_owned;
    vector<seed_file> files_owned(num_files_owned);

    for (int i = 0; i < num_files_owned; i++)
    {
        fin >> files_owned[i].filename;
        fin >> files_owned[i].chunks;
        for (int j = 0; j < files_owned[i].chunks; j++)
        {
            string hash;
            fin >> hash;
            files_owned[i].hash.push_back(hash);
        }
    }

    int num_files_wanted;
    fin >> num_files_wanted;
    vector<string> files_wanted(num_files_wanted);
    for (int i = 0; i < num_files_wanted; i++)
    {
        fin >> files_wanted[i];
    }

    // TRIMITERE INFORMATII CATRE TRACKER

    MPI_Send(&num_files_owned, 1, MPI_INT, TRACKER_RANK, INIT_SEEDER, MPI_COMM_WORLD);

    for (const auto &file : files_owned)
    {
        char filename[MAX_FILENAME];
        strncpy(filename, file.filename.c_str(), MAX_FILENAME);
        MPI_Send(filename, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, INIT_SEEDER, MPI_COMM_WORLD);

        MPI_Send(&file.chunks, 1, MPI_INT, TRACKER_RANK, INIT_SEEDER, MPI_COMM_WORLD);

        for (const auto &hash : file.hash)
        {
            char hash_str[HASH_SIZE + 1];
            strncpy(hash_str, hash.c_str(), HASH_SIZE);
            MPI_Send(hash_str, HASH_SIZE, MPI_CHAR, TRACKER_RANK, INIT_SEEDER, MPI_COMM_WORLD);
        }
    }
    char ack[RESPONSE_SIZE];
    MPI_Recv(ack, RESPONSE_SIZE, MPI_CHAR, TRACKER_RANK, INIT_ACK, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    ack[RESPONSE_SIZE - 1] = '\0';
    if (strcmp(ack, "ACK") != 0)
    {
        printf("Eroare la primirea ACK de la tracker\n");
        exit(-1);
    }
    /////////////////////////////////////////////////////////

    thread_data data;
    data.rank = rank;
    data.num_files_owned = num_files_owned;
    data.files_owned = files_owned;
    data.num_files_wanted = num_files_wanted;
    data.files_wanted = files_wanted;

    pthread_t download_thread;
    pthread_t upload_thread;
    void *status;
    int r;

    r = pthread_create(&download_thread, NULL, download_thread_func, (void *)&data);
    if (r)
    {
        printf("Eroare la crearea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_create(&upload_thread, NULL, upload_thread_func, (void *)&data);
    if (r)
    {
        printf("Eroare la crearea thread-ului de upload\n");
        exit(-1);
    }

    r = pthread_join(download_thread, &status);
    if (r)
    {
        printf("Eroare la asteptarea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_join(upload_thread, &status);
    if (r)
    {
        printf("Eroare la asteptarea thread-ului de upload\n");
        exit(-1);
    }
}

int main(int argc, char *argv[])
{
    int numtasks, rank;

    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE)
    {
        fprintf(stderr, "MPI nu are suport pentru multi-threading\n");
        exit(-1);
    }
    MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == TRACKER_RANK)
    {
        tracker(numtasks, rank);
    }
    else
    {
        peer(numtasks, rank);
    }

    MPI_Finalize();
}
