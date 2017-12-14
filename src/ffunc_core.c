#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>

#include <execinfo.h>
#include <unistd.h> /* needed to define getpid() */
#include <signal.h>
#include <setjmp.h>

#include <sys/wait.h>
#include <sys/types.h>
#include <fcgi_config.h>
#include <fcgiapp.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/socket.h>
#include "ffunc_core.h"

#define _get_param_(KEY) FCGX_GetParam(KEY, request->envp)

typedef int (*h)(FCGX_Request *, ffunc_session_t*);

struct ffunc_handler {
    char* name;
    h handle;
};

static int func_count = 0;
struct ffunc_handler *dyna_funcs = NULL;

static const long MAX_STDIN = 10240;
int no_debug(FILE *__restrict __stream, const char *__restrict __format, ...) {return 1;}

static __atomic_hash_n *thread_hash;
static int total_workers, remain_workers;
static void (*ffunc_shutdown_handler)(void);

int ffunc_init(char** ffunc_nmap_func);

void* Malloc_Function(size_t sz);
void Free_Function(void* v);
int strpos(const char *haystack, const char *needle);

void* multi_thread_accept(void* sock_ptr);
// void *thread_admin(void* sock);
// void *add_on_workers(void* sock);
int hook_socket(char* sock_port, int backlog, int workers, char** ffunc_nmap_func, void (*init_func)(void) );
sigset_t* handle_request(FCGX_Request *request);
// int init_error_signal(f_singal_t *ferr_signals);
void* ffunc_init_signal(void *v);
static char *appname = NULL; //global apps deployment
static struct sigaction sa;
static struct sigaction shutdown_sa;
static int isUpAndRunning = 1, fcgi_func_socket;
static void *usr_req_handle = NULL;
static FILE* logfile_writer = NULL;

void* Malloc_Function(size_t sz) {
    return malloc(sz);
}
void Free_Function(void* v) {
    // flog_info("%s\n", "FREE-------------- ");
    free(v);
}

static void* read_t(void *origin) {
    ffunc_session_t *_csession_ = malloc(sizeof(ffunc_session_t));

    memcpy(_csession_, origin, sizeof(ffunc_session_t));
    return _csession_;
}


static void free_t(void* node) {
    free(node);
}


int ffunc_init(char** ffunc_nmap_func) {
    ffunc_hash_set_alloc_fn(Malloc_Function, Free_Function);
    ffunc_buf_set_alloc_fn(Malloc_Function, Free_Function);


    usr_req_handle = NULL;
    char *error;
    usr_req_handle = dlopen(appname, RTLD_LAZY | RTLD_NOW);
    if (!usr_req_handle) {
        flog_err("%s\n", dlerror());
        flog_info("%s\n", "Have you included -rdynamic in your compiler option, for e.g gcc ... -rdynamic");
        return 0;
    }

    while (*(ffunc_nmap_func + func_count++));

    flog_info("total function = %d\n", --func_count); // Remove NULL
    dyna_funcs = malloc(func_count * sizeof(struct ffunc_handler));

    for (int f = 0; f < func_count; f++) {
        // flog_info("%d\n", f);
        // struct ffunc_handler *func = (struct ffunc_handler *)ffunc_map_assign(dyna_funcs, ffunc_nmap_func[f]);

        dyna_funcs[f].name = calloc((strlen(ffunc_nmap_func[f]) + 1), sizeof(char));

        strcpy(dyna_funcs[f].name, ffunc_nmap_func[f]);

        *(void **) (&dyna_funcs[f].handle) = dlsym(usr_req_handle, ffunc_nmap_func[f]);

        if ((error = dlerror()) != NULL) {
            flog_err("%s\n", error);
            flog_info("%s\n", "Have you included -rdynamic in your compiler option, for e.g gcc ... -rdynamic");
            return 0;
        }
    }
    return 1;

}

/**To prevent -std=99 issue**/
char *duplistr(const char *str) {
    int n = strlen(str) + 1;
    ffunc_session_t * csession = ffunc_get_session();
    char *dup = falloc(&csession->pool, n);
    memset(dup, 0, n);
    if (dup)
        memcpy(dup, str, n - 1);
    // dup[n - 1] = '\0';
    // please do not put flog_info here, it will recurve
    return dup;
}

