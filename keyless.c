// keyless.c: TLSv1.2 server for the CloudFlare Keyless SSL
// protocol
//
// Copyright (c) 2013-2014 CloudFlare, Inc.

#include "kssl.h"
#include "kssl_helpers.h"

#if PLATFORM_WINDOWS
#include <winsock2.h>
#else
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/ip.h>
#include <getopt.h>
#include <glob.h>
#include <pwd.h>
#include <grp.h>
#endif
#include <fcntl.h>
#include <uv.h>

#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/conf.h>
#include <openssl/engine.h>

#include <stdarg.h>

#include "kssl_getopt.h"

#include "kssl_log.h"
#include "kssl_private_key.h"
#include "kssl_core.h"
#include "kssl_thread.h"

// This defines argv[0] without the calling path
#define PROGRAM_NAME "keyless"

// ssl_error: call when a fatal SSL error occurs. Exits the program
// with return code 1.
void ssl_error(void)
{
  ERR_print_errors_fp(stderr);
  exit(1);
}

// fatal_error: call to print an error message to STDERR. Exits the
// program with return code 1.
void fatal_error(const char *fmt, ...)
{
  va_list l;
  va_start(l, fmt);
  vfprintf(stderr, fmt, l);
  va_end(l);
  fprintf(stderr, "\n");
  exit(1);
}

// log_ssl_error: log an SSL error and clear the OpenSSL error buffer
void log_ssl_error(SSL *ssl, int rc)
{
  const char *err = ERR_error_string(SSL_get_error(ssl, rc), 0);
  write_log(1, "SSL error: %s", err);
  ERR_clear_error();
}

// log_err_error: log an OpenSSL error and clear the OpenSSL error buffer
void log_err_error(void)
{
  const char *err = ERR_error_string(ERR_get_error(), 0);
  write_log(1, "SSL error: %s", err);
  ERR_clear_error();
}

// error_string: converts an error return code from libuv into
// a string error message
const char *error_string(int e) 
{
  // All the libuv specific error codes (see uv-errno.h) are less than or
  // equal to -3000. Other error codes are from the system.

  if (e <= -3000) {
    return uv_strerror(e);
  }

  return strerror(-e);
}

// This structure is used to store a private key and the SHA256 hash
// of the modulus of the public key which it is associated with.
pk_list privates = 0;
char *pk_dir = NULL;
uv_rwlock_t *pk_lock;
SSL_CTX *g_ctx;

// Load all the private keys found in the pk_dir. This only
// looks for files that end with .key and the part before the .key is taken
// to be the DNS name.
static void load_private_keys(SSL_CTX *ctx)
{
  char *pattern;
  int privates_count, i;
#if PLATFORM_WINDOWS
  WIN32_FIND_DATA FindFileData;
  HANDLE hFind;
  const char *starkey = "\\*.key";
#else
  int rc;
  glob_t g;
  const char *starkey = "/*.key";
#endif
  uv_rwlock_wrlock(pk_lock);

  pattern = (char *)malloc(strlen(pk_dir) + strlen(starkey) + 1);
  if (pattern == NULL) {
    SSL_CTX_free(ctx);
    fatal_error("Memory allocatione error");
  }
  strcpy(pattern, pk_dir);
  strcat(pattern, starkey);

#if PLATFORM_WINDOWS
  hFind = FindFirstFile(pattern, &FindFileData);
  if (hFind == INVALID_HANDLE_VALUE) {
    SSL_CTX_free(ctx);
    fatal_error("Error %d finding private keys in %s", hFind, pk_dir);
  }

  // count the number of files
  privates_count = 1;
  while (FindNextFile(hFind, &FindFileData) != 0) {
    privates_count++;
  }
  FindClose(hFind);

  privates = new_pk_list(privates_count);
  if (privates == NULL) {
    SSL_CTX_free(ctx);
    fatal_error("Failed to allocate room for private keys");
  }

  hFind = FindFirstFile(pattern, &FindFileData);
  for (i = 0; i < privates_count; ++i) {
    char *path = (char *)malloc(strlen(pk_dir) + 1 +
                                strlen(FindFileData.cFileName) + 1);
    strcpy(path, pk_dir);
    strcat(path, "\\");
    strcat(path, FindFileData.cFileName);
    if (add_key_from_file(path, privates) != 0) {
      SSL_CTX_free(ctx);
      fatal_error("Failed to add private keys");
    }
    FindNextFile(hFind, &FindFileData);
    free(path);
  }
  FindClose(hFind);
#else
  g.gl_pathc  = 0;
  g.gl_offs   = 0;

  rc = glob(pattern, GLOB_NOSORT, 0, &g);

  if (rc != 0) {
    SSL_CTX_free(ctx);
    fatal_error("Error %d finding private keys in %s", rc, pk_dir);
  }

  if (g.gl_pathc == 0) {
    SSL_CTX_free(ctx);
    fatal_error("Failed to find any private keys in %s", pk_dir);
  }

  privates_count = g.gl_pathc;
  privates = new_pk_list(privates_count);
  if (privates == NULL) {
    SSL_CTX_free(ctx);
    fatal_error("Failed to allocate room for private keys");
  }

  for (i = 0; i < privates_count; ++i) {
    write_log(0, "loading key: %s", g.gl_pathv[i]);
    if (add_key_from_file(g.gl_pathv[i], privates) != 0) {
      SSL_CTX_free(ctx);
      fatal_error("Failed to add private keys");
    }
  }

  globfree(&g);
#endif

  free(pattern);
  uv_rwlock_wrunlock(pk_lock);
}

