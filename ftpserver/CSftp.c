#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <stdlib.h>
#include "dir.h"
#include "usage.h"

#include <sys/types.h>
#include <sys/stat.h>

const char* userCmd = "USER";
const char* quitCmd = "QUIT";
const char* cdCmd   = "CWD";
const char* cdupCmd = "CDUP";
const char* typeCmd = "TYPE";
const char* modeCmd = "MODE";
const char* struCmd = "STRU";
const char* retrCmd = "RETR";
const char* pasvCmd = "PASV";
const char* nlstCmd = "NLST";

#define USER 0
#define QUIT 1
#define CWD 2
#define CDUP 3
#define TYPE 4
#define MODE 5
#define STRU 6
#define RETR 7
#define PASV 8
#define NLST 9
#define UNKNOWN -1

#define SIZE 1024
#define DATA_BUF_SIZE 4096


/**
 * Given a command inserted by the user and returns the number associated with that command
 * @param cmd command inserted by the user at the command line
 */
int parseCMD(const char * cmd){
    if(!strcasecmp(cmd,userCmd))return USER;
    if(!strcasecmp(cmd,quitCmd))return QUIT;
    if(!strcasecmp(cmd,cdCmd))return CWD;   
    if(!strcasecmp(cmd,cdupCmd))return CDUP;   
    if(!strcasecmp(cmd,typeCmd))return TYPE; 
    if(!strcasecmp(cmd,modeCmd))return MODE;  
    if(!strcasecmp(cmd,struCmd))return STRU;   
    if(!strcasecmp(cmd,retrCmd))return RETR;
    if(!strcasecmp(cmd,pasvCmd))return PASV;
    if(!strcasecmp(cmd,nlstCmd))return NLST;   
    return UNKNOWN;
}

/**
 * Given a IPV4 address string like a.b.c.d, extract the corresponding numbers to a,b,c,d respectively
 * These a b c d are used for PASV
 */
void fillABCD(char *addr, uint8_t * a, uint8_t * b,uint8_t * c,uint8_t * d){
    *a = (uint8_t)atoi(strtok(addr, "."));
    *b = (uint8_t)atoi(strtok(NULL, "."));
    *c = (uint8_t)atoi(strtok(NULL, "."));
    *d = (uint8_t)atoi(strtok(NULL, "."));
}
/**
 * Sends the msg to the client,
 * returns an int that determines if the msg was send properly or not
 */
int sendMessageOK(int con_sd,const char* msg){
    return send(con_sd, msg, strlen(msg),0) == strlen(msg);
}

/**
 *  Produce error message if message is not send correctly
 */
void sendMessage(int con_sd,const char* msg, const char* onError){
    if(!sendMessageOK(con_sd,msg)){
        perror(onError);   
    }
}

/**
 *  Sends 530 message to notify that the client is not logged lin
 */
void loginCheck(int loginDone,int client_SD){
    if(!loginDone){
        sendMessage(client_SD,"530 Not logged in.\r\n","error on send message to notify that the client is not logged in");
    }
}

/**
 * Send the IP and Port
 */