int setup_logger(char* out_file_path) {
    flog_info("%s\n", "initialize log file");
    if ((logfile_writer = fopen(out_file_path, "ab+")) == NULL) {
        flog_info("%s\n", "fopen source-file");
        return 0;
    }

    int fd = fileno(logfile_writer);

    // dup2(fd, STDIN_FILENO);
    // dup2(fd, STDOUT_FILENO);
    // dup2(fd, STDERR_FILENO);

    // fclose(logfile_writer);
    if (dup2(fd, STDERR_FILENO) < 0) return 0;


    // if (freopen( out_file_path, "ab+", FLOGGER_ ) == NULL) {
    //     flog_err("%s\n", "fopen source-file");
    //     return 0;
    // }
    return 1;
}

void print_time(char* buff) {
    time_t now = time(NULL);
    strftime(buff, 20, "%Y-%m-%d %H:%M:%S", localtime(&now));
}

sigset_t* handle_request(FCGX_Request *request) {
    sigset_t *status_mask = NULL;

    // flog_info("dummy value = %d\n", dummy++) ;
    // usleep(1000 * 1000 * 5); // 5 secs sleep
    // Dl_info info;
    char *value;
    // int (*handler)(FCGX_Request *, ffunc_session_t*);
    // static void *usr_req_handle = NULL;
    char **env = request->envp;

    ffunc_pool *p = create_pool(DEFAULT_BLK_SZ);

    // fhash_lock(thread_table);

    ffunc_session_t *_csession_ = malloc(sizeof(ffunc_session_t)), *_rel_csession_;

    // ffunc_LF_put(&thread_table, pthread_self(), _csession_);
    __atomic_hash_n_put(thread_hash, pthread_self(), _csession_);

    // fhash_unlock(thread_table);
    _csession_->pool = p;
    _csession_->query_str = NULL;



    while (*(env) && strncmp(*env, "FN_HANDLER", 10) != 0) env++;
    value = (char*)(uintptr_t) * (env) + 11;

    if (value != NULL) {
        int i;
        for (i = 0; i < func_count; i++) {
            if (strcmp(dyna_funcs[i].name, value) == 0) {
                if (strcmp(_get_param_("REQUEST_METHOD"), "GET") == 0) {
                    char* query_string = duplistr(_get_param_("QUERY_STRING"));
                    if (!is_empty(query_string))
                        _csession_->query_str = query_string;//parse_json_fr_args(query_string);
                }

                // trying to catch back to life from error
                if (/*sig*/setjmp(_csession_->jump_err_buff) == 999) {
                    flog_err("%s\n", "Critical Error Found!!!!!!!!!!!!!!!!!!!!");
                    ffunc_session_t* csession = (ffunc_session_t *)__atomic_hash_n_read(thread_hash, pthread_self());
                    status_mask = csession->curr_mask;
                    free(csession); // This is read, you have to free
                    goto SKIP_METHOD;
                }
                dyna_funcs[i].handle(request, _csession_);

                goto RELEASE_POOL;
            }
        }

        flog_err("%s\n", "Method not found!!!!!!!!!!!!!!!!!!!!");
SKIP_METHOD:
        ffunc_write_http_status(request, 500);
        ffunc_write_out("Content-Type: text/plain\r\n\r\n");
        ffunc_write_out("%s\n", "Method not found or memory corruption");
        ffunc_write_out("%s\n", "check the log whether memory leak or method does not existed in your program");

    } else {
        ffunc_write_out("Content-Type: text/plain\r\n\r\n");
        ffunc_write_out("%s\n", "FN_HANDLER not found");
        ffunc_write_out("%s\n", "Please provide your FN_HANDLER in your config file");
        ffunc_write_out("%s\n", "For e.g nginx.conf fastcgi_param FN_HANDLER getProfile");
    }


RELEASE_POOL:
    _rel_csession_ = __atomic_hash_n_pop(thread_hash, pthread_self());
    if (_rel_csession_) { //free the _csession_
        destroy_pool(_rel_csession_->pool);
        free(_rel_csession_);
        // fprintf(stdout, "%s\n", "successfully free _csession_:");
        // fprintf(stdout, "%s\n", "successfully freed");
        // so far all are freed.
        // fhash_unlock(thread_table);
    } else {
        destroy_pool(p);
        // fhash_unlock(thread_table);
    }
    return status_mask;
}

size_t slen(const char* str) {
    register const char *s;
    for (s = str; *s; ++s);
    return (s - str);
}