// This defines the maximum number of workers to create

#define DEFAULT_WORKERS 1
#define MAX_WORKERS 32

int num_workers = DEFAULT_WORKERS;

worker_data worker[MAX_WORKERS];

// This is the TCP connection on which we listen for TLS connections

uv_tcp_t tcp_server;

// sighup_cb: handle SIGHUP and reload files on disk.
void sighup_cb(uv_signal_t *w, int signum)
{
  free_pk_list(privates);
  load_private_keys(g_ctx);
}

// sigterm_cb: handle SIGTERM and terminates program cleanly. The
// actual termination is handled in main once the uv_run has
// exited. That will happen when this is called because we call
// uv_signal_stop which is the last event handler running in the main
// thread.
void sigterm_cb(uv_signal_t *w, int signum)
{
  int rc = uv_signal_stop(w);
  uv_close((uv_handle_t *)w, NULL);
  if (rc != 0) {
    write_log(1, "Failed to stop SIGTERM handler: %s",
              error_string(rc));
  }
}

void sigpipe_cb(uv_signal_t *w, int signum)
{
  write_log(1, "Received SIGPIPE signal");
}

// thread_stop_cb: called via async_* to stop a thread
void thread_stop_cb(uv_async_t *handle)
{
  worker_data *worker = (worker_data *)handle->data;

  uv_close((uv_handle_t *)&worker->server, NULL);
  uv_close((uv_handle_t *)&worker->stopper, NULL);
}

typedef struct {
  uv_pipe_t pipe;
  uv_tcp_t *handle;
  uv_connect_t connect_req;
} ipc_client;

// ipc_client_close_cb: called when the client has finished reading the
// server handle from the pipe and has called uv_close()
void ipc_client_close_cb(uv_handle_t *handle)
{
  ipc_client *client = (ipc_client *)handle->data;
  free(client);
}

// ipc_read2_cb: data (the TCP server handle) ready to read on the pipe.
// Read the handle and close the pipe.
void ipc_read2_cb(uv_stream_t *handle,
                  ssize_t nread,
                  const uv_buf_t *buf)
{
  uv_pipe_t *pipe = (uv_pipe_t *)handle;
  uv_loop_t *loop = pipe->loop;
  ipc_client *client = (ipc_client *)pipe->data;

  if (uv_pipe_pending_count(pipe) == 1) {
    uv_handle_type type = uv_pipe_pending_type(pipe);

    if (type == UV_TCP) {
      int rc = uv_tcp_init(loop, client->handle);
      if (rc != 0) {
        write_log(1, "Failed to create TCP handle in thread: %s",
            error_string(rc));
      } else {
        rc = uv_accept(handle, (uv_stream_t *)client->handle);
        if (rc != 0) {
          write_log(1, "Failed to uv_accept in thread: %s",
              error_string(rc));
        }
      }
    } else {
      write_log(1, "Wrong handle type in IPC");
    }
  } else {
      write_log(1, "No handles despite ipc_read_cb");
  }

  uv_close((uv_handle_t *)&client->pipe, NULL);
}

