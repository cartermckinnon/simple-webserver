/*
|  A simple multi-threaded HTTP server.
|  Created by Carter McKinnon on 10/25/16.
|
| THANKS:
| socket library overview-> http://www.linuxhowtos.org/C_C++/socket.htm
| strnstr() implementation-> http://stackoverflow.com/questions/23999797/implementing-strnstr
| ^ Creative Commons license. 
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <pthread.h>
#include <signal.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <limits.h>
#include "strnstr.c"      // necessary for many Linux systems.
                          // (BSD varients often implement strnstr() in the standard C lib.)

/* _____ OPTIONS _____ */
#define NUM_THREADS 4       // number of threads, number of simultaneous requests handled.
                            /* (Because no connection is persistant, a higher number will result
                               in increased performance for a single client and for multiple clients,
                               up to a point. Optimal number would be the number of hardware threads. ) */
#define BUFFER_SIZE 1024     // size of client buffer
#define MOBILE_USER_AGENTS "mobile_ids"     // file containing list of mobile User-Agent ID's


/* _____ GLOBALS _____ */
static int port;                                               // port number
static char* dir;                                              // root directory
static int sock, cli_addr_len, n[NUM_THREADS];                 // socket handle;
static int connections[NUM_THREADS];                           // connection handles
static char* buffers[NUM_THREADS];                             // connection buffers
static struct sockaddr_in serv_addr,cli_addr[NUM_THREADS];     // server and client addresses
static int online;                                             // status of server
static pthread_t threads[NUM_THREADS];                         // thread handles
static int avail[NUM_THREADS];                                 // client id's
static pthread_mutex_t mutex;                                  // thread syncronization
char* mobile_ids;                                              // list of mobile 'User-Agent' id's


/* _____ FUNCTIONS _____ */
void error(char *msg);                          // error handler
int add_client();                               // allocate client id
void sub_client();                              // deallocate client id
void parse_args(int argc, const char* argv[] ); // parse command line arguments
void prepare_socket();                          // create & bind socket
void accept_connections();                      // listen for & deploy connections to threads
void* serve(void* client);                      // serve client (thread workload)
char* get_header(int id,
                 char* packet,
                 int hostname_len);             // generate header for response
char* header_helper(int id,
                    int status_code,
                    char* status_msg,
                    char* content_type,
                    int packet_length,
                    int filename_length);       // prints header to thread's buffer
char* redirect_helper(int id, char* url);       // prints status 301 header to thread's buffer
void send_header(int connection, char* header); // send header to client
void send_data(int connection,
               int file,
               char* dynamic_page);             // send data from (templated) file to client
int file_open(char* file);                      // open file for reading
int file_size(char* file);                      // get file size in bytes
int file_exists(char* file);                    // check if file exists
void file_build_path(int id, char* packet);     // combine root directory w/ requested file's path in thread's buffer
void file_build_mobile_path(int id, char* packet); // combine root directory + "/mobile" + file name in thread's buffer
char* url_build(int id, char* packet);           // build url in thread's buffer
void go_offline();                              // first step in exit--break all loops
int is_online();                                // check online status
void load_mobile_ids(char* filename);           // load list of mobile User-Agents into mobile_ids
int mobile_user(char* packet);                  // determine whether a packet uses a supported mobile User-Agent
void cleanup_and_exit();                        // go_offline, cleanup memory, join threads, and exit (called w/ CTRL+C)

int main(int argc, const char * argv[])
{
    /* parse args */
    parse_args(argc, argv);
    
    /* register exit signal (CTRL+C) */
    signal(SIGINT, cleanup_and_exit);
    
    /* initialize mutex */
    pthread_mutex_init(&mutex, NULL);
    
    /* initialize client ID's */
    for( int i = 0; i < NUM_THREADS; i++){
        avail[i] = NUM_THREADS;
    }
    
    /* prepare socket */
    prepare_socket();
    
    /* accept connections (loop) */
    accept_connections();
}

