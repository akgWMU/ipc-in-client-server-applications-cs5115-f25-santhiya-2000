// server.c
// CS5115 PA6 — FIFO-based client/server arithmetic service
// This file implements a server that listens on a well-known named pipe
// (`/tmp/arith_req_fifo`) for fixed-size request structs. For each request
// it forks a child which computes the arithmetic result and writes a fixed
// response struct to the client's response FIFO (path provided in request).

#define _GNU_SOURCE
#include <stdio.h>      // fprintf, perror, FILE*, fopen, fclose
#include <stdlib.h>     // exit, atexit
#include <stdint.h>     // int64_t, int32_t
#include <stdbool.h>    // bool type
#include <string.h>     // memset, memcpy, strerror
#include <errno.h>      // errno values
#include <unistd.h>     // read, write, close, unlink, fork, getpid
#include <fcntl.h>      // open flags
#include <sys/stat.h>   // mkfifo
#include <time.h>       // time, localtime_r, strftime
#include <signal.h>     // sigaction
#include <stdarg.h>     // va_list, va_start, vfprintf
#include <sys/wait.h>   // waitpid

// Path for the server's well-known request FIFO
#define REQ_FIFO_PATH "/tmp/arith_req_fifo"
// Maximum sizes used in request/response structures
#define RESP_NAME_MAX 128
#define OP_MAX 4

// Request message the client writes into REQ_FIFO_PATH
typedef struct __attribute__((packed)) {
    char   operation[OP_MAX];         // "add","sub","mul","div" (not NUL-terminated necessarily)
    int64_t operand1;                // first operand
    int64_t operand2;                // second operand
    pid_t  client_pid;               // client's PID (informational)
    char   resp_fifo[RESP_NAME_MAX]; // path to client's response FIFO
} request_msg_t;

// Response message server writes back to client's FIFO
typedef struct __attribute__((packed)) {
    int64_t result;                  // arithmetic result
    int32_t success;                 // 1 => ok, 0 => error
    char    error[128];              // error message if success==0
} response_msg_t;

// Global file descriptors and log handle
static int   req_fd = -1;  // read end of the request FIFO
static int   dummy_w = -1; // write end kept open to avoid EOF on req_fd
static FILE *logf   = NULL; // server.log FILE*

// cleanup: close fds and remove request FIFO and close log
static void cleanup(void) {
    if (req_fd >= 0) close(req_fd);     // close request FIFO fd if open
    if (dummy_w >= 0) close(dummy_w);   // close dummy writer if opened
    unlink(REQ_FIFO_PATH);               // remove the FIFO file from the filesystem
    if (logf) { fclose(logf); logf = NULL; } // close the log file
}

// Convenience to print an error and exit
static void die(const char *msg) {
    perror(msg); // print lib call error
    exit(EXIT_FAILURE);
}

// Write a timestamped line to the server log (if opened)
static void log_line(const char *fmt, ...) {
    if (!logf) return;                  // no-op if log not available
    time_t t = time(NULL);              // current time
    struct tm tmv;
    localtime_r(&t, &tmv);              // thread-safe localtime
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv); // format timestamp

    fprintf(logf, "[%s] ", ts);       // write timestamp prefix
    va_list ap; va_start(ap, fmt);       // handle variable args
    vfprintf(logf, fmt, ap);            // write formatted message
    va_end(ap);
    fputc('\n', logf);                 // newline and flush
    fflush(logf);
}

// Signal handlers: SIGINT/TERM will trigger a clean shutdown via a flag
static volatile sig_atomic_t stop_requested = 0;
static void on_sigint(int sig){ (void)sig; stop_requested = 1; }
// Reap child processes to avoid zombies (safe to call waitpid in handler with WNOHANG)
static void on_sigchld(int sig){ (void)sig; int st; while (waitpid(-1,&st,WNOHANG)>0){} }

// Helper to read exactly n bytes or return an error/short read
static ssize_t read_full(int fd, void *buf, size_t n){
    size_t off=0;
    while(off<n){
        ssize_t r=read(fd,(char*)buf+off,n-off); // attempt to read remaining bytes
        if(r==0) return (ssize_t)off;            // EOF: return bytes read so far
        if(r<0){ if(errno==EINTR) continue; return -1; } // retry on EINTR
        off+=(size_t)r;                          // advance offset by bytes read
    }
    return (ssize_t)off;                         // success: n bytes read
}

// Helper to write exactly n bytes or return error
static ssize_t write_full(int fd, const void *buf, size_t n){
    size_t off=0;
    while(off<n){
        ssize_t w=write(fd,(const char*)buf+off,n-off); // write remaining bytes
        if(w<0){ if(errno==EINTR) continue; return -1; } // retry on EINTR
        off+=(size_t)w;                                // advance by bytes written
    }
    return (ssize_t)off;                              // success: n bytes written
}

// Compute the arithmetic operation requested and fill response struct
static void compute(const request_msg_t *rq, response_msg_t *rp){
    rp->success = 1; rp->error[0]='\0';   // assume success until an error occurs
    int64_t a=rq->operand1, b=rq->operand2;
    if      (memcmp(rq->operation,"add",3)==0) rp->result=a+b; // add
    else if (memcmp(rq->operation,"sub",3)==0) rp->result=a-b; // subtract
    else if (memcmp(rq->operation,"mul",3)==0) rp->result=a*b; // multiply
    else if (memcmp(rq->operation,"div",3)==0){                // divide
        if(b==0){ rp->success=0; snprintf(rp->error,sizeof(rp->error),"Divide by zero");}
        else rp->result=a/b;
    } else { rp->success=0; snprintf(rp->error,sizeof(rp->error),"Invalid operation"); }
}