// ipc_connect_cb: call when a thread has made a connection to the IPC
// server. Just reads the TCP server handle.
void ipc_connect_cb(uv_connect_t *req, int status)
{
  ipc_client *client = (ipc_client *)req->data;
  int rc = uv_read_start((uv_stream_t *)&client->pipe, allocate_cb,
                          ipc_read2_cb);
  if (rc != 0) {
    write_log(1, "Failed to begin reading on pipe: %s",
              error_string(rc));
  }
}

#if PLATFORM_WINDOWS
#define PIPE_NAME "\\\\.\\pipe\\cloudflare-keyless"
#else
#define PIPE_NAME "/tmp/cloudflare-keyless"
#endif

// get_handle: retrieves the handle of the TCP server. Returns 0 on
// failure.
int get_handle(uv_loop_t *loop, uv_tcp_t *server)
{
  ipc_client *client = (ipc_client *)malloc(sizeof(ipc_client));
  int rc;

  client->handle = server;
  client->connect_req.data = (void *)client;

  rc = uv_pipe_init(loop, &client->pipe, 1);
  if (rc != 0) {
    write_log(1, "Failed to initialize client pipe: %s",
              error_string(rc));
    return 1;
  }

  client->pipe.data = (void *)client;
  uv_pipe_connect(&client->connect_req, &client->pipe,
                  PIPE_NAME, ipc_connect_cb);
  uv_run(loop, UV_RUN_DEFAULT);

  return 0;
}

// thread_entry: starts a new thread and begins listening for
// connections. Before listening it obtains the server handle from
// the main thread.
void thread_entry(void *data)
{
  worker_data *worker = (worker_data *)data;
  uv_loop_t *loop = uv_loop_new();
  int rc;

  // The stopper is used to terminate the thread gracefully. The
  // uv_unref is here so that if the thread has terminated the
  // async event doesn't keep the loop alive.

  worker->stopper.data = (void *)worker;
  rc = uv_async_init(loop, &worker->stopper, thread_stop_cb);
  if (rc != 0) {
    write_log(1, "Failed to create async in thread: %s",
              error_string(rc));
    uv_loop_delete(loop);
    return;
  }
  uv_unref((uv_handle_t *)&worker->stopper);

  // Wait for the main thread to be ready and obtain the
  // server handle

  uv_sem_wait(&worker->semaphore);
  rc = get_handle(loop, &worker->server);
  uv_sem_post(&worker->semaphore);

  if (rc == 0) {
    worker->server.data = (void *)worker;
    worker->active = 0;

    rc = uv_listen((uv_stream_t *)&worker->server, SOMAXCONN,
                   new_connection_cb);
    if (rc != 0) {
      write_log(1, "Failed to listen on socket in thread: %s",
                error_string(rc));
    }

    uv_run(loop, UV_RUN_DEFAULT);
  }

  uv_loop_delete(loop);
}

// cleanup: clean up state.
void cleanup(uv_loop_t *loop, SSL_CTX *ctx, pk_list privates)
{
  SSL_CTX_free(ctx);

  free_pk_list(privates);

  // This monstrous sequence of calls is attempting to clean up all
  // the memory allocated by SSL_library_init() which has no analagous
  // SSL_library_free()!

  CONF_modules_unload(1);
  EVP_cleanup();
  ENGINE_cleanup();
  CRYPTO_cleanup_all_ex_data();
  ERR_remove_state(0);
  ERR_free_strings();

  uv_loop_delete(loop);

  uv_rwlock_destroy(pk_lock);
  free(pk_dir);
  free(pk_lock);
}

typedef struct {
  uv_pipe_t pipe;
  uv_tcp_t *server;
  int connects;
} ipc_server;

typedef struct {
  uv_pipe_t pipe;
  uv_write_t write_req;
} ipc_peer;

// ipc_close_cb: called when the uv_close in ipc_write_cb has
// completed and frees memory allocated for the peer connection.
void ipc_close_cb(uv_handle_t *handle)
{
  ipc_peer *peer = (ipc_peer *)handle->data;
  free(peer);
}