void sendIPandPort(int pasvsocketfd,int isLoopback,int controlsd,char * buffer){    
    struct ifaddrs *ifap, *ifa;
    struct sockaddr_in *sa;
    char *addr;

    uint8_t a,b,c,d;
    struct sockaddr_in socketaddress_pasvsocketfd;
	unsigned int pasvPort; 

    bzero(&socketaddress_pasvsocketfd, sizeof(struct sockaddr_in));
    socketaddress_pasvsocketfd.sin_family = AF_INET;
    socketaddress_pasvsocketfd.sin_port = 0;
    getifaddrs (&ifap);
    for (ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET) {
            sa = (struct sockaddr_in *) ifa->ifa_addr;
            addr = inet_ntoa(sa->sin_addr);
            if(isLoopback){
                printf("Interface: %s\tAddress: %s\n", ifa->ifa_name, addr);
                fillABCD(addr,&a,&b,&c,&d);
                socketaddress_pasvsocketfd.sin_addr.s_addr = sa->sin_addr.s_addr;
                break;
            }else if(strcmp(addr,"127.0.0.1") != 0){
                printf("Interface: %s\tAddress: %s\n", ifa->ifa_name, addr);
                fillABCD(addr,&a,&b,&c,&d);
                socketaddress_pasvsocketfd.sin_addr.s_addr = sa->sin_addr.s_addr;
                break;
            }
        }
    }
    freeifaddrs(ifap);


    // bind the listenfd to a socket address
    if (bind(pasvsocketfd, (const struct sockaddr*) &socketaddress_pasvsocketfd, sizeof(struct sockaddr_in)) != 0)
    {
        perror("Failed to bind the pasv socket");
    }


    // Set the socket to listen for connections
    if (listen(pasvsocketfd, 1) != 0)
    {
        perror("Failed to listen for pasv connections");
    }
    int len = sizeof(struct sockaddr_in);
    bzero(sa, len);
    getsockname(pasvsocketfd, (struct sockaddr *) sa, &len);
    pasvPort = ntohs(sa->sin_port);
    printf("pasv port: %d\n", pasvPort);
    printf("pasv port: %d\n", htons(sa->sin_port));

    len = snprintf(buffer, SIZE, "227 Entering Passive Mode (%d,%d,%d,%d,%d,%d)\r\n",a,b,c,d,pasvPort / 256,pasvPort % 256);
    if (send(controlsd, buffer, len, 0) != len)
    {
        perror("Failed to send to the socket");
    }
}

/**
 * Retrieves a file in the current directory
 * @param client_SD client socket
 * @param pasvsocketfd passive socket
 * @param currentdirectory the current directory the client is in
 * @param param the file wanted to be retrieved indicated by the client
 */
void retr(int client_SD, int pasvsocketfd, char* currentdirectory, char* param){
    struct sockaddr_in dataadress;
    int datalength = sizeof(struct sockaddr_in);

    char send_data_buffer[DATA_BUF_SIZE]; // data buffer that is used when retrieving of a file

    // Establish data connection
    int newdatafd = accept(pasvsocketfd, (struct sockaddr*) &dataadress, &datalength);
    printf("current dir is %s\n", currentdirectory);
    FILE* file = fopen(param, "rb");
    if (!file) {
        perror("550 File unavailable.");
        sendMessage(client_SD,"550 Requested action not taken. File unavailable\r\n",
            "error on send message to notify that the client's specified file is not available");
    }
    else {
        // Seek to the start of the file to began reading and sending
        fseek(file, 0, SEEK_SET);
        sendMessage(client_SD,"125 Data connection already open; transfer starting.\r\n",
            "error on send message to notify that the client's specified file is now being transferred");
        size_t result;
        ssize_t sendresult;
        // Read the data in the file to the buffer and then sending the buffer
        while ( (result = fread(send_data_buffer, sizeof(char), DATA_BUF_SIZE, file)) > 0) {
            sendresult = send(newdatafd, send_data_buffer, result,0);
            if (sendresult != result) {
                perror("550 File send error.");
                sendMessage(client_SD,"550 Requested action not taken. File send error.\r\n",
                    "error on send message to notify that the client's specified file has error when sending");
                break;
            }
        }
        sendMessage(client_SD,"226 Closing data connection. Requested file action successful.\r\n",
            "error on send message to notify that closing data connection has error");
        fclose(file);
    }

    close(newdatafd);
}

/**
 * Produce a listing for the current directory
 * @param client_SD client socket
 * @param pasvsocketfd passive socket
 * @param currentdirectory the current directory the client is in
 */
void nlst(int client_SD, int pasvsocketfd, char* currentdirectory){
    struct sockaddr_in dataadress;
    int datalength = sizeof(struct sockaddr_in);
    int newdatafd = accept(pasvsocketfd, (struct sockaddr*) &dataadress, &datalength);

    getcwd(currentdirectory, SIZE);
    sendMessage(client_SD,"150 here comes the data.\r\n","error on send message to notify that data transfer is prepared");
    listFiles(newdatafd, currentdirectory);
    sendMessage(client_SD,"226 Closing Data Connection, Requested File Action Successful.\r\n","error on send message to notify that data transfer is done");

    close(newdatafd);
}
/**
 * Server status machine, ftp command implementation here
 *
 * @param args the two integer params array, where the first one is client sd, the second one is to indicate whether it is 
 * a loop back address.
 */