int main(void){
    // Open server log for appending; die() if we can't open the log
    logf=fopen("server.log","a"); if(!logf) die("fopen log");
    atexit(cleanup); // ensure cleanup runs on normal exit

    // Install signal handlers: request stop on SIGINT/SIGTERM; reap children on SIGCHLD
    struct sigaction sa={0}; sa.sa_handler=on_sigint;  sigaction(SIGINT,&sa,NULL); sigaction(SIGTERM,&sa,NULL);
    struct sigaction sc={0}; sc.sa_handler=on_sigchld; sc.sa_flags=SA_RESTART|SA_NOCLDSTOP; sigaction(SIGCHLD,&sc,NULL);

    // Create the request FIFO if it doesn't already exist
    if (mkfifo(REQ_FIFO_PATH,0666)<0 && errno!=EEXIST) die("mkfifo request");

    // Open the request FIFO for reading; use a dummy writer to avoid EOF when no clients
    req_fd=open(REQ_FIFO_PATH,O_RDONLY);
    if(req_fd<0) die("open request fifo (read)");
    dummy_w=open(REQ_FIFO_PATH,O_WRONLY);
    if(dummy_w<0) die("open request fifo (dummy write)");

    fprintf(stderr,"[server] Listening on %s …\n", REQ_FIFO_PATH);
    log_line("Server started; listening on %s", REQ_FIFO_PATH);

    for(;;){
        // If a stop was requested by a signal handler, break out and exit cleanly
        if (stop_requested) break;

        request_msg_t rq; // buffer to read the next request
        ssize_t r=read_full(req_fd,&rq,sizeof(rq)); // block until a full request struct arrives
        if(r==0){ // reader got EOF because all writers closed
            close(req_fd); // close and re-open to continue receiving future writers
            req_fd=open(REQ_FIFO_PATH,O_RDONLY);
            if(req_fd<0) die("reopen");
            continue;
        }
        if(r<0){ if(errno==EINTR) continue; die("read request"); }
        if((size_t)r<sizeof(rq)){ log_line("Partial request (%zd bytes) ignored", r); continue; }

        // ---- PRINT: received request ----
        char opbuf[OP_MAX+1]={0}; memcpy(opbuf,rq.operation,OP_MAX); // make operation NUL-terminated for printing
        printf("[SERVER] recv from PID=%d : %s(%lld,%lld) -> resp=%s\n",
               (int)rq.client_pid, opbuf,
               (long long)rq.operand1, (long long)rq.operand2, rq.resp_fifo);
        fflush(stdout);

        log_line("Recv PID=%d op=%s a=%lld b=%lld resp=%s",
                 (int)rq.client_pid, opbuf,
                 (long long)rq.operand1,(long long)rq.operand2,rq.resp_fifo);

        // Fork a child to handle this request concurrently
        pid_t cpid=fork();
        if(cpid<0){
            // Fork failed: attempt to compute and send a response in the parent (best-effort)
            log_line("fork() failed: %s", strerror(errno));
            response_msg_t rp; compute(&rq,&rp);
            int resp_fd=open(rq.resp_fifo,O_WRONLY); // blocking open
            if(resp_fd>=0){ write_full(resp_fd,&rp,sizeof(rp)); close(resp_fd); }
            continue;
        }
        if(cpid==0){
            // Child: compute and respond, then exit
            response_msg_t rp; memset(&rp,0,sizeof(rp));
            compute(&rq,&rp);

            // ---- PRINT: computed result ----
            char cop[OP_MAX+1]={0}; memcpy(cop,rq.operation,OP_MAX);
            if(rp.success){
                printf("[SERVER child=%d] computed %s(%lld,%lld) = %lld\n",
                       (int)getpid(), cop,
                       (long long)rq.operand1, (long long)rq.operand2,
                       (long long)rp.result);
            } else {
                printf("[SERVER child=%d] computed %s(%lld,%lld) -> ERROR: %s\n",
                       (int)getpid(), cop,
                       (long long)rq.operand1, (long long)rq.operand2,
                       rp.error);
            }
            fflush(stdout);

            // Open client's response FIFO for writing (blocks until client opens read end)
            int resp_fd=open(rq.resp_fifo,O_WRONLY); // BLOCKS until client opens read end
            if(resp_fd<0){
                log_line("child(%d) open resp %s failed: %s",(int)getpid(),rq.resp_fifo,strerror(errno));
                printf("[SERVER child=%d] failed to open %s: %s\n",
                       (int)getpid(), rq.resp_fifo, strerror(errno));
                fflush(stdout);
                _exit(0);
            }
            if(write_full(resp_fd,&rp,sizeof(rp))<0){
                log_line("child(%d) write resp failed: %s",(int)getpid(),strerror(errno));
                printf("[SERVER child=%d] write to %s FAILED: %s\n",
                       (int)getpid(), rq.resp_fifo, strerror(errno));
            }else{
                // ---- PRINT: sent response ----
                printf("[SERVER child=%d] response sent to %s\n",
                       (int)getpid(), rq.resp_fifo);
            }
            fflush(stdout);
            close(resp_fd); // close the response FIFO writer fd
            _exit(0); // child exits without running parent's atexit handlers
        }
        // parent continues; children are reaped by SIGCHLD handler
    }
    return 0;
}