// ipc_write_cb: called when the uv_write2 (sending the handle)
// completes. Just closes the connection to the peer (i.e. the
// thread).
void ipc_write_cb(uv_write_t *req, int status)
{
  ipc_peer *peer = (ipc_peer *)req->data;
  uv_close((uv_handle_t *)&peer->pipe, ipc_close_cb);
}

// ipc_connection_cb: called when a connection is made to the IPC
// server. Connections come from worker threads requesting the listen
// handle.
void ipc_connection_cb(uv_stream_t *pipe, int status)
{
  ipc_server *server = (ipc_server *)pipe->data;
  ipc_peer *peer = (ipc_peer *)malloc(sizeof(ipc_peer));
  uv_loop_t *loop = pipe->loop;
  uv_buf_t buf = uv_buf_init("ABCD", 4);
  int rc;

  // Accept the connection on the pipe and immediately write the
  // server handle to it using uv_write2 to send a handle

  rc = uv_pipe_init(loop, (uv_pipe_t *)&peer->pipe, 1);
  if (rc != 0) {
    write_log(1, "Failed to create client pipe: %s",
              error_string(rc));
  } else {
    rc = uv_accept(pipe, (uv_stream_t *)&peer->pipe);
    if (rc != 0) {
      write_log(1, "Failed to accept pipe connection: %s",
                error_string(rc));
    } else {
      peer->write_req.data = (void *)peer;
      peer->pipe.data = (void *)peer;
      rc = uv_write2(&peer->write_req, (uv_stream_t *)&peer->pipe,
                     &buf, 1, (uv_stream_t *)server->server,
                     ipc_write_cb);
      if (rc != 0) {
        write_log(1, "Failed to write server handle to pipe: %s",
                  error_string(rc));
      }
    }
  }

  // Decrement the connection counter. Once this reaches 0 it indicates
  // that every thread has connected and obtained the server handle so
  // the IPC server can be terminated.

  server->connects -= 1;
  if (server->connects == 0) {
    uv_close((uv_handle_t *)pipe, NULL);
  }
}

uv_mutex_t *locks;

// thread_id_cb: used by OpenSSL to get the currently running thread's
// ID
unsigned long thread_id_cb(void)
{
  return uv_thread_self();
}

// locking_cb: used by OpenSSL to lock its internal data
void locking_cb(int mode, int type, const char *file, int line)
{
  if (mode & CRYPTO_LOCK) {
    uv_mutex_lock(&locks[type]);
  } else {
    uv_mutex_unlock(&locks[type]);
  }
}

// write_pid: write the current process PID to the file in
// pid_file. This can be null.
void write_pid(char *pid_file, int pid, int write)
{
  if (pid_file) {
    int ok = 1;
    FILE *fp = fopen(pid_file, write?"w":"a");
    if (fp) {
      if (write) {
        if (fprintf(fp, "%d\n", pid) < 0) {
          ok = 0;
        }
      }
      if (fclose(fp) != 0) {
        ok = 0;
      }
    } else {
      ok = 0;
    }

    if (!ok) {
      fatal_error("Can't write to pid file %s", pid_file);
    }
  }
}

