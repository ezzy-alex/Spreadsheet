/* 
 * File:   main.c
 * Author: Kathilee Ledgister
 *
 * Created on April 4, 2021, 11:53 AM
 */

/* Extension from POSIX.1:2001. 
Structure to contain information about address of a service provider.  */
#define _POSIX_C_SOURCE 200112L // MUST be decleared
#define _GNU_SOURCE


#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>

/**
 * BE SURE TO COMPILE WITH AND LINK WITH :  "-pthread" link and compile option
 */

/*
 * Two user defined signals we use to communicate internally
 */
#define RECYCLE_TIMEOUT             10 // 10 seconds
#define SERVERHOSTNAME        NULL //"localhost"
#define SERVERPORT            "20020"        

volatile int gbContinueProcessingSpreadSheet = 1;
pthread_t pthreadHandler;
int firstClientSocket = -1;

/*
 * One user defined signals we use to communicate internally
 */
#define SIGNAL_SEND_SPREADSHEET (SIGRTMIN+1)

/**
 * floward declare list
 */
#define STR_CELL_SIZE   128
typedef struct _tag_linkedlist clientlist_t;

typedef struct _tag_linkedlist {
    int clientSocket;
    pthread_t pthreadClient;
    clientlist_t *next;
} clientlist_t;

typedef struct _tag_cell {
    int ival;
    char sval[STR_CELL_SIZE];
} cell_t;

clientlist_t *listTop = NULL;
pthread_mutex_t gmutex_SSheet;
/**
 * package the last spread sheet here for sending
 */
char strLastSpreadSheet[4096];

cell_t sheet[9][9];

int isvalidcellref(char *cellref) {
    int valid = 0;
    do {
        if (cellref == NULL)
            break;

        if (tolower(cellref[0]) < 'a' || tolower(cellref[0]) > 'i')
            break;

        if (cellref[1] < '1' || cellref[1] > '9')
            break;

    } while (0);
    return valid;
}

int formulatype(char *formula) {
    int ftype = 0;
    do {
        if (formula == NULL)
            break;

        if (!strcasecmp(&formula[3], "AVERAGE(")) {
            ftype = 1;
            break;
        }
        if (!strcasecmp(&formula[3], "RANGE(")) {
            ftype = 2;
            break;
        }

        if (!strcasecmp(&formula[3], "SUM(")) {
            ftype = 3;
            break;
        }
    } while (0);
    return ftype;
}

int isvalidformula(char *formula) {
    int valid = 0;
    int ft;
    int offset = 0;
    do {
        if (formula == NULL)
            break;

        if (!isvalidcellref(formula))
            break;

        if (formula[2] != '=')
            break;

        if ((ft = formulatype(formula)) == 0)
            break;
        switch (ft) {
            case 1:
                offset = 11;
                break;
            case 2:
                offset = 9;
                break;
            case 3:
                offset = 7;
                break;
        }
        if (!isvalidcellref(&formula[offset]))
            break;

        offset++;
        if (formula[offset] != ',')
            break;
        offset++;

        if (!isvalidcellref(&formula[offset]))
            break;

        offset += 2;
        if (formula[offset] != ')')
            break;

        offset++;
        if (formula[offset] != '\0')
            break;
    } while (0);
    return valid;
}

int isnumstr(char *str){
    int ret = 0;
    int i = 0;
    
    for(;isdigit(str[i]);i++);
    
    
    return ret;
}