void* interact(void* args) {

    const int client_SD = *(int*) args;
    printf("client socket des is %d\n",client_SD);
    const int isLoopback = *((int*) args + 1);
    printf("isLoopback flag is %d\n",isLoopback);

    //Responds with a 220 to initiate a login sequence. terminated by CRLF
    sendMessage(client_SD,"220 (v1) Please enter your user name below\r\n","fail to initiate a greeting to client");

    // Interact with the client, this is a buffer for recv of the control connection
    char buffer[SIZE];
    char *command, *param;

    //save starting root path
    char initialworkingdirectory[SIZE];
    getcwd(initialworkingdirectory, SIZE);
    char currentdirectory[SIZE];

    //int variables that track the current state
    int passivemodeon = 0;
    int optval = 1;
    int loginDone = 0;
    int shouldQuit = 0;
    int pasvsocketfd;

    while (1)
    {
        bzero(buffer, SIZE);
        
        // Receive the client message
        ssize_t length = recv(client_SD, buffer, SIZE, 0);
        
        if (length < 0){
            perror("Failed to read from the socket");
            break;
        }
        
        if (length == 0){
            printf("EOF\n");
            break;
        }
        
        //split string as token
        command = strtok(buffer, " \t\n\r");
        if (!command) {
            printf("command is null\n");
            continue;
        }
        
        // Get the arguments to the command, if provided.
        param = strtok(NULL, " \t\n\r");
        printf("value of command: %s\n", command);
        printf("value of param: %s\n", param);

        // terminates the while loop when it send a quit command
        if (parseCMD(command) == QUIT) {
            printf("about to quit while loop and close control connection\n");
            loginDone = 0;
            sendMessage(client_SD,"221 Service closing control connection\r\n","error on send");
            break;
        }  

        // Switch cases for different commands typed by the user
        switch (parseCMD(command))
        {
        case USER:
            // prompts the user to log in with a valid user name if already not logged in
            if(loginDone){
                sendMessage(client_SD,"530 cs317 already login successful\r\n","error on send message to resoponse USER command");
            }else if(param && (strcmp(param,"cs317") == 0) && (loginDone == 0)){
                sendMessage(client_SD,"230 cs317 login successful\r\n","error on send message to resoponse USER command");
                loginDone = 1;
            }else{
                sendMessage(client_SD,"530 Not logged in.\r\n","error on send message to resoponse USER command");
            }
            break;
        case UNKNOWN:
            // unknown command, send message 500
            sendMessage(client_SD,"500 unknown command \r\n","error on send message to resoponse UNKNOWN command");
            break;
        case TYPE:
            // only support Image and ASCII type
            loginCheck(loginDone,client_SD);
            // unknown mode's response code
            if(!param || (strcasecmp(param,"I") != 0 && strcasecmp(param,"A") != 0)){
                sendMessage(client_SD,"500 unknown mode\r\n",
                    "error on send message to notify that the client's specified type is not supported");
                break;
            }
            if( strcasecmp(param,"I") == 0 ){
                sendMessage(client_SD, "200 switch to image mode\r\n",
                    "error on send message to notify that we have already switched to binary type");
                break;
            }
            if( strcasecmp(param,"A") == 0 ){
                sendMessage(client_SD, "200 switch to ascii mode\r\n",
                    "error on send message to notify that we have already switched to ascii type");
                break;
            }
            break;

        case MODE:
            // Only supports steam mode
            loginCheck(loginDone,client_SD);
            if(!param || strcasecmp(param,"S") != 0){
                sendMessage(client_SD, "500 unknwon data transfer mode\r\n","error on send message to notify that the client's specified data transfer mode is not supported");
                break;
            }else{
                sendMessage(client_SD,"200 switch to stream data transfer mode\r\n","error on send message to notify that the client's specified data transfer mode has changed successfully");
                break;
            }
            break;
        case CWD:
            // directory change to the directory specified by param
            loginCheck(loginDone,client_SD);
            // checking if parameter is null
            if (!param) {
                perror("Missing param for cwd");
                sendMessage(client_SD,"501 Syntax error in parameters or arguments.\r\n",
                    "error on telling client that they miss param for cd command");
            }
            //checking if param begins with .. or . or contains .. or contains .
            if (strstr(param, "../") != NULL || strstr(param, "./") != NULL || strncmp(param, ".", strlen(".")) == 0 || strncmp(param, "..", strlen("..")) == 0) {
                perror("illegal path with . or ..");
                sendMessage(client_SD,"550 Requested Action Not Taken, illegal path\r\n",
                    "error on telling client that their param is illegal by containing . or .. for cd command");
            }  else {
                // change to the directory
                int result = chdir(param);
                if (result == 0) {
                    sendMessage(client_SD,"250 Requested File Action Okay, Completed\r\n",
                        "error on telling client that their cwd changed correctly");
                } else {
                    sendMessage(client_SD,"550 Requested Action Not Taken fail to change directory\r\n",
                        "error on telling client that their cwd changed unsuccessfully");
                }
            }
            getcwd(currentdirectory,SIZE);
            printf("current after cd is %s\n",currentdirectory);
            break;
        case CDUP:
            // Changes to the next higher directory level
            loginCheck(loginDone,client_SD);       
            // Respond with 501 if error in parametes
            if (param) {
                perror("501 Syntax error in parameters or arguments.");
                sendMessage(client_SD,"501 Syntax error in parameters or arguments.\r\n","error on telling client that they included param for cdup command");
                break;
            }
            getcwd(currentdirectory,SIZE);
            // Checks if the current directory is at the root directory
            if (strcmp(currentdirectory, initialworkingdirectory)== 0) {
                perror("550 Requested Action Not Taken");
                sendMessage(client_SD,"550 Requested Action Not Taken, illegal cd requirement\r\n","error on telling client that their param is illegal cdup from initial working dir");
                break;
            }
            // goes to the higher directory
            int resultafter = chdir("..");
            if (resultafter == 0) {
                sendMessage(client_SD,"250 Requested File Action Okay, Completed\r\n","error on telling client that their cdup action is done");
            } else {
                perror("550 Requested Action Not Taken");
                sendMessage(client_SD,"550 Requested Action Not Taken fail to change directory\r\n","error on telling client that their cdup changed unsuccessfully");
            }
            getcwd(currentdirectory,SIZE);
            printf("current after cdup is %s\n",currentdirectory);
            break;
        case STRU:
            // only support File structure type
            loginCheck(loginDone,client_SD);
            if(!param || strcasecmp(param,"F") != 0){
                sendMessage(client_SD,"500 only File-structure available\r\n","error on send message to notify that only File-structure supported");
            }else{
                sendMessage(client_SD,"200 data structure switched to File-structure\r\n","error on send message to notify that file-structure changed correctly");
            }
            break;
        case PASV:
            // Passive mode
            loginCheck(loginDone,client_SD); 
            if ((pasvsocketfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
                perror("error opening socket");
                sendMessage(client_SD,"425 Can't open data connection.\r\n","error on send message to notify that the pasv socket is not created");
                break;
            }    
            if (setsockopt(pasvsocketfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(int)) != 0){
                perror("Failed to set the socket option");
            }
            // Helper function
            sendIPandPort(pasvsocketfd,isLoopback,client_SD,buffer);
            passivemodeon = 1; // set this to one if passive mode is on
            break;
        case RETR:
            // transfer a copy of the file,
            // specified in the pathname, to the server- or user-DTP
            // at the other end of the data connection.
            loginCheck(loginDone,client_SD);
            // Respond with 501 if error in parameters
            if (!param) {
                perror("501 Syntax error in parameters or arguments.");
                sendMessage(client_SD,"501 Syntax error in parameters or arguments.\r\n",
                    "error on send message to notify that the param is missing for retr");
                break;
            }

            // Respond with 425 if not in passive mode
            if (!passivemodeon) {
                perror("use pasv first");
                sendMessage(client_SD,"425 Not In Passive Mode.\r\n",
                    "error on send message to notify that client is not in pasv");
                break;
            }
            // Helper function
            retr(client_SD, pasvsocketfd, currentdirectory, param);
            close(pasvsocketfd);
            passivemodeon = 0;
            pasvsocketfd = -1;
            break;
        case NLST:
            // NLST - (4.1.3) to produce a directory listing,
            loginCheck(loginDone,client_SD);

            // Respond with a 501 if the server gets an NLST with a parameter.
            if (param) {
                perror("501 Syntax error in parameters or arguments.");
                sendMessage(client_SD,"501 Syntax error in parameters or arguments.\r\n",
                    "error on send message to notify that the param is missing for retr");
                break;
            }
            // Respond with 425 if not in passive mode
            if (!passivemodeon) {
                perror("use pasv first");
                sendMessage(client_SD,"425 Not In Passive Mode.\r\n","error on send message to notify that client is not in pasv");
                break;
            }
            // Helper function
            nlst(client_SD,pasvsocketfd,currentdirectory);
            close(pasvsocketfd);
            passivemodeon = 0;
            pasvsocketfd = -1;
            break;
        default:
            break;
        }
    }
    close(client_SD);
    return NULL;
}

int main(int argc, char *argv[]){
    
    // Check the command line arguments
    if (argc != 2){
      usage(argv[0]);
      return -1;
    }

    //create an TCP endpoint and received by socket descriptor listenfd;
    //Its first paramenter selects the protocol family for which here is protocol family INET
    //SOCK_STREAM indicates that it is a sequenced reliable full-duplex TCP type
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);

    if (listenfd < 0){
        perror("Failed to create the socket.");
        exit(-1);
    }

    // before invoking bind, set socket socketaddress to be able to be reused
    // option name is SO_REUSEADDR to avoid "Address already in use" problem.
    // optval should be non negative to enable a boolean option.
    int optval = 1;
    
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval, sizeof(int)) != 0){
        perror("Failed to set the socket option");
        exit(-1);
    }

    struct sockaddr_in socketaddress;
    
    //zero out the socket address
    bzero(&socketaddress, sizeof(struct sockaddr_in));
    
    socketaddress.sin_family = AF_INET;
    socketaddress.sin_port = htons(atoi(argv[1]));
    socketaddress.sin_addr.s_addr = htonl(INADDR_ANY);

    // bind the listenfd to a socket address
    if (bind(listenfd, (const struct sockaddr*) &socketaddress, sizeof(struct sockaddr_in)) != 0){
        perror("Failed to bind the socket");
        exit(-1);
    }

    // Set the socket to listen for connections
    if (listen(listenfd, 1) != 0){
        perror("Failed to listen for connections");
        exit(-1);
    }

    while (1)
    {
        struct sockaddr_in clientAddress;
        
        socklen_t clientAddressLength = sizeof(struct sockaddr_in);
        
        printf("Waiting for incomming connections...\n");
        
        //Pass in two params into interact function, the first on is a new socket returned by the accept
        //The second one is to indicate whether the client IP address is a loopback addr i.e. 127.0.0.1
        //If it is loopback, second param is set to 1, otherwise it is 0.
        int paramsForInteract[2] = {0,0};

        // Accept the top queue connection.
        if ((paramsForInteract[0] = accept(listenfd, (struct sockaddr*) &clientAddress, &clientAddressLength)) < 0){
            perror("Failed to accept the client connection");
            continue;
        }
        printf("addr is %s\n",inet_ntoa(clientAddress.sin_addr));

        if ((paramsForInteract[1] = !strcmp(inet_ntoa(clientAddress.sin_addr),"127.0.0.1")) == 1){
            printf("Client is local host\n");
        }

        printf("Accepted the client connection from %s:%d.\n", inet_ntoa(clientAddress.sin_addr), ntohs(clientAddress.sin_port));

        
        // Create a separate thread to interact with the client
        pthread_t thread;
        
        if (pthread_create(&thread, NULL, interact, paramsForInteract) != 0)
        {
            perror("Failed to create the thread");
            continue;
        }
        
        // The main thread just waits until the interaction is done
        pthread_join(thread, NULL);
        
        printf("Interaction thread has finished.\n");
    }
    
    return 0;

}