void parse_args(int argc, const char* argv[] )
{
    // check number of args
    if( argc != 3 ) error("usage: webserver <port #> <root directory>\n");
    
    // check if port in range
    port = atoi(argv[1]);
    if( port == 0 ) error("port number must be greater than zero.\n");
    
    // save root directory
    dir = malloc(strlen(argv[2])+1);
    strcpy(dir, argv[2]);
    
    // verify dir exists
    if (0 != access(dir, F_OK)) {
        if (ENOENT == errno) {
            // does not exist
            error("Root directory does not exist");
        }
        if (ENOTDIR == errno) {
            // not a directory
            error("Path given is not directory");
        }
    }
}

void prepare_socket()
{
    /* create socket */
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if( sock < 0 ) error("Couldn't create socket");
    
    /* prepare tools */
    bzero((char *) &serv_addr, sizeof(serv_addr));  // initialize to zero
    serv_addr.sin_family = AF_INET;                 // set address family to 'internet'
    serv_addr.sin_port = htons(port);               // convert host port to network port
    serv_addr.sin_addr.s_addr = INADDR_ANY;         // set server IP to symbolic
    
    /* bind socket */
    if( bind(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0 ){
        error("Coulnd't bind socket");}
    
    /* go online */
    listen(sock, 5);    // 5 is the standard maximum for waiting socket clients
    online = 1;
    printf("\n------------------------------\n    Listening on port %d\n       EXIT WITH CTRL+C       \n------------------------------\r\n",port);
}

void accept_connections()
{
    cli_addr_len = sizeof(cli_addr);
    if( file_exists(MOBILE_USER_AGENTS) ){
        load_mobile_ids(MOBILE_USER_AGENTS);
    }
    while(online){
        int id = add_client();
        if( id != NUM_THREADS ){
            bzero(&cli_addr[id], sizeof(struct sockaddr_in));
            connections[id] = accept(sock,
                                     (struct sockaddr*) &cli_addr[id],
                                     (unsigned int *)&cli_addr_len);
            if ( connections[id] < 0 ){ error("Error accepting client.\n"); }
            else if ( pthread_create(&threads[id], NULL, serve, &avail[id]) != 0 ){
                error("Couldn't create thread");
            }
        }
    }
}

void* serve(void* arg)
{
    /* allocate resources */
    int id = *((int*)arg);                  // local copy of id makes following code cleaner
    buffers[id] = malloc(BUFFER_SIZE);      // create buffer for session
    int timeout = 0;                        // timeout counter; 3 1-second empty reads until session ends
    
    /* serving client */
    while( is_online() && timeout < 3 )
    {
        bzero(buffers[id], BUFFER_SIZE);                                    // clean buffer
        n[id] = (int)read(connections[id],buffers[id],BUFFER_SIZE);   // read from socket
        
        // if there's nothing to read
        if( n[id] <= 0 ){ timeout++; /*increment timeout counter*/ }
        else{
            /* save host name from buffer */
            char* end = strnstr(buffers[id], "Host: ", 256);    // find beginning of host name
            char* start = end + 6;                              // most past "Host: "
            end = strnstr(start, "\r", 32);                     // find end of host name
            int host_len = (int)(end - start);                  // find length of host name
            char host[host_len+1];                              // create storage
            strncpy(host, start, host_len);                     // copy name
            host[host_len] = '\0';                              // add nullchar (strncpy doesn't)
            
            send_header(id, get_header(id, buffers[id], host_len));     // parse what was read & send response header
            if( strnstr(buffers[id], "http://",7) == NULL ){            // if not a redirect
                if( file_exists(buffers[id])){
                    send_data(id, file_open(buffers[id]), NULL);          // send HTTP payload
                }
                else{   // if file doesn't exist
                    char url[100]; // url used in templated 404 page
                    snprintf(url, 100, "http://%s%s",host,buffers[id]+strlen(dir));
                    send_data(id, file_open("404.html"), url);
                }
            }
            /* NO CONNECTIONS ARE PERSISTANT--if response was sent, close connection */
            break;
        }
    }
    
    /* ending session */
    shutdown(connections[id], 2);   // close connection
    free(buffers[id]);              // deallocate buffer
    sub_client(id);                 // deallocate client ID
    pthread_detach(threads[id]);    // detach thread to deallocate resources
    return NULL;
}

char* get_header(int id, char* packet, int host_len)
{
    char* request = strnstr(packet, "GET", 32);     // search for GET
    if( request != NULL )
    {
        /* if /go/ redirect specified */
        if( strnstr(packet, " /go/", 16) != NULL){
            return redirect_helper(id, url_build(id, packet));
        }
        
        /* if no page specified */
        else if( strnstr(packet, " / ", 16) != NULL ){      // default "/" to "/index.html"
            if( mobile_user(packet) ){
                file_build_mobile_path(id, "/index.html");  // if mobile user
            } else {
                file_build_path(id, "/index.html ");        // if non-mobile
            }
        }
        
        /* if page is specified */
        else if( strnstr(packet, " /", 16) != NULL ){
            if( mobile_user(packet) ){
                file_build_mobile_path(id, packet);     // if mobile user
            } else {
                file_build_path(id, packet);            // if non-mobile
            }
        }
        
        /* verify file exists, then generate header */
        if( file_exists(buffers[id]) ){
            if( strnstr(buffers[id], ".jpg", 50) != NULL){
                return header_helper(id, 200, "OK", "image/jpeg", file_size(buffers[id]), (int)strlen(buffers[id]));
            }
            if( strnstr(buffers[id], ".png", 50) != NULL ){
                return header_helper(id, 200, "OK", "image/png", file_size(buffers[id]), (int)strlen(buffers[id]));
            }
            if( strnstr(buffers[id], ".html", 50) != NULL ){
                return header_helper(id, 200, "OK", "text/html", file_size(buffers[id]), (int)strlen(buffers[id]));
            }
            if( strnstr(buffers[id], ".css", 50) != NULL ){
                return header_helper(id, 200, "OK", "text/css", file_size(buffers[id]), (int)strlen(buffers[id]));
            }
            if( strnstr(buffers[id], ".js", 50) != NULL ){
                return header_helper(id, 200, "OK", "Application/javascript", file_size(buffers[id]), (int)strlen(buffers[id]));
            }
            return header_helper(id, 200,"OK", "text", file_size(buffers[id]), (int)strlen(buffers[id]));   // default text content
        }
        
        /* if file doesn't exist, generate 404 from template */
        else{
            int filename_len = (int)(strlen(buffers[id])-strlen(dir));  // length of filename without full dir path
            int url_len = 7;    // length of "http://"
            // <HTML> + "http://" + <host:port> + <file>
            int content_len = file_size("404.html") - 1
                              + url_len
                              + host_len
                              + filename_len;
            return header_helper(id, 404, "Not Found", "text/html", content_len, (int)strlen(buffers[id]));
        }
    }
    return NULL;    // shouldn't happen, but necessary for thread function prototype
                    // and useful for error detection.
}

/* print generic response header to buffer */
char* header_helper(int id, int status_code, char* status_msg, char* content_type, int content_length, int filename_length)
{
    char* header = buffers[id] + filename_length + 1;     // write header after file path in the buffer
    snprintf(header,
             120,   // limit on header length to avoid overflow in small buffers
             "HTTP/1.1 %d %s\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %d\r\n"
             "Connection: close\r\n"
             "\r\n", status_code, status_msg, content_type, content_length);
    return header;
}

/* print status 301 header in buffer for redirect functionality */
char* redirect_helper(int id, char* url)
{
    char* header = buffers[id] + strlen(url) + 1;     // write header after file path in the buffer
    snprintf(header,
             120,   // limit on header length to avoid overflow in small buffers
             "HTTP/1.1 301 Moved Permanently\r\n"
             "Location: %s\r\n"
             "\r\n", url);
    return header;
}

/* send header to client */
void send_header(int id, char* header)
{
    if( header != NULL ){
        int n = (int)strlen(header);
        void* pos = header;
        while (n > 0) {
            int bytes_written = (int)send(connections[id], pos, n, 0);
            if (bytes_written <= 0) { error("Couldn't send header."); }
            n -= bytes_written;
            pos += bytes_written;
        }
    }
}

/* Send input_file to client, optionally filling dynamic template data */
void send_data(int id, int input_file, char* dynamic)
{
    /* handle missing file cases */
    if( input_file == -1 ){
        input_file = open("404.html",O_RDONLY, S_IREAD);                        // 404 if specified file missing
        if( input_file == -1 ){ error("Necessary files missing: 404.html"); }   // error if 404 template missing
    }
    /* clear buffer */
    bzero(buffers[id], BUFFER_SIZE);
    
    /* send file to client */
    char* read_pos = buffers[id];
    char* dyn_pos = dynamic;
    while (1) {
        int bytes_read;
        if(dynamic == NULL){
            bytes_read = (int)read(input_file, buffers[id], BUFFER_SIZE);       // Read data into buffer.
        }
        else{
            bytes_read = (int)read(input_file, read_pos, 1);       // Read data into buffer.
            read_pos++;
        }
        if (bytes_read == 0){ break; }                                      // Exit if nothing to read.
        if (bytes_read < 0){ error("Couldn't read file into buffer"); }     // Handle errors.
        if( dynamic != NULL ){
            
            /* 1 char read test for exit condition
             is necessary in dynamic case. Because
             of this, first char cannot be template ('*') */
            bytes_read = (int)read(input_file, read_pos, 1);
            if (bytes_read == 0){ break; }                                      // Exit if nothing to read.
            if (bytes_read < 0){ error("Couldn't read file into buffer"); }     // Handle errors.
            read_pos++;
            
            /* render template */
            while ( bytes_read <= BUFFER_SIZE ) {
                int new_bytes = (int)read(input_file, read_pos, 1);
                if (new_bytes == 0){ break; }                                          // Exit if nothing to read.
                if (new_bytes < 0){ error("Couldn't read file into buffer"); }         // Handle errors.
                bytes_read += new_bytes;
                if( *read_pos == '*' ){
                    while( (*dyn_pos != '\0') && (bytes_read <= BUFFER_SIZE) ){
                        *read_pos = *dyn_pos;
                        read_pos++;
                        dyn_pos++;
                        bytes_read++;
                    }
                }
                else{
                    read_pos++;
                }
            }
        }
        
        // Write data into socket.
        void *p = buffers[id];
        while (bytes_read > 0) {
            int bytes_written = (int)write(connections[id], p, bytes_read);
            if (bytes_written <= 0){ error("Couldn't send file to client."); }
            bytes_read -= bytes_written;
            p += bytes_written;
        }
    }
    
    /* close file */
    close(input_file);
}

/* open file for reading only */
int file_open(char* file)
{
    return open(file, O_RDONLY);
}

/* get file size in bytes */
int file_size(char* file)
{
    struct stat st;
    stat(file, &st);
    return (int)st.st_size;
}

/* check if file exists */
int file_exists(char* file)
{
    if (0 == access(file, 0)) {
        return 1;
    }
    return 0;
}

/* build root dir + filename path at beginning of buffer */
void file_build_path(int id, char* packet)
{
    char* start = strnstr(packet, "/", 100);    // find beginning of filename
    char* end = strnstr(start, " ", 100);       // find end of filename
    int len = (int)(end - start);               // find length of filename
    char filename[len+1];                       // allocate storage
    strncpy(filename, start, len);              // copy filename, buffer about to be wiped
    filename[len] = '\0';                       // add null (strncpy doesn't)
    bzero(buffers[id], BUFFER_SIZE);            // clear buffer
    strncpy(buffers[id], dir, strlen(dir));     // copy root dir to buffer
    strncpy(buffers[id]+strlen(dir), filename, len+1);    // add filename to path
}

/* Builds root dir + "/mobile* + filename path at beginning of buffer */
void file_build_mobile_path(int id, char* packet)
{
    char* start = strnstr(packet, "/", 100);    // find beginning of filename
    char* end = strnstr(start, " ", 100);       // find end of filename
    int len = (int)(end - start) + 8;           // find length of filename + "/mobile"
    char filename[len+1];                       // allocate storage
    snprintf(filename, len, "/mobile%s",start); // add "/mobile" to file name
    //strncpy(filename, start, len);            // copy filename, buffer about to be wiped
    filename[len] = '\0';                       // add null (strncpy doesn't)
    bzero(buffers[id], BUFFER_SIZE);            // clear buffer
    strncpy(buffers[id], dir, strlen(dir));     // copy root dir to buffer
    strncpy(buffers[id]+strlen(dir), filename, len);    // add filename to path
}

/* builds URL of requested file based on packet's request line and "Host:" value */
char* url_build(int id, char* packet){
    char* start = strnstr(packet, "/go/", 100); // find beginning of hostname
    start += 4;                                  // move past "/go/"
    char* end = strnstr(start, " ", 100);       // find end of hostname
    int len = (int)(end - start);               // find length of filename
    char hostname[len+1];                       // allocate storage
    strncpy(hostname, start, len);              // copy hostname, buffer about to be wiped
    hostname[len] = '\0';                       // add null (strncpy doesn't)
    bzero(buffers[id], BUFFER_SIZE);            // clear buffer
    int url_components = 7 + 4 + 4 + 1;         // "http://" + "www." + ".com\0"
    snprintf(buffers[id], len+url_components, "http://www.%s.com",hostname);
    return buffers[id];
}

/* allocates client id */
int add_client()
{
    int e = 0;
    pthread_mutex_lock(&mutex);
    while( (avail[e] != NUM_THREADS) && (e < NUM_THREADS) ){    // find available id for client
        e++;
    }
    if( e < NUM_THREADS ){ avail[e] = e; }    // if id allocated, mark as used
    pthread_mutex_unlock(&mutex);
    if( e == NUM_THREADS ){
        return NUM_THREADS;
    }
    return e;
}

/* deallocates client id */
void sub_client(int id)
{
    pthread_mutex_lock(&mutex);
    avail[id] = NUM_THREADS;        // mark client ID as available
    pthread_mutex_unlock(&mutex);
}

/* deallocates client id */
int is_online()
{
    int x;
    pthread_mutex_lock(&mutex);
    x = online;
    pthread_mutex_unlock(&mutex);
    return x;
}

/* deallocates client id */
void go_offline()
{
    pthread_mutex_lock(&mutex);
    online = 0;
    pthread_mutex_unlock(&mutex);
}

/* Loads a line-separated list of mobile User-Agents from filename */
void load_mobile_ids(char* filename)
{
    mobile_ids = malloc(file_size(filename));
    if( (int)read(file_open(filename), mobile_ids, file_size(filename)) != file_size(filename)){
        error("Couldn't load mobile User-Agent ID's");
    }
}


/* Determine whether a client is a mobile user */
/* compares the list of mobile User-Agent ID's stored
   in mobile_ids to the User-Agent of packet. */
int mobile_user(char* packet)
{
    char* start = strstr(packet, "User-Agent: ");
    if( start == NULL ){ return 0; }
    start += 12;
    int len = 27;               // 27 characters is enough to detect mobile User-Agent, while disregarding
                                // specific browser versions/platform numbers
                                // Based on tests with iOS devices and Android, this threshold was necessary in order to
                                // make maintaning a list of User-Agents easier.
    char useragent[len+1];
    strncpy(useragent, start, len);
    useragent[len] = '\0';
    if( strstr(mobile_ids, useragent) == NULL){ return 0; }
    return 1;
}

/* Stop & join threads, close sockets, free malloc's, & exit. */
void cleanup_and_exit()
{
    printf("\nCleaning up...");
    go_offline();
    for( int i = 0; i < NUM_THREADS; i++ ){
        pthread_join(threads[i], NULL);
    }
    printf("\nthreads terminated...");
    for( int i = 0; i < NUM_THREADS; i++ ){
        shutdown(connections[i], 2);
    }
    shutdown(sock, 2);
    printf("\nsockets/connections closed...");
    free(dir);
    if( mobile_ids != NULL){ free(mobile_ids); }
    printf("\ngoodbye!\n");
    exit(0);
}

/* error handler */
void error(char *msg)
{
    perror(msg);
    exit(1);
}