int ffunc_isspace(const char* s) {
    return (*s == SPACE || *s == TAB || *s == NEW_LINE ||
            *s == VERTICAL_TAB || *s == FF_FEED || *s == CARRIAGE_RETURN ? 1 : 0);
    // return (*s == '\t' || *s == '\n' ||
    //         *s == '\v' || *s == '\f' || *s == '\r' || *s == ' ' ? 1 : 0);
}

int is_empty(char *s) {
    if (s && s[0])
        return 0;
    return 1;
}

int file_existed(const char* fname) {
    if ( access( fname, F_OK ) != -1 )
        return 1;
    else
        return 0;
}

int strpos(const char *haystack, const char *needle)
{
    char *p = strstr(haystack, needle);
    if (p)
        return p - haystack;
    return -1;   // Not found = -1.
}

void* ffunc_getParam(const char *key, char* query_str) {
    int len, pos;
    char *qs = query_str;
    if (key && *key && qs && *qs) {
        len = strlen(key);
        do {
            if ((pos = strpos(qs, key)) < 0) return NULL;

            if (pos == 0 || qs[pos - 1] == '&') {
                qs = (char*)qs + pos + len;
                if (*qs++ == '=') {
                    char *src = qs,
                          *ret;
                    size_t sz = 0;
                    while (*qs && *qs++ != '&')sz++;

                    ffunc_session_t * csession = ffunc_get_session();
                    ret = falloc(&csession->pool, sz + 1);
                    memset(ret, 0, sz + 1);
                    return memcpy(ret, src, sz);
                } else while (*qs && *qs++ != '&');
            } else while (*qs && *qs++ != '&');
        } while (*qs);
    }
    return NULL;
}

long ffunc_readContent(FCGX_Request *request, char** content) {
    char* clenstr = _get_param_("CONTENT_LENGTH");
    FCGX_Stream *in = request->in;
    long len;

    if (clenstr) {
        // long int strtol (const char* str, char** endptr, int base);
        // endptr -- Reference to an object of type char*, whose value is set by the function to the next character in str after the numerical value.
        // This parameter can also be a null pointer, in which case it is not used.
        len = strtol(clenstr, &clenstr, 10);
        /* Don't read more than the predefined limit  */
        if (len > MAX_STDIN) { len = MAX_STDIN; }

        ffunc_session_t * csession = ffunc_get_session();
        *content = falloc(&csession->pool, len + 1);
        memset(*content, 0, len + 1);
        len = FCGX_GetStr(*content, len, in);
    }

    /* Don't read if CONTENT_LENGTH is missing or if it can't be parsed */
    else {
        *content = 0;
        len = 0;
    }

    /* Chew up whats remaining (required by mod_fastcgi) */
    while (FCGX_GetChar(in) != -1);

    return len;
}

ffunc_session_t *ffunc_get_session(void) {
    return (ffunc_session_t *)__atomic_hash_n_get(thread_hash, pthread_self());
}

int init_socket(char* sock_port, int backlog, int workers, int forkable, int signalable, int debugmode, char* logfile, char* solib, char** ffunc_nmap_func, void (*init_func)(void), void (*shutdown_func)(void)) {
    // For Shutdown Handler
    ffunc_shutdown_handler = shutdown_func;

    if (!ffunc_nmap_func[0]) {
        flog_info("%s\n", "ffunc_nmap_func has no defined...");
        return 1;
    }

    if (debugmode) {
        ffunc_dbg = fprintf;
    }
    appname = solib;
    pid_t childPID;
    // FCGX_Init();
    // if ((logfile != NULL) && (logfile[0] == '\0'))setup_logger(logfile);
    if (logfile && logfile[0]) {
        setup_logger(logfile);
    }

    flog_info("%s\n", "Starting TCP Socket");
    flog_info("sock_port=%s, backlog=%d\n", sock_port, backlog);


    int *has_error_handler = malloc(sizeof(int));
    *has_error_handler = signalable;

    if (forkable) {
        childPID = fork();
        if (childPID >= 0) // fork was successful
        {
            if (childPID == 0) // child process
            {
                pthread_t pth;
                pthread_create(&pth, NULL, ffunc_init_signal, has_error_handler);

                pthread_detach(pth);

                return hook_socket(sock_port, backlog, workers, ffunc_nmap_func, init_func);
            } else {}//Parent process
        } else {// fork failed
            flog_info("%s\n", "Fork failed..");
            return 1;
        }
    } else {
        pthread_t pth;
        pthread_create(&pth, NULL, ffunc_init_signal, has_error_handler);
        pthread_detach(pth);

        return hook_socket(sock_port, backlog, workers, ffunc_nmap_func, init_func);
    }
    return 0;
}