int main(int argc, char *argv[])
{
  int port = 2407;
  int help = 0;
  char *server_cert = 0;
  char *server_key = 0;
  char *private_key_directory = 0;
  const char *cipher_list = "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384";
  const char *ec_curve_name = "prime256v1";

  char *ca_file = 0;
  char *pid_file = 0;
  int parsed;

  const SSL_METHOD *method;
  SSL_CTX *ctx;
#if !PLATFORM_WINDOWS
  char *usergroup = 0;
  char *user = 0;
  char *group = 0;
  struct passwd * pwd = 0;
  struct group * grp = 0;
  int daemon = 0;
#endif

  int rc, i;
  struct sockaddr_in addr;
  STACK_OF(X509_NAME) *cert_names;
  uv_loop_t *loop;
  uv_signal_t sigterm_watcher;
  uv_signal_t sighup_watcher;
  ipc_server *p;

  // If this is set to 1 (by the --test command-line option) then the program
  // will do all work necessary to start but not actually start. The return
  // code will be 0 if start up would have been successful without --test.

  int test_mode = 0;

  const struct option long_options[] = {
    {"port",                  required_argument, 0, 0},
    {"server-cert",           required_argument, 0, 1},
    {"server-key",            required_argument, 0, 2},
    {"private-key-directory", required_argument, 0, 3},
    {"ca-file",               required_argument, 0, 4},
    {"silent",                no_argument,       0, 5},
    {"verbose",               no_argument,       0, 6},
    {"pid-file",              required_argument, 0, 7},
    {"num-workers",           optional_argument, 0, 8},
    {"help",                  no_argument,       0, 9},
    {"ip",                    required_argument, 0, 10},
#if !PLATFORM_WINDOWS
    {"user",                  required_argument, 0, 11},
    {"daemon",                no_argument,       0, 12},
    {"syslog",                no_argument,       0, 13},
    {"version",               no_argument,       0, 14},
#endif
    {"test",                  no_argument,       0, 15},
    {0,                       0,                 0, 0}
  };


  // This is set up here because it may be overriden by the --ip option which
  // is about to be parsed. That option is optional and this sets the default.

  addr.sin_addr.s_addr = INADDR_ANY;

  parsed = 0;
  optind = 1;
  while (!parsed) {
    switch (getopt_long(argc, argv, "", long_options, 0)) {
    case -1:
      parsed = 1;
      break;

    case '?':
      exit(1);
      break;

    case 0:
      port = atoi(optarg);
      break;

    case 1:
      server_cert = (char *)malloc(strlen(optarg)+1);
      strcpy(server_cert, optarg);
      break;

    case 2:
      server_key = (char *)malloc(strlen(optarg)+1);
      strcpy(server_key, optarg);
      break;

    case 3:
      private_key_directory = (char *)malloc(strlen(optarg)+1);
      strcpy(private_key_directory, optarg);
      break;

    case 4:
      ca_file = (char *)malloc(strlen(optarg)+1);
      strcpy(ca_file, optarg);
      break;

    case 5:
      silent = 1;
      break;

    case 6:
      verbose = 1;
      break;

    case 7:
      pid_file = (char *)malloc(strlen(optarg)+1);
      strcpy(pid_file, optarg);
      break;

    case 8:
      num_workers = atoi(optarg);
      break;

    case 9:
      help = 1;
      break;

    case 10:
      if (inet_pton(AF_INET, optarg, &addr.sin_addr) != 1) {
        fatal_error("The --ip parameter must be a valid IPv4 address");
      }
      break;

#if !PLATFORM_WINDOWS

      // The --user parameter can be in the form username:group or
      // username. The latter will be equivalent to username:username.

    case 11:
      if (geteuid() == 0) {
        usergroup = (char *)malloc(strlen(optarg)+1);
        strcpy(usergroup, optarg);
        user = usergroup;
        group = strstr(user, ":");
        if (group == 0) {
          group = user;
        } else {
          *group = '\0';
          group += 1;

          // This is checking for a : at the end of the parameter (e.g.
          // username:) and treats it as username:username

          if (*group == '\0') {
            group = 0;
          }
        }

        // Verify that the user and group are valid and obtain the IDs that
        // will be necessary for switching to them.

        pwd = getpwnam(user);
        if (pwd == 0) {
          fatal_error("Unable to find user %s", user);
        }

        grp = getgrnam(group);
        if (grp == 0) {
          fatal_error("Unable to find group %s", group);
        }
      } else {
        fatal_error("The --user can only be used by the root user");
      }
      break;

    case 12:
      daemon = 1;
      break;

    case 13:
      use_syslog = 1;
      break;

#endif

    case 14:
      fatal_error("keyless: %s", KSSL_VERSION);
      break;

    case 15:
      test_mode = 1;
      break;
    }
  }

  if (help) {
    printf("Usage: %s [OPTIONS]\n", PROGRAM_NAME);
    fatal_error("\n\
REQUIRED\n\
     --server-cert\n\
     --server-key\n\
\n\
              Path to PEM-encoded files containing the certificate and\n\
              private key that are used when a connection is made to the\n\
              server. These must be signed by an authority that the client\n\
              side recognizes (e.g. the same CA as --ca-file).\n\
\n\
     --ca-file\n\
\n\
              Path to a PEM-encoded file containing the CA certificate\n\
              used to sign client certificates presented on connection.\n\
\n\
    --private-key-directory\n\
\n\
              Path to a directory containing private keys which the keyserver\n\
              provides decoding service against. The key files must end with\n\
              \".key\" and be PEM-encoded. There should be no trailing / on \n\
              the path.\n\
\n\
OPTIONAL\n\
     --ip     \n\
              The IP address of the interface to bind to.\n\
              If missing binds to all available interfaces.\n\
\n\
     --port\n\
              The TCP port on which to listen for connections.\n\
              There connections must be TLSv1.2. Defaults to 2407.\n\
\n\
    --cipher-list\n\
\n\
              An OpenSSL list of ciphers that the TLS server will accept\n\
              for connections. e.g. ECDHE-RSA-AES128-SHA256:RC4:HIGH:!MD5\n\
\n\
\n\
    --silent\n\
              Prevents keyserver from producing any log output. Fatal\n\
              start up errors are sent to stderr.\n\
\n\
    --verbose\n\
\n\
              Enables verbose logging. When enabled access log data is\n\
              sent to the logger as well as errors.\n\
\n\
    --num-workers\n\
\n\
              The number of worker threads to start. Each worker thread\n\
              will handle a single connection from a KSSL client. \n\
              Defaults to 1.\n\
\n\
    --pid-file\n\
\n\
              Path to a file into which the PID of the keyserver.\n\
              This file is only written if the keyserver starts successfully.\n\
\n\
    --test\n\
              Run through start up and check all parameters for validity.\n\
              Returns 0 if configuration is good.\n\
\n\
\n\
The following options are not available on Windows systems:\n\
\n\
    --user\n\
\n\
            user:group to switch to. Can be in the form user:group or just\n\
            user (in which case user:user is implied) (root only)\n\
\n\
    --daemon\n\
\n\
            Forks and abandons the parent process.\n\
\n\
    --syslog\n\
\n\
            Log lines are sent to syslog (instead of stdout or stderr).\n");
  }
  if (!server_cert) {
    fatal_error("The --server-cert parameter must be specified with the path to the server's SSL certificate");
  }
  if (!server_key) {
    fatal_error("The --server-key parameter must be specified with the path to the server's SSL private key");
  }
  if (!ca_file) {
    fatal_error("The --ca-file parameter must be specified with the path to the CA certificate used to sign client certificates presented on connection");
  }
  if (!private_key_directory) {
    fatal_error("The --private-key-directory parameter must be specified with the path to directory containing private keys");
  }
  if (num_workers <= 0 || num_workers > MAX_WORKERS) {
    fatal_error("The --num-workers parameter must between 1 and %d", MAX_WORKERS);
  }

#if !PLATFORM_WINDOWS
  if (daemon && !test_mode) {
    int pid = fork();
    if (pid == -1) {
      fatal_error("Failed to fork");
    }
    if (pid != 0) {
      write_pid(pid_file, pid, !test_mode);
      exit(0);
    }
  } else {
    write_pid(pid_file, getpid(), !test_mode);
  }

  if (usergroup != 0) {
    if (setgid(grp->gr_gid) == -1) {
      fatal_error("Failed to set group %d (%s)", grp->gr_gid, group);
    }
    if (initgroups(user, grp->gr_gid) == -1) {
      fatal_error("Failed to initgroups %d (%s)", grp->gr_gid, user);
    }
    if (setuid(pwd->pw_uid) == -1) {
      fatal_error("Failed to set user %d (%s)", pwd->pw_uid, user);
    }
  }
#else
  write_pid(pid_file, getpid(), !test_mode);
#endif

  SSL_library_init();
  SSL_load_error_strings();
  ERR_load_BIO_strings();

  method = TLSv1_2_server_method();
  ctx = SSL_CTX_new(method);

  if (!ctx) {
    ssl_error();
  }

  // Set the context to ask for a peer (i.e. client certificate on connection)
  // and to refuse connections that do not have a client certificate. The client
  // certificate must be signed by the CA in the --ca-file parameter.

  if (SSL_CTX_set_cipher_list(ctx, cipher_list) == 0) {
    SSL_CTX_free(ctx);
    fatal_error("Failed to set cipher list %s", cipher_list);
  }

  int nid = OBJ_sn2nid(ec_curve_name);
  if (NID_undef == nid) {
    SSL_CTX_free(ctx);
    fatal_error("ECDSA curve not present");
  }

  EC_KEY *ecdh = EC_KEY_new_by_curve_name(nid);
  if (NULL == ecdh) {
    SSL_CTX_free(ctx);
    fatal_error("ECDSA new curve error");
  }

  if(SSL_CTX_set_tmp_ecdh(ctx, ecdh) != 1) {
    SSL_CTX_free(ctx);
    fatal_error("Call to SSL_CTX_set_tmp_ecdh failed");
  }

  SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, 0);

  cert_names = SSL_load_client_CA_file(ca_file);
  if (!cert_names) {
    SSL_CTX_free(ctx);
    fatal_error("Failed to load CA file %s", ca_file);
  }

  SSL_CTX_set_client_CA_list(ctx, cert_names);

  if (SSL_CTX_load_verify_locations(ctx, ca_file, 0) != 1) {
    SSL_CTX_free(ctx);
    fatal_error("Failed to load CA file %s", ca_file);
  }

  if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
    SSL_CTX_free(ctx);
    fatal_error("Call to SSL_CTX_set_default_verify_paths failed");
  }

  free(ca_file);

  if (SSL_CTX_use_certificate_file(ctx, server_cert, SSL_FILETYPE_PEM) != 1) {
    SSL_CTX_free(ctx);
    fatal_error("Problem loading certificate from --server-cert=%s", server_cert);
  }
  if (SSL_CTX_use_PrivateKey_file(ctx, server_key, SSL_FILETYPE_PEM) != 1) {
    SSL_CTX_free(ctx);
    fatal_error("Problem loading private key from --server-key=%s", server_key);
  }
  if (SSL_CTX_check_private_key(ctx) != 1) {
    SSL_CTX_free(ctx);
    fatal_error("Private key %s and certificate %s do not match", server_key, server_cert);
  }

  free(server_cert);
  free(server_key);

  // Create lock and load private keys
  pk_lock = (uv_rwlock_t *)malloc(sizeof(uv_rwlock_t));
  if (pk_lock == NULL) {
    SSL_CTX_free(ctx);
    fatal_error("Memory error");
  }
  rc = uv_rwlock_init(pk_lock);
  if (rc != 0) {
    SSL_CTX_free(ctx);
    fatal_error("Can't initialize lock");
  }
  pk_dir = private_key_directory;
  load_private_keys(ctx);

  // Begin application loop
  loop = uv_loop_new();

  rc = uv_tcp_init(loop, &tcp_server);
  if (rc != 0) {
    SSL_CTX_free(ctx);
    fatal_error("Failed to create TCP server: %s",
                error_string(rc));
  }

  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  memset(&(addr.sin_zero), 0, 8);

  rc = uv_tcp_bind(&tcp_server, (const struct sockaddr*)&addr, 0);
  if (rc != 0) {
    SSL_CTX_free(ctx);
    fatal_error("Can't bind to port %d: %s", port, error_string(rc));
  }

  tcp_server.data = (void *)ctx;

  // Make the worker threads
  for (i = 0; i < num_workers; i++) {
    rc = uv_sem_init(&worker[i].semaphore, 0);
    if (rc != 0) {
      SSL_CTX_free(ctx);
      fatal_error("Failed to create semaphore: %s",
                  error_string(rc));
    }

    worker[i].ctx = ctx;

    rc = uv_thread_create(&worker[i].thread, thread_entry,
                          &worker[i]);
    if (rc != 0) {
      SSL_CTX_free(ctx);
      fatal_error("Failed to create worker thread: %s",
                  error_string(rc));
    }
  }

  // Create a pipe server which will hand the tcp_server handle
  // to threads. Note the 1 in the third parameter of uv_pipe_init:
  // that specifies that this pipe will be used to pass handles.

  p = (ipc_server *)malloc(sizeof(ipc_server));
  p->connects = num_workers;
  p->server = &tcp_server;

  rc = uv_pipe_init(loop, &p->pipe, 1);
  if (rc != 0) {
      SSL_CTX_free(ctx);
      fatal_error("Failed to create parent pipe: %s",
                  error_string(rc));
  }
  rc = uv_pipe_bind(&p->pipe, PIPE_NAME);
  if (rc != 0) {
      SSL_CTX_free(ctx);
      fatal_error("Failed to bind pipe to name %s: %s", PIPE_NAME,
                  error_string(rc));
  }
  p->pipe.data = (void *)p;
  rc = uv_listen((uv_stream_t *)&p->pipe, MAX_WORKERS,
                 ipc_connection_cb);
  if (rc != 0) {
    SSL_CTX_free(ctx);
    fatal_error("Failed to listen on pipe: %s",
                error_string(rc));
  }

  // Pass the tcp_server to all the worker threads and close it
  // here as it is not needed in the main thread.

  for (i = 0; i < num_workers; i++) {
    uv_sem_post(&worker[i].semaphore);
  }
  uv_run(loop, UV_RUN_DEFAULT);
  uv_close((uv_handle_t *)&tcp_server, NULL);
  uv_run(loop, UV_RUN_DEFAULT);
  for (i = 0; i < num_workers; i++) {
    uv_sem_wait(&worker[i].semaphore);
  }

  // The main thread will just wait around for SIGTERM
  if (!test_mode) {
    rc = uv_signal_init(loop, &sigterm_watcher);
    if (rc != 0) {
      SSL_CTX_free(ctx);
      fatal_error("Failed to create SIGTERM watcher: %s",
                  error_string(rc));
    }
    rc = uv_signal_start(&sigterm_watcher, sigterm_cb, SIGTERM);
    if (rc != 0) {
      SSL_CTX_free(ctx);
      fatal_error("Failed to start SIGTERM watcher: %s",
                  error_string(rc));
    }
#if !PLATFORM_WINDOWS
    rc = uv_signal_start(&sigterm_watcher, sigpipe_cb, SIGPIPE);
    if (rc != 0) {
      SSL_CTX_free(ctx);
      fatal_error("Failed to start SIGPIPE watcher: %s", 
                  error_string(rc));
    }
#endif
  }

  // The main thread will wait for SIGHUP to reload
  if (!test_mode) {
    rc = uv_signal_init(loop, &sighup_watcher);
    if (rc != 0) {
      SSL_CTX_free(ctx);
      fatal_error("Failed to create SIGHUP watcher: %s",
                  error_string(rc));
    }
    g_ctx = ctx;
    rc = uv_signal_start(&sighup_watcher, sighup_cb, SIGHUP);
    if (rc != 0) {
      SSL_CTX_free(ctx);
      fatal_error("Failed to start SIGHUP watcher: %s",
                  error_string(rc));
    }
  }

  // Since we'll be running multiple threads OpenSSL needs mutexes as its
  // state is shared across them.

  locks = (uv_mutex_t *)malloc(CRYPTO_num_locks() * sizeof(uv_mutex_t));

  for (i = 0; i < CRYPTO_num_locks(); i++) {
    rc = uv_mutex_init(&locks[i]);
    if (rc != 0) {
      SSL_CTX_free(ctx);
      fatal_error("Failed to create mutex: %s",
                  error_string(rc));
    }
  }

  CRYPTO_set_id_callback(thread_id_cb);
  CRYPTO_set_locking_callback(locking_cb);

  // If in test mode never run this loop. This will cause the program to stop
  // immediately.

  if (!test_mode) {
    uv_run(loop, UV_RUN_DEFAULT);
  }

  // Now clean up all the running threads

  for (i = 0; i < num_workers; i++) {
    rc = uv_async_send(&worker[i].stopper);
    if (rc != 0) {
      write_log(1, "Failed to send stop async message: %s",
                error_string(rc));
    }
    rc = uv_thread_join(&worker[i].thread);
    if (rc != 0) {
      write_log(1, "Thread join failed: %s",
                error_string(rc));
    }
    uv_sem_destroy(&worker[i].semaphore);
  }

  cleanup(loop, ctx, privates);

  for (i = 0; i < CRYPTO_num_locks(); i++) {
    uv_mutex_destroy(&locks[i]);
  }
  free(locks);

#if !PLATFORM_WINDOWS
  free(usergroup);
#endif

  free(pid_file);

  exit(0);
}

