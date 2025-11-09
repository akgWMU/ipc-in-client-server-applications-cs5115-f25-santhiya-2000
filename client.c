// client.c (fixed)
// Interactive client for the FIFO-based arithmetic server. The client creates
// a per-process response FIFO, sends a fixed-size request struct to the
// server's well-known request FIFO, then opens its response FIFO to receive
// exactly one fixed-size response struct.

#define _GNU_SOURCE
#include <stdio.h>      // printf, fprintf, fgets
#include <stdlib.h>     // exit
#include <stdint.h>     // int64_t
#include <stdbool.h>    // bool
#include <string.h>     // memset, strncpy, strlen
#include <errno.h>      // errno
#include <unistd.h>     // getpid, unlink, read, write, close
#include <fcntl.h>      // open flags
#include <sys/stat.h>   // mkfifo, stat
#include <sys/types.h>  // pid_t

#define REQ_FIFO_PATH "/tmp/arith_req_fifo" // server request FIFO path
#define RESP_NAME_MAX 128
#define OP_MAX 4

// Request struct mirrors the server's expectation
typedef struct __attribute__((packed)) {
    char   operation[OP_MAX];
    int64_t operand1;
    int64_t operand2;
    pid_t  client_pid;
    char   resp_fifo[RESP_NAME_MAX];
} request_msg_t;

// Response struct mirrors the server's response
typedef struct __attribute__((packed)) {
    int64_t result;
    int32_t success;
    char    error[128];
} response_msg_t;

// Read exactly n bytes or return -1 on error / short read (EOF)
static ssize_t read_full(int fd, void *buf, size_t n){
    size_t off=0;
    while(off<n){
        ssize_t r=read(fd,(char*)buf+off,n-off);
        if(r==0) return (ssize_t)off;           // EOF: return bytes read so far
        if(r<0){ if(errno==EINTR) continue; return -1; }
        off+=(size_t)r;
    }
    return (ssize_t)off; // success: exactly n bytes read
}

// Write exactly n bytes or return -1 on error
static ssize_t write_full(int fd, const void *buf, size_t n){
    size_t off=0;
    while(off<n){
        ssize_t w=write(fd,(const char*)buf+off,n-off);
        if(w<0){ if(errno==EINTR) continue; return -1; }
        off+=(size_t)w;
    }
    return (ssize_t)off; // success
}

// Remove trailing LF/CR from a string read by fgets
static void trim_newline(char *s){
    size_t n=strlen(s);
    while(n && (s[n-1]=='\n'||s[n-1]=='\r')) s[--n]='\0';
}

// Validate the textual operation the user typed
static bool is_valid_op(const char *op){
    return !strcmp(op,"add")||!strcmp(op,"sub")||!strcmp(op,"mul")||!strcmp(op,"div");
}

int main(void){
    // Construct a per-process response FIFO path in /tmp using PID
    char resp_fifo[RESP_NAME_MAX];
    snprintf(resp_fifo,sizeof(resp_fifo),"/tmp/arith_resp_%d.fifo",(int)getpid());

    // Create the FIFO if it does not exist. If it exists, mkfifo returns -1
    // with errno==EEXIST; we allow that case but we should verify the path
    // is indeed a FIFO (see notes in review).
    if (mkfifo(resp_fifo,0666)<0 && errno!=EEXIST){ perror("mkfifo resp"); return 1; }

    printf("Client ready. Type 'exit' to quit.\n");
    printf("Allowed operations: add, sub, mul, div\n\n");

    char line[64];
    for(;;){
        // Read operation from user
        printf("Enter operation (add/sub/mul/div or exit): "); fflush(stdout);
        if(!fgets(line,sizeof(line),stdin)) break; // EOF on stdin -> exit
        trim_newline(line);
        if(!strcmp(line,"exit")) break;
        if(!is_valid_op(line)){ printf("Invalid operation. Try again.\n"); continue; }

        // Read two integers from user
        long long a,b;
        printf("Enter two integers (e.g., 6 9): "); fflush(stdout);
        if (scanf("%lld %lld",&a,&b)!=2){
            int ch; while((ch=getchar())!='\n' && ch!=EOF){} // consume rest of invalid line
            printf("Invalid input. Please enter two integers.\n");
            continue;
        }
        int ch; while((ch=getchar())!='\n' && ch!=EOF){} // consume trailing input on the line

        // Build the request struct to send to the server
        request_msg_t rq; memset(&rq,0,sizeof(rq)); // zero to ensure fields are clean
        // copy operation (OP_MAX bytes). rq.operation is zeroed so NUL termination is ensured
        memcpy(rq.operation,line, OP_MAX < (int)strlen(line) ? OP_MAX : strlen(line));
        rq.operand1=(int64_t)a; rq.operand2=(int64_t)b; rq.client_pid=getpid();
        strncpy(rq.resp_fifo, resp_fifo, RESP_NAME_MAX-1); // ensure NUL-termination

        // Send request to server by opening the request FIFO in write-only mode.
        // This open will block if the server isn't running and has no reader.
        int req_fd=open(REQ_FIFO_PATH,O_WRONLY);
        if(req_fd<0){ perror("open request fifo"); continue; }
        if(write_full(req_fd,&rq,sizeof(rq))<0){ perror("write request"); close(req_fd); continue; }
        close(req_fd); // close the write end after sending the request

        // Now open our response FIFO and block until the server opens it for writing
        int rfd = open(resp_fifo, O_RDONLY);      // blocks until server opens writer & writes/close
        if(rfd<0){ perror("open resp fifo"); continue; }

        response_msg_t rp; memset(&rp,0,sizeof(rp));
        ssize_t rr = read_full(rfd,&rp,sizeof(rp));  // read exactly one response struct
        close(rfd);                                  // close after each transaction

        if(rr<0 || (size_t)rr<sizeof(rp)){
            fprintf(stderr,"Failed to read full response. Got %zd bytes.\n", rr);
            continue;
        }

        if(rp.success) printf("Result from server: %lld\n\n",(long long)rp.result);
        else           printf("Server error: %s\n\n", rp.error);
    }

    unlink(resp_fifo); // remove our response FIFO before exiting
    printf("Client exiting. Goodbye!\n");
    return 0;
}