int isvalidcelldata(char *celldata){
    int valid = 0;
    do{
        if(celldata==NULL)
            break;
        
        if(celldata[0]=='\''){
            valid = 1;
            break;
        }
        
        
    }while(0);
    return valid;
}
int sendall(int socket, const char *buf, unsigned int len, int flags) {
    int total = 0; // how many bytes we've sent
    int bytesleft = len; // how many we have left to send
    int n, rc = -1;
    fd_set writefds;
    fd_set exceptfds;
    struct timeval timeout;

    while (total < len) {
        FD_ZERO(&writefds);
        FD_SET(socket, &writefds);

        FD_ZERO(&exceptfds);
        FD_SET(socket, &exceptfds);

        timeout.tv_sec = 5; //0; if we don't receive more data in 30 seconds terminate
        timeout.tv_usec = 0;

        n = select(socket + 1, NULL, &writefds, &exceptfds, &timeout);
        switch (n) {
            case -1:
                switch (errno) {
                    case EINTR:
                        break;
                    default:
                        //goto send_error;?
                        break;
                }
                break;

            case 0: // timeout occurred
                // the connection may be slow
                break;

            default:
                if (FD_ISSET(socket, &exceptfds)) {
                    //goto send_error;?
                } else if (FD_ISSET(socket, &writefds)) {
                    n = send(socket, buf + total, bytesleft, flags);
                    if (n == -1) {
                        switch (errno) {
                            case EWOULDBLOCK:
                                break;
                            default:
                                //goto send_error;?
                                break;
                        }
                    } else if (n > 0) {
                        total += n;
                        bytesleft -= n;
                    }
                }
                break;
        }
    }

send_error:
    rc = bytesleft < 0 ? 0 : bytesleft;
    return rc; // return bytesleft: should be (0) on success
}

void notifyAllUsers(char *sheetCopy) {
    clientlist_t *list;
    int rc = 0;

    //-------------------------------------
    // TODO
    // notify all users in the linked list
    //-------------------------------------

    // for client in the linlked list
    for (list = listTop; list; list = list->next) {
        rc = sendall(list->clientSocket, sheetCopy, strlen(sheetCopy), 0);
        if (rc) {
            /**
             * for some reason all the data was not sent
             * Try resending the rest of the data
             * SANITY CHECK **************
             * CHECK: there could also be a bug that locks the program into this loop
             */
            while (rc) {
                rc = sendall(list->clientSocket, &sheetCopy[rc], strlen(&sheetCopy[rc]), 0);
            }
        }
    }
}

static void *sig_sendSheethandler() {
    sigset_t set;
    int s, sig;

    sigemptyset(&set);
    sigaddset(&set, SIGNAL_SEND_SPREADSHEET);

    for (;;) {
        sig = 0; // SANITY
        s = sigwait(&set, &sig);
        if (s == 0) {
            s = pthread_sigmask(SIG_BLOCK, &set, NULL);
            if (sig == SIGNAL_SEND_SPREADSHEET) {
                char sheetCopy[4096];
                do {
                    if (pthread_mutex_trylock(&gmutex_SSheet)) {
                        usleep(100000); // microseconds
                        // when you wake up
                        continue;
                    }
                    // make a copy of the sheet
                    strncpy(sheetCopy, strLastSpreadSheet, 4095);
                    pthread_mutex_unlock(&gmutex_SSheet);
                    break;
                } while (1);

                notifyAllUsers(sheetCopy);
            }
            s = pthread_sigmask(SIG_UNBLOCK, &set, NULL);
        }
    }
}

void updateSpreadSheet(char *data) {
    do {
        if (pthread_mutex_trylock(&gmutex_SSheet)) {
            usleep(100000); // microseconds
            // when you wake up
            continue;
        }

        //-------------------------------------
        // TODO
        // update the spread sheet

        // package the last spread sheet in this variable
        // while no other thread can change it
        // strLastSpreadSheet == last spread sheet

        // CREATE A DUMMY SPREADSHEET FOR TESTING
        strncpy(strLastSpreadSheet, "1\r\n2\r\n3\r\n4\r\n/5\r\n6\r\n7\r\n8\r\n9\r\n\r\n", 4095);

        //-------------------------------------
        pthread_mutex_unlock(&gmutex_SSheet);

        /**
         * send the updated spreadsheet to all users in the linked list
         */
        // notify all users in the linked list
        union sigval sval;
        sval.sival_int = 0;
        int rc = pthread_sigqueue(pthreadHandler, SIGNAL_SEND_SPREADSHEET, sval);

        break;

    } while (1);
}