int hook_socket(char* sock_port, int backlog, int workers, char** ffunc_nmap_func, void (*init_func)(void)) {

    FCGX_Init();
    if (!ffunc_init(ffunc_nmap_func)) {
        exit(1);
    }

    remain_workers = 0;
    total_workers = workers;

    if (sock_port) {
        if (backlog)
            fcgi_func_socket = FCGX_OpenSocket(sock_port, backlog);
        else
            fcgi_func_socket = FCGX_OpenSocket(sock_port, 50);
    } else {
        flog_info("%s\n", "argument wrong");
        exit(1);
    }

    if (fcgi_func_socket < 0) {
        flog_info("%s\n", "unable to open the socket" );
        exit(1);
    }

    flog_info("%d workers in process\n", workers);
    thread_hash = __atomic_hash_n_init(workers + 10, read_t, free_t);


    if (workers == 1) {
        FCGX_Request request;
        FCGX_InitRequest(&request, fcgi_func_socket, FCGI_FAIL_ACCEPT_ON_INTR);

        flog_info("Socket on hook %s", sock_port);

        while (FCGX_Accept_r(&request) >= 0) {
            handle_request(&request);
            FCGX_Finish_r(&request);
        }
    } else {
        int i;

        pthread_t pth_workers[total_workers];
        for (i = 0; i < total_workers; i++) {
            pthread_create(&pth_workers[i], NULL, multi_thread_accept, &fcgi_func_socket);
            __atomic_fetch_add(&remain_workers, 1, __ATOMIC_ACQUIRE);
            pthread_detach(pth_workers[i]);
        }

        init_func();

BACKTO_WORKERS:
        while (remain_workers == total_workers)
            sleep(1);

        if (isUpAndRunning) {

            // for (i = 0; i < total_workers; i++) {
            //     pthread_kill(pth_workers[i], SIGTSTP);
            // }
            pthread_t worker;
            pthread_create(&worker, NULL, multi_thread_accept, &fcgi_func_socket);
            __atomic_fetch_add(&remain_workers, 1, __ATOMIC_ACQUIRE);
            pthread_detach(worker);

            flog_info("%s\n", "BACKTO_WORKERS...");

            goto BACKTO_WORKERS;
        }

    }

    if (sock_port)
        free(sock_port);
    flog_info("%s\n", "Exiting");
    return EXIT_SUCCESS;
}

void *multi_thread_accept(void* sock_ptr) {
    int rc;
    int sock = *(int*) sock_ptr;

    FCGX_Request request;
    if (FCGX_InitRequest(&request, sock, FCGI_FAIL_ACCEPT_ON_INTR) != 0) {
        flog_info("%s\n", "Can not init request");
        return NULL;
    }
    while (isUpAndRunning) {
        static pthread_mutex_t accept_mutex = PTHREAD_MUTEX_INITIALIZER; // this is lock for all threads

        pthread_mutex_lock(&accept_mutex);
        rc = FCGX_Accept_r(&request);
        pthread_mutex_unlock(&accept_mutex);
        if (rc < 0) {
            flog_info("%s\n", "Cannot accept new request");
            __atomic_fetch_sub(&remain_workers, 1, __ATOMIC_RELEASE);
            FCGX_Finish_r(&request); // this will free all the fcgiparams memory and request

            goto EXIT_WORKER;
        }

        sigset_t *_mask_ = handle_request(&request);

        FCGX_Finish_r(&request); // this will free all the fcgiparams memory and request
        /***If _mask_ is not NULL, means there is a error signal, need to suspend the thread completely **/
        if (_mask_) {
            // For detached threads, the system recycles its underlying resources automatically after the thread terminates.
            __atomic_fetch_sub(&remain_workers, 1, __ATOMIC_RELEASE);
            // flog_info("%s\n", "Detached");
            // sigsuspend(_mask_);
            pthread_exit(NULL);
            return NULL;
        }
    }
EXIT_WORKER:
    pthread_exit(NULL);
    return NULL;
}

/***Error Signal Handler Scope***/
void error_handler(int signo);
void shut_down_handler(int signo);
void _backtrace(int fd, int signo,  int stack_size);


