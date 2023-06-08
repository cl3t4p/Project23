#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <errno.h>

#include "errExit.h"
#include "commands.h"
#include "f4logic.h"
#include "ioutils.h"


static volatile int active = 1;


void intHandler(int dummy) {
    active = 0;
}

int init_fifo(char name[]) {
    if (mkfifo(name, S_IRUSR | S_IWUSR) != 0) {
        printf("There is already an instance of f4server running\n");
        printf("If you want to force start please use -f as last argument\n");
        exit(1);
    }
    //TODO Trying without O_NONBLOCK
    return open(name, O_RDONLY);
}


int main(int argc, char *argv[]) {


    // controlla il numero di argomenti di argc
    // esecuzione del server con numero minore di parametri (o 0) comporta la stampa di un messaggio di aiuto
    if (argc < 5) {
        printf("Usage: %s <n_row> <n_column> sym_1 sym_2\n", argv[0]);
        return 0;
    }
    //-f it remove the old pipe file before running the server
    if (argc == 6) {
        if (argv[5] == "-f") {
            printf("Unlinking old file\n");
            unlink(DEFAULT_PATH);
        }
    }

    // trasformiamo argv1 e 2 in interi
    int row = atoi(argv[1]);
    int column = atoi(argv[2]);

    //controllo validitá della matrice
    if (row < 5) {
        printf("The input row must be >= 5\n");
        return 1;
    }
    if (column < 5) {
        printf("The input column must be < 5\n");
        return 1;
    }

    //inizializziamo i simboli usati dai due giocatori che devono essere diversi tra di loro
    char symbols[2];
    if (*argv[3] == *argv[4]) {
        printf("Can't use same symbols\n");
        return 1;
    }
    symbols[0] = *argv[3];
    symbols[1] = *argv[4];

    //Creation FIFO for init connection
    //No need to have sem has if the read or write is below PIPE_BUF then you can have multiple reader and writer
    //https://www.gnu.org/software/libc/manual/html_node/Pipe-Atomicity.html
    //Min size for PIPE_BUF is 512 byte


    signal(SIGINT, intHandler); // Read CTRL+C and stop the program
    signal(SIGTERM, intHandler); // Read CTRL+C and stop the program


    //Creation default dir
    if(mkdir(DEFAULT_DIR,0777) == -1){
        errExit("error creating : ");
    }



    //Create fifo for connecting
    int fd_fifo_first_input = init_fifo(DEFAULT_PATH);


    //pid_t games[1024];
    int n_game = 0;
    struct client_info clients[2];
    int queue_size = 0;
    while (1) {
        //Read a client info from the FIFO
        struct client_info buffer;
        ssize_t n = read(fd_fifo_first_input, &buffer, sizeof(struct client_info));
        if (n > 0) {
            printf("Client connecting with :\n");
            printf("pid :%d\n", buffer.pid);
            printf("key id : %d\n", buffer.fifo_fd);
            printf("mode : %c\n", buffer.mode);

            if (buffer.mode == '*') {
                //Single logic here
            } else {

                clients[queue_size] = buffer;
                queue_size++;
                if (queue_size == 2) {
                    queue_size = 0;
                    //Create fork and pass logic

                    if (!is_alive(clients[0].pid)) {
                        printf("The process %d is not alive\n", clients[0].pid);
                        clients[0] = clients[1];
                        queue_size = 1;
                        continue;
                    }

                    if (!is_alive(clients[1].pid)) {
                        printf("The process %d is not alive\n", clients[1].pid);
                        queue_size = 1;
                        continue;
                    }


                    if (fork() == 0) {
                        //TODO store child and terminate them.
                        break;
                    }
                    n_game++;
                }
            }
        }

        //Sopra tutto giusto
        if (active == 0) {
            printf("Stopping server\n");
            close(fd_fifo_first_input);
            unlink(DEFAULT_PATH);
            remove_dir(DEFAULT_DIR);
            //Wait for every child to terminate
            pid_t child;
            int status;
            //TODO Fix child termination
            while ((child = waitpid(0, &status, 0)) != -1) {
                printf("Child %d has been terminated\n", child);
            }
            //Close fifo


            return 1;
        }
    }

    //Child
    printf("Starting game number %d\n", n_game);

    //Send the symbols to the clients
    //Passiamo al primo client il primo simbolo



    for (int i = 0; i < 2; ++i){
        clients[i].fifo_fd = cmd_mkfifo(clients[i].pid,DEFAULT_DIR,O_WRONLY);

    }
    perror("error ");


    cmd_send(clients[0], CMD_SET_SYMBOL, &symbols[0]);
    perror("error ");
    errno = 0;
    cmd_send(clients[1], CMD_SET_SYMBOL, &symbols[1]);
    perror("error ");

    //Broadcast input queue id unico
    key_t key_msg_qq_input = ftok(DEFAULT_PATH, getpid()); //key dei client
    cmd_broadcast(clients, CMD_SET_MSG_QQ_ID, &key_msg_qq_input);

    //Calculating the size of the shared memory
    struct shared_mem_info shm_mem_inf;
    shm_mem_inf.row = row;
    shm_mem_inf.column = column;

    //Key for the shared memory and the semaphore
    shm_mem_inf.key = ftok(".", getpid()); //key matrice




    //Creation of the semaphore
    int shm_mem_sem_id = semget(shm_mem_inf.key, 2, IPC_CREAT);

    union semun sem_val;
    sem_val.val = 0;
    //Set semaphore to 1
    semctl(shm_mem_inf.key, 0, SETALL, sem_val);




    //Create shared memory
    int shm_mem_id =
            shmget(shm_mem_inf.key, row * column * sizeof(pid_t), IPC_CREAT | IPC_CREAT | S_IRUSR | S_IWUSR) == -1;
    if (shm_mem_id) {
        //TODO handle error if there is multiple shared_memory (should be impossible)
    }
    pid_t *matrix = shmat(shm_mem_inf.key, NULL, 0);

    //Fill the array with 0
    clean_array(matrix, row, column); //resetta la matrice

    //Broadcast info of shared memory to clients
    cmd_broadcast(clients, CMD_SET_SH_MEM, &shm_mem_inf);

    //Unlock semaphore
    sem_val.val = 2;
    semctl(shm_mem_inf.key, 0, SETALL, sem_val);


    //Tell the client to update their internal shared memory
    // legge da memoria condivisa CMD_UPDATE
    cmd_broadcast(clients, CMD_UPDATE, NULL);




    //Dopo aver conneso d
    //Set the turn to the first player
    int turn_num = 0; //turno primo client
    struct client_info player = cmd_turn(clients, turn_num); //giocatore che sta facendo la mossa
    struct msg_buffer buffer;
    //Main game loop
    while (active) {
        //Read incoming msg queue non blocking
        if (msgrcv(key_msg_qq_input, &buffer, CMD_CLI_ACTION, CMD_CLI_ACTION / 100, MSG_NOERROR | IPC_NOWAIT) > 0) {
            //Read the action
            struct client_action action = *(struct client_action *) buffer.msg;
            //Check if a player has abandon the game
            if (action.column == -1) {
                //TODO Abandon logic
            }
            //Check if sender is the player
            if (action.pid == player.pid) {
                //Play the move
                pid_t result = f4_play(matrix, column, action.pid, column, row);
                if (result == -1) {
                    //Wrong input
                    cmd_send(player, CMD_INPUT_ERROR, NULL);
                } else if (result == player.pid) {
                    //Winner
                    cmd_broadcast(clients, CMD_WINNER, NULL);
                    active = 0;
                }
                turn_num = !turn_num;



                //TODO Unlock the semaphore
                sem_val.val = 2;
                semctl(shm_mem_inf.key, 0, SETALL, sem_val);
                //Send update command
                cmd_broadcast(clients, CMD_UPDATE, NULL);


                player = cmd_turn(clients, turn_num);

            }
        }
    }

    //Clean e chiusura partita
    //Removing shared memory
    if (shmdt(matrix) == -1) {
        //TODO Handling error ?
    }
    if (shmctl(shm_mem_id, IPC_RMID, NULL) == -1) {
        //TODO Handling error ?
    }

    //Removing semaphore
    if (semctl(shm_mem_sem_id, IPC_RMID, 0) == -1) {
        //TODO Handling error ?
    }

    //Close all pipes
    for (int i = 0; i < 2; ++i) {
        msgctl(clients[i].fifo_fd, IPC_RMID, NULL);
    }
}