void * serverProcessor(void *arg) {
    int commSocket = -1;
    int ires;
    int total = 0; // how many bytes we've received
    fd_set readfds;
    fd_set exceptfds;
    struct timeval timeout;
    char recvbuf[4096];
    size_t recvbuflen = 4096;

    if (arg == NULL) {
        return NULL;
    }
    commSocket = *(int*) arg;
    free(arg);

    do {
        // process the socket
        FD_ZERO(&readfds);
        FD_SET(commSocket, &readfds);

        FD_ZERO(&exceptfds);
        FD_SET(commSocket, &exceptfds);

        memset(&timeout, 0x00, sizeof (struct timeval));
        timeout.tv_sec = RECYCLE_TIMEOUT; // cycle every 10 seconds
        timeout.tv_usec = 0;

        ires = select(commSocket + 1, &readfds, NULL, &exceptfds, &timeout);
        switch (ires) {
            case -1: // error occurred
                switch (errno) {
                    case EBADF:
                        /**
                         * An invalid file descriptor was given in one of the sets.
                         * (Perhaps a file descriptor that was already closed, or one
                         * on which an error has occurred.)
                         * */
                    case ENOMEM:
                        /**
                         * Unable to allocate memory for internal tables.
                         * */
                    case EINTR:
                        /**
                         * A signal was caught
                         * */
                    default:
                        /**
                         * Just go back to listening for data from the server or the user
                         */
                        continue;
                        break;
                }
                break;

            case 0: // timeout occurred
                /**
                 * Just go back to listening for data from the server or the user
                 */
                continue;
                break;

            default:
                if (FD_ISSET(commSocket, &exceptfds)) {
                    printf("Socket Exception: Error reading scoket\n");
                } else if (FD_ISSET(commSocket, &readfds)) {
                    ires = recv(commSocket, &recvbuf[total], recvbuflen - total, 0);
                    /**
                     * process the data in the received buffer
                     */

                    if (ires == -1) {
                        switch (errno) {
                            case EWOULDBLOCK:
                                /**
                                 * reading would blcok the socket - what sould we do
                                 * -- default -- just wait for more data
                                 */
                                break;
                            default:
                                /**
                                 * There is some other error -- print it
                                 * but we still continue
                                 */
                            {
                                char *perr = strerror(errno);
                                if (perr)
                                    printf("Socket Read Error :  [%s]\n", perr);
                            }
                                break;
                        }
                    } else if (ires >= 0) {
                        // it could be 0: for zero length data
                        // store it in the large buffers
                        // "\r\n\r\n" length = 4

                        total += ires;
                        recvbuf[total] = '\0';
                        char *pstr_find = strstr(recvbuf, "\r\n\r\n");
                        if (pstr_find) {
                            /**
                             * manage the buffer for the socket
                             */

                            char received_data[4096];
                            int ioffset = (int) (pstr_find - recvbuf);

                            strncpy(received_data, recvbuf, 4096);
                            received_data[ioffset] = '\0';
                            strncpy(recvbuf, &received_data[ioffset + 4], total - (ioffset + 4));
                            total = total - (ioffset + 4);
                            recvbuf[total] = '\0';

                            if (firstClientSocket == commSocket && !strcmp(received_data, "SHUTDOWN")) {
                                gbContinueProcessingSpreadSheet = 0; // stop processing
                                break;
                            } else {
                                /**
                                 * upon receipt of data - just update the spreadsheet
                                 */
                                updateSpreadSheet(received_data);
                            }
                        }
                    }
                }
                break;
        }
    } while (gbContinueProcessingSpreadSheet);

    return NULL;
}