void _backtrace(int fd, int signo,  int stack_size) {
    // void *buffer = ngx_pcalloc(ngx_cycle->pool, sizeof(void *) * stack_size);
    void *buffer[stack_size];
    int size = 0;

    // if (buffer != NULL) {
    // goto KillProcess;
    size = backtrace(buffer, stack_size);
    backtrace_symbols_fd(buffer, size, fd);
    // }
    // size = backtrace(buffer, stack_size);
    // backtrace_symbols_fd(buffer, size, fd);

// KillProcess:
//     kill(getpid(), signo);
}

void shut_down_handler(int signo) {
    if (isUpAndRunning) {
        isUpAndRunning = 0;
        
        flog_info("%s\n", "Shutting Down...");
        
        if (ffunc_shutdown_handler) {
            ffunc_shutdown_handler();
        }


        FCGX_ShutdownPending();

        /** releasing all the workers **/
// #ifdef __APPLE__
//         close(fcgi_func_socket);
// #else
//         shutdown(fcgi_func_socket, 2);
// #endif
        shutdown(fcgi_func_socket, 2);
        close(fcgi_func_socket);


        while (remain_workers > 0) {
            sleep(1); // Waiting for 3 sec for other pending thread to exit
            // flog_info("worker left %d\n", remain_workers);
        }

        if (usr_req_handle)  {
            if (dlclose(usr_req_handle) != 0) {
                flog_err("Error while shutting down %s\n", dlerror());
            }
        }
        __atomic_hash_n_destroy(thread_hash);

        int f;
        for (f = 0; f < func_count; f++) {
            free(dyna_funcs[f].name);
        }
        free(dyna_funcs);

        if (logfile_writer)
            fclose(logfile_writer);


        if (appname)
            free(appname); // free lib app name


        sleep(3);
    }
}

void error_handler(int signo) {
    ffunc_session_t *_csession_ = ffunc_get_session();

    if (isUpAndRunning) {
        _csession_->curr_mask = &sa.sa_mask;
    } else {
        _csession_->curr_mask = NULL;
    }

    int fd = fileno(FLOGGER_);
    fseek(FLOGGER_, 0, SEEK_END);
    _backtrace(fd, signo, 10);

    /*sig*/longjmp(_csession_->jump_err_buff, 999);

}

void* ffunc_init_signal(void *v) {
    flog_info("%s\n", "initializing...");

    int has_error_handler = *((int *) v);

    if (has_error_handler) {
        memset(&sa, 0, sizeof(struct sigaction));
        sa.sa_handler = error_handler;
        sigemptyset(&sa.sa_mask);

        sigaction(SIGABRT, &sa, NULL);
        sigaction(SIGFPE, &sa, NULL);
        sigaction(SIGILL, &sa, NULL);
        sigaction(SIGIOT, &sa, NULL);
        sigaction(SIGSEGV, &sa, NULL);
#ifdef SIGBUS
        sigaction(SIGBUS, &sa, NULL);
#endif
    }

    free(v);

    /** Shut down handler **/
    memset(&shutdown_sa, 0, sizeof(struct sigaction));
    shutdown_sa.sa_handler = shut_down_handler;
    sigemptyset(&shutdown_sa.sa_mask);
    // sigaction(SIGKILL, &shutdown_sa, NULL);
    sigaction(SIGINT, &shutdown_sa, NULL);
    sigaction(SIGQUIT, &shutdown_sa, NULL);


    flog_info("%s\n", "Please do not stop the process while service is starting...");


    while (remain_workers != total_workers)
        sleep(1);

    flog_info("%s\n", "Service has started");

    while (remain_workers > 0) {
        sleep(1);
    }

    pthread_exit(NULL);
}

void ffunc_write_http_status(FCGX_Request * request, uint16_t code) {

    switch (code) {
    case 200:
        ffunc_write_out("Status: 200 OK\r\n");
        break;
    case 204:
        ffunc_write_out("Status: 204 No Content\r\n");
        break;

    case 500:
        ffunc_write_out("Status: 500 Internal Server Error\r\n");
        break;

    default:
        ffunc_write_out("Status: 200 OK\r\n");
        break;
    }
}

void ffunc_write_default_header(FCGX_Request * request) {
    ffunc_write_out("Content-Type: application/x-www-form-urlencoded\r\n\r\n");
}

void ffunc_write_jsonp_header(FCGX_Request * request) {
    ffunc_write_out("Content-Type: application/javascript\r\n\r\n");
}

void ffunc_write_json_header(FCGX_Request * request) {
    ffunc_write_out("Content-Type: application/json\r\n\r\n");
}