int main(int argc, char** argv) {
    int rc;
    int commSocket = -1;
    struct addrinfo *rp;
    struct addrinfo *result = NULL;
    struct addrinfo hints;
    struct timeval timeout;
    fd_set readfds;
    fd_set exceptfds;

    int bOptVal = 1;
    int bOptLen = sizeof (int);
    //struct linger lingerOptVal;
    //int lingerOptLen = sizeof (struct linger);

    /**
     * SANITY CHECK: Make sure the signal structure is clear
     */
    memset(&pthreadHandler, 0, sizeof (pthread_t));

    memset(&hints, 0x00, sizeof (struct addrinfo));
    hints.ai_family = AF_UNSPEC; //AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE; /* For wildcard IP address */

    do {
        /**
         * Resolve the server address and port 
         * change SERVERHOSTNAME and SERVERPORT to the values desired
         */

        rc = getaddrinfo(SERVERHOSTNAME, SERVERPORT, &hints, &result);
        if (rc != 0) {
            printf("ERROR: getaddrinfo failed with: %d\n", rc);
            printf("UNABLE to resolve the server PORT = [%s]\n", SERVERPORT);
            break;
        }

        /*
         * Create a SOCKET for connecting to server
         * getaddrinfo() returns a list of address structures.
         * we will try each address until we successfully connect.
         * If socket(...) fails, we close the socket and try the next address.
         *  
         */

        for (rp = result; rp != NULL; rp = rp->ai_next) {
            commSocket = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (commSocket == -1)
                continue;

            /*
             *  make sure the socket is nonblocking 
             */
            //if ((rc = fcntl(commSocket, F_SETFL, O_NONBLOCK)) != -1) {
            if ((rc = bind(commSocket, rp->ai_addr, rp->ai_addrlen)) == 0)
                break; /* Success */
            //}

            close(commSocket);
            commSocket = -1;
        }

        // Setup the TCP listening socket INADDR_ANY
        if (rp == NULL) { /* No address succeeded */
            printf("ERROR: we could not connect to the server on any address\n");
            break;
        }

        /*
         * SANITY CHECK : this should not occur
         */
        if (commSocket == -1) { /* wah0 - horsey */
            printf("WEIRD ERROR: Thia should not occur. *** whats up ***\n");
            break;
        }

        /* No longer needed */
        if (result) {
            freeaddrinfo(result);
            result = NULL;
        }

        /**
         * set the keep alive so that the socket is always open
         * the connection is not closed due to inactivity
         * 
         * we set linger so that is we crash the "**ALL**" of the last data will
         * still be sent
         */
        //lingerOptVal.l_onoff = 1; // true - turn linger on
        //lingerOptVal.l_linger = RECYCLE_TIMEOUT; // 10 seconds - wait before terminate
        bOptLen = setsockopt(commSocket, SOL_SOCKET, SO_KEEPALIVE, (char*) &bOptVal, bOptLen);
        //bOptLen = setsockopt(commSocket, SOL_SOCKET, SO_LINGER, (char*) &lingerOptVal, lingerOptLen);

        /**
         * Create a thread to handle termination signals for network handler thread
         * We need a thread so that we can send signal to it - easily
         */
        if ((rc = pthread_create(&pthreadHandler, NULL, &sig_sendSheethandler, NULL))) {
            printf("Signal handler thread creation failed: [%d]\n", rc);
            close(commSocket);
            exit(EXIT_FAILURE);
        }

        pthread_mutex_init(&gmutex_SSheet, NULL);

        printf("Ready To Start Processing Spread Sheet Requests\n");

        do {
            // if the socket fail restart the socket
            rc = listen(commSocket, SOMAXCONN);
            if (rc == -1) {
                char *perr = strerror(errno);
                if (perr)
                    printf("Socket Listen error :  [%s]\n", perr);
                break;
            }
            //%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
            //Process this socket to check for incomming requests
            do { // process the socket
                FD_ZERO(&readfds);
                FD_SET(commSocket, &readfds);

                FD_ZERO(&exceptfds);
                FD_SET(commSocket, &exceptfds);

                timeout.tv_sec = RECYCLE_TIMEOUT; // cycle every 10 seconds
                timeout.tv_usec = 0;

                rc = select(commSocket + 1, &readfds, NULL, &exceptfds, &timeout);
                switch (rc) {
                    case -1:
                        rc = 0;
                        switch (errno) {
                            case EINTR:
                                continue;
                                break;
                            default:
                                break;
                        }
                        break;

                    case 0: // timeout occurred
                        // the connection may be slow
                        rc = 0;
                        break; // just retry

                    default:
                        rc = 0;
                        if (FD_ISSET(commSocket, &exceptfds)) {
                            break;
                        } else if (FD_ISSET(commSocket, &readfds)) {
                            rc = 1;
                            break;
                        }
                        break;
                }
            } while (gbContinueProcessingSpreadSheet && !rc);

            //%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

            if (gbContinueProcessingSpreadSheet) {
                // Accept a client socket
                int cSocket = accept(commSocket, NULL, NULL);
                if (cSocket == -1) {
                    switch (errno) {
                        case EINTR:
                        case EWOULDBLOCK:
                            /* just retry */
                            break;
                        default:
                            break;
                    }
                } else {
                    //lingerOptVal.l_onoff = 1;
                    //lingerOptVal.l_linger = RECYCLE_TIMEOUT; // 10 seconds
                    bOptLen = setsockopt(cSocket, SOL_SOCKET, SO_KEEPALIVE, (char*) &bOptVal, bOptLen);
                    //bOptLen = setsockopt(cSocket, SOL_SOCKET, SO_LINGER, (char*) &lingerOptVal, lingerOptLen);


                    do {
                        int *pclientSocket = (int *) malloc(sizeof (int));
                        if (pclientSocket == NULL) {
                            printf("Error: NO mem processing client connection\n");
                            close(cSocket);
                            break;
                        }
                        // add socket to linked list
                        // add also the thread-id to the list structure
                        clientlist_t *plist = malloc(sizeof (clientlist_t));
                        if (plist == NULL) {
                            printf("Error: NO mem processing client connection\n");
                            close(cSocket);
                            free(pclientSocket);
                            break;
                        }
                        *pclientSocket = cSocket;

                        pthread_t thread;
                        if (pthread_create(&thread, NULL, serverProcessor, (void *) pclientSocket) != 0) {
                            printf("Error Setting up session for a client\n");
                            if (plist) {
                                free(plist);
                            }
                            close(cSocket);
                            free(pclientSocket);
                            break;
                        }

                        // add socket to linked list
                        // add also the thread-id to the list structure
                        plist->clientSocket = cSocket;
                        plist->next = NULL;
                        plist->pthreadClient = thread;
                        if (firstClientSocket == -1) {
                            firstClientSocket = cSocket;
                            /*this is the controller socket client*/
                        }
                        // DUMMY TEST LINKED LIST -- DO IT PROPERLY
                        plist->next = listTop;
                        listTop = plist;
                        //-------------------------------------
                        // TODO
                        // add "plist" to the linked list
                        //-------------------------------------
                    } while (0);
                }
            }

        } while (gbContinueProcessingSpreadSheet);
    } while (0);

    if (commSocket != -1)
        close(commSocket);

    if (pthreadHandler != 0) {
        pthread_cancel(pthreadHandler);
        pthread_join(pthreadHandler, NULL);
    }

    // for client in the linlked list
    clientlist_t *list;
    for (list = listTop, listTop = NULL; list;) {
        // tell the other clients to shutdown
        // if they get a socket error - THEY SHOULD TERMINATE
        sendall(list->clientSocket, "SHUTDOWN\r\n\r\n", strlen("SHUTDOWN\r\n\r\n"), 0);
        clientlist_t *temp = list;
        list = list->next;
        if (temp->pthreadClient) {
            pthread_cancel(temp->pthreadClient);
            pthread_join(temp->pthreadClient, NULL);
        }
        close(temp->clientSocket);
        free(temp);
    }

    pthread_mutex_destroy(&gmutex_SSheet);

    return (EXIT_SUCCESS);
}

