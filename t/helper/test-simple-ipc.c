/*
 * test-simple-ipc.c: verify that the Inter-Process Communication works.
 */

#include "test-tool.h"
#include "cache.h"
#include "strbuf.h"
#include "simple-ipc.h"
#include "parse-options.h"
#include "thread-utils.h"
#include "strvec.h"

#ifndef SUPPORTS_SIMPLE_IPC
int cmd__simple_ipc(int argc, const char **argv)
{
	die("simple IPC not available on this platform");
}
#else

/*
 * The test daemon defines an "application callback" that supports a
 * series of commands (see `test_app_cb()`).
 *
 * Unknown commands are caught here and we send an error message back
 * to the client process.
 */
static int app__unhandled_command(const char *command,
				  ipc_server_reply_cb *reply_cb,
				  struct ipc_server_reply_data *reply_data)
{
	struct strbuf buf = STRBUF_INIT;
	int ret;

	strbuf_addf(&buf, "unhandled command: %s", command);
	ret = reply_cb(reply_data, buf.buf, buf.len);
	strbuf_release(&buf);

	return ret;
}

/*
 * Reply with a single very large buffer.  This is to ensure that
 * long response are properly handled -- whether the chunking occurs
 * in the kernel or in the (probably pkt-line) layer.
 */
#define BIG_ROWS (10000)
static int app__big_command(ipc_server_reply_cb *reply_cb,
			    struct ipc_server_reply_data *reply_data)
{
	struct strbuf buf = STRBUF_INIT;
	int row;
	int ret;

	for (row = 0; row < BIG_ROWS; row++)
		strbuf_addf(&buf, "big: %.75d\n", row);

	ret = reply_cb(reply_data, buf.buf, buf.len);
	strbuf_release(&buf);

	return ret;
}

/*
 * Reply with a series of lines.  This is to ensure that we can incrementally
 * compute the response and chunk it to the client.
 */
#define CHUNK_ROWS (10000)
static int app__chunk_command(ipc_server_reply_cb *reply_cb,
			      struct ipc_server_reply_data *reply_data)
{
	struct strbuf buf = STRBUF_INIT;
	int row;
	int ret;

	for (row = 0; row < CHUNK_ROWS; row++) {
		strbuf_setlen(&buf, 0);
		strbuf_addf(&buf, "big: %.75d\n", row);
		ret = reply_cb(reply_data, buf.buf, buf.len);
	}

	strbuf_release(&buf);

	return ret;
}

/*
 * Slowly reply with a series of lines.  This is to model an expensive to
 * compute chunked response (which might happen if this callback is running
 * in a thread and is fighting for a lock with other threads).
 */
#define SLOW_ROWS     (1000)
#define SLOW_DELAY_MS (10)
static int app__slow_command(ipc_server_reply_cb *reply_cb,
			     struct ipc_server_reply_data *reply_data)
{
	struct strbuf buf = STRBUF_INIT;
	int row;
	int ret;

	for (row = 0; row < SLOW_ROWS; row++) {
		strbuf_setlen(&buf, 0);
		strbuf_addf(&buf, "big: %.75d\n", row);
		ret = reply_cb(reply_data, buf.buf, buf.len);
		sleep_millisec(SLOW_DELAY_MS);
	}

	strbuf_release(&buf);

	return ret;
}

/*
 * The client sent a command followed by a (possibly very) large buffer.
 */
static int app__sendbytes_command(const char *received,
				  ipc_server_reply_cb *reply_cb,
				  struct ipc_server_reply_data *reply_data)
{
	struct strbuf buf_resp = STRBUF_INIT;
	const char *p = "?";
	int len_ballast = 0;
	int k;
	int errs = 0;
	int ret;

	if (skip_prefix(received, "sendbytes ", &p))
		len_ballast = strlen(p);

	/*
	 * Verify that the ballast is n copies of a single letter.
	 * And that the multi-threaded IO layer didn't cross the streams.
	 */
	for (k = 1; k < len_ballast; k++)
		if (p[k] != p[0])
			errs++;

	if (errs)
		strbuf_addf(&buf_resp, "errs:%d\n", errs);
	else
		strbuf_addf(&buf_resp, "rcvd:%c%08d\n", p[0], len_ballast);

	ret = reply_cb(reply_data, buf_resp.buf, buf_resp.len);

	strbuf_release(&buf_resp);

	return ret;
}

/*
 * An arbitrary fixed address to verify that the application instance
 * data is handled properly.
 */
static int my_app_data = 42;

static ipc_server_application_cb test_app_cb;

/*
 * This is the "application callback" that sits on top of the
 * "ipc-server".  It completely defines the set of commands supported
 * by this application.
 */
static int test_app_cb(void *application_data,
		       const char *command,
		       ipc_server_reply_cb *reply_cb,
		       struct ipc_server_reply_data *reply_data)
{
	/*
	 * Verify that we received the application-data that we passed
	 * when we started the ipc-server.  (We have several layers of
	 * callbacks calling callbacks and it's easy to get things mixed
	 * up (especially when some are "void*").)
	 */
	if (application_data != (void*)&my_app_data)
		BUG("application_cb: application_data pointer wrong");

	if (!strcmp(command, "quit")) {
		/*
		 * The client sent a "quit" command.  This is an async
		 * request for the server to shutdown.
		 *
		 * We DO NOT send the client a response message
		 * (because we have nothing to say and the other
		 * server threads have not yet stopped).
		 *
		 * Tell the ipc-server layer to start shutting down.
		 * This includes: stop listening for new connections
		 * on the socket/pipe and telling all worker threads
		 * to finish/drain their outgoing responses to other
		 * clients.
		 *
		 * This DOES NOT force an immediate sync shutdown.
		 */
		return SIMPLE_IPC_QUIT;
	}

	if (!strcmp(command, "ping")) {
		const char *answer = "pong";
		return reply_cb(reply_data, answer, strlen(answer));
	}

	if (!strcmp(command, "big"))
		return app__big_command(reply_cb, reply_data);

	if (!strcmp(command, "chunk"))
		return app__chunk_command(reply_cb, reply_data);

	if (!strcmp(command, "slow"))
		return app__slow_command(reply_cb, reply_data);

	if (starts_with(command, "sendbytes "))
		return app__sendbytes_command(command, reply_cb, reply_data);

	return app__unhandled_command(command, reply_cb, reply_data);
}

/*
 * This process will run as a simple-ipc server and listen for IPC commands
 * from client processes.
 */
static int daemon__run_server(const char *path, int argc, const char **argv)
{
	struct ipc_server_opts opts = {
		.nr_threads = 5
	};

	const char * const daemon_usage[] = {
		N_("test-helper simple-ipc run-daemon [<options>"),
		NULL
	};
	struct option daemon_options[] = {
		OPT_INTEGER(0, "threads", &opts.nr_threads,
			    N_("number of threads in server thread pool")),
		OPT_END()
	};

	argc = parse_options(argc, argv, NULL, daemon_options, daemon_usage, 0);

	if (opts.nr_threads < 1)
		opts.nr_threads = 1;

	/*
	 * Synchronously run the ipc-server.  We don't need any application
	 * instance data, so pass an arbitrary pointer (that we'll later
	 * verify made the round trip).
	 */
	return ipc_server_run(path, &opts, test_app_cb, (void*)&my_app_data);
}

#ifndef GIT_WINDOWS_NATIVE
/*
 * This is adapted from `daemonize()`.  Use `fork()` to directly create and
 * run the daemon in a child process.
 */
static int spawn_server(const char *path,
			const struct ipc_server_opts *opts,
			pid_t *pid)
{
	*pid = fork();

	switch (*pid) {
	case 0:
		if (setsid() == -1)
			error_errno(_("setsid failed"));
		close(0);
		close(1);
		close(2);
		sanitize_stdfds();

		return ipc_server_run(path, opts, test_app_cb, (void*)&my_app_data);

	case -1:
		return error_errno(_("could not spawn daemon in the background"));

	default:
		return 0;
	}
}
#else
/*
 * Conceptually like `daemonize()` but different because Windows does not
 * have `fork(2)`.  Spawn a normal Windows child process but without the
 * limitations of `start_command()` and `finish_command()`.
 */
static int spawn_server(const char *path,
			const struct ipc_server_opts *opts,
			pid_t *pid)
{
	char test_tool_exe[MAX_PATH];
	struct strvec args = STRVEC_INIT;
	int in, out;

	GetModuleFileNameA(NULL, test_tool_exe, MAX_PATH);

	in = open("/dev/null", O_RDONLY);
	out = open("/dev/null", O_WRONLY);

	strvec_push(&args, test_tool_exe);
	strvec_push(&args, "simple-ipc");
	strvec_push(&args, "run-daemon");
	strvec_pushf(&args, "--threads=%d", opts->nr_threads);

	*pid = mingw_spawnvpe(args.v[0], args.v, NULL, NULL, in, out, out);
	close(in);
	close(out);

	strvec_clear(&args);

	if (*pid < 0)
		return error(_("could not spawn daemon in the background"));

	return 0;
}
#endif

/*
 * This is adapted from `wait_or_whine()`.  Watch the child process and
 * let it get started and begin listening for requests on the socket
 * before reporting our success.
 */
static int wait_for_server_startup(const char * path, pid_t pid_child,
				   int max_wait_sec)
{
	int status;
	pid_t pid_seen;
	enum ipc_active_state s;
	time_t time_limit, now;

	time(&time_limit);
	time_limit += max_wait_sec;

	for (;;) {
		pid_seen = waitpid(pid_child, &status, WNOHANG);

		if (pid_seen == -1)
			return error_errno(_("waitpid failed"));

		else if (pid_seen == 0) {
			/*
			 * The child is still running (this should be
			 * the normal case).  Try to connect to it on
			 * the socket and see if it is ready for
			 * business.
			 *
			 * If there is another daemon already running,
			 * our child will fail to start (possibly
			 * after a timeout on the lock), but we don't
			 * care (who responds) if the socket is live.
			 */
			s = ipc_get_active_state(path);
			if (s == IPC_STATE__LISTENING)
				return 0;

			time(&now);
			if (now > time_limit)
				return error(_("daemon not online yet"));

			continue;
		}

		else if (pid_seen == pid_child) {
			/*
			 * The new child daemon process shutdown while
			 * it was starting up, so it is not listening
			 * on the socket.
			 *
			 * Try to ping the socket in the odd chance
			 * that another daemon started (or was already
			 * running) while our child was starting.
			 *
			 * Again, we don't care who services the socket.
			 */
			s = ipc_get_active_state(path);
			if (s == IPC_STATE__LISTENING)
				return 0;

			/*
			 * We don't care about the WEXITSTATUS() nor
			 * any of the WIF*(status) values because
			 * `cmd__simple_ipc()` does the `!!result`
			 * trick on all function return values.
			 *
			 * So it is sufficient to just report the
			 * early shutdown as an error.
			 */
			return error(_("daemon failed to start"));
		}

		else
			return error(_("waitpid is confused"));
	}
}

/*
 * This process will start a simple-ipc server in a background process and
 * wait for it to become ready.  This is like `daemonize()` but gives us
 * more control and better error reporting (and makes it easier to write
 * unit tests).
 */
static int daemon__start_server(const char *path, int argc, const char **argv)
{
	pid_t pid_child;
	int ret;
	int max_wait_sec = 60;
	struct ipc_server_opts opts = {
		.nr_threads = 5
	};

	const char * const daemon_usage[] = {
		N_("test-helper simple-ipc start-daemon [<options>"),
		NULL
	};

	struct option daemon_options[] = {
		OPT_INTEGER(0, "max-wait", &max_wait_sec,
			    N_("seconds to wait for daemon to startup")),
		OPT_INTEGER(0, "threads", &opts.nr_threads,
			    N_("number of threads in server thread pool")),
		OPT_END()
	};

	argc = parse_options(argc, argv, NULL, daemon_options, daemon_usage, 0);

	if (max_wait_sec < 0)
		max_wait_sec = 0;
	if (opts.nr_threads < 1)
		opts.nr_threads = 1;

	/*
	 * Run the actual daemon in a background process.
	 */
	ret = spawn_server(path, &opts, &pid_child);
	if (pid_child <= 0)
		return ret;

	/*
	 * Let the parent wait for the child process to get started
	 * and begin listening for requests on the socket.
	 */
	ret = wait_for_server_startup(path, pid_child, max_wait_sec);

	return ret;
}

/*
 * This process will run a quick probe to see if a simple-ipc server
 * is active on this path.
 *
 * Returns 0 if the server is alive.
 */
static int client__probe_server(const char *path)
{
	enum ipc_active_state s;

	s = ipc_get_active_state(path);
	switch (s) {
	case IPC_STATE__LISTENING:
		return 0;

	case IPC_STATE__NOT_LISTENING:
		return error("no server listening at '%s'", path);

	case IPC_STATE__PATH_NOT_FOUND:
		return error("path not found '%s'", path);

	case IPC_STATE__INVALID_PATH:
		return error("invalid pipe/socket name '%s'", path);

	case IPC_STATE__OTHER_ERROR:
	default:
		return error("other error for '%s'", path);
	}
}

/*
 * Send an IPC command to an already-running server daemon and print the
 * response.
 *
 * argv[2] contains a simple (1 word) command that `test_app_cb()` (in
 * the daemon process) will understand.
 */
static int client__send_ipc(int argc, const char **argv, const char *path)
{
	const char *command = argc > 2 ? argv[2] : "(no command)";
	struct strbuf buf = STRBUF_INIT;
	struct ipc_client_connect_options options
		= IPC_CLIENT_CONNECT_OPTIONS_INIT;

	options.wait_if_busy = 1;
	options.wait_if_not_found = 0;

	if (!ipc_client_send_command(path, &options, command, &buf)) {
		if (buf.len) {
			printf("%s\n", buf.buf);
			fflush(stdout);
		}
		strbuf_release(&buf);

		return 0;
	}

	return error("failed to send '%s' to '%s'", command, path);
}

/*
 * Send an IPC command to an already-running server and ask it to
 * shutdown.  "send quit" is an async request and queues a shutdown
 * event in the server, so we spin and wait here for it to actually
 * shutdown to make the unit tests a little easier to write.
 */
static int client__stop_server(int argc, const char **argv, const char *path)
{
	const char *send_quit[] = { argv[0], "send", "quit", NULL };
	int max_wait_sec = 60;
	int ret;
	time_t time_limit, now;
	enum ipc_active_state s;

	const char * const stop_usage[] = {
		N_("test-helper simple-ipc stop-daemon [<options>]"),
		NULL
	};

	struct option stop_options[] = {
		OPT_INTEGER(0, "max-wait", &max_wait_sec,
			    N_("seconds to wait for daemon to stop")),
		OPT_END()
	};

	argc = parse_options(argc, argv, NULL, stop_options, stop_usage, 0);

	if (max_wait_sec < 0)
		max_wait_sec = 0;

	time(&time_limit);
	time_limit += max_wait_sec;

	ret = client__send_ipc(3, send_quit, path);
	if (ret)
		return ret;

	for (;;) {
		sleep_millisec(100);

		s = ipc_get_active_state(path);

		if (s != IPC_STATE__LISTENING) {
			/*
			 * The socket/pipe is gone and/or has stopped
			 * responding.  Lets assume that the daemon
			 * process has exited too.
			 */
			return 0;
		}

		time(&now);
		if (now > time_limit)
			return error(_("daemon has not shutdown yet"));
	}
}

/*
 * Send an IPC command followed by ballast to confirm that a large
 * message can be sent and that the kernel or pkt-line layers will
 * properly chunk it and that the daemon receives the entire message.
 */
static int do_sendbytes(int bytecount, char byte, const char *path,
			const struct ipc_client_connect_options *options)
{
	struct strbuf buf_send = STRBUF_INIT;
	struct strbuf buf_resp = STRBUF_INIT;

	strbuf_addstr(&buf_send, "sendbytes ");
	strbuf_addchars(&buf_send, byte, bytecount);

	if (!ipc_client_send_command(path, options, buf_send.buf, &buf_resp)) {
		strbuf_rtrim(&buf_resp);
		printf("sent:%c%08d %s\n", byte, bytecount, buf_resp.buf);
		fflush(stdout);
		strbuf_release(&buf_send);
		strbuf_release(&buf_resp);

		return 0;
	}

	return error("client failed to sendbytes(%d, '%c') to '%s'",
		     bytecount, byte, path);
}

/*
 * Send an IPC command with ballast to an already-running server daemon.
 */
static int client__sendbytes(int argc, const char **argv, const char *path)
{
	int bytecount = 1024;
	char *string = "x";
	const char * const sendbytes_usage[] = {
		N_("test-helper simple-ipc sendbytes [<options>]"),
		NULL
	};
	struct option sendbytes_options[] = {
		OPT_INTEGER(0, "bytecount", &bytecount, N_("number of bytes")),
		OPT_STRING(0, "byte", &string, N_("byte"), N_("ballast")),
		OPT_END()
	};
	struct ipc_client_connect_options options
		= IPC_CLIENT_CONNECT_OPTIONS_INIT;

	options.wait_if_busy = 1;
	options.wait_if_not_found = 0;
	options.uds_disallow_chdir = 0;

	argc = parse_options(argc, argv, NULL, sendbytes_options, sendbytes_usage, 0);

	return do_sendbytes(bytecount, string[0], path, &options);
}

struct multiple_thread_data {
	pthread_t pthread_id;
	struct multiple_thread_data *next;
	const char *path;
	int bytecount;
	int batchsize;
	int sum_errors;
	int sum_good;
	char letter;
};

static void *multiple_thread_proc(void *_multiple_thread_data)
{
	struct multiple_thread_data *d = _multiple_thread_data;
	int k;
	struct ipc_client_connect_options options
		= IPC_CLIENT_CONNECT_OPTIONS_INIT;

	options.wait_if_busy = 1;
	options.wait_if_not_found = 0;
	/*
	 * A multi-threaded client should not be randomly calling chdir().
	 * The test will pass without this restriction because the test is
	 * not otherwise accessing the filesystem, but it makes us honest.
	 */
	options.uds_disallow_chdir = 1;

	trace2_thread_start("multiple");

	for (k = 0; k < d->batchsize; k++) {
		if (do_sendbytes(d->bytecount + k, d->letter, d->path, &options))
			d->sum_errors++;
		else
			d->sum_good++;
	}

	trace2_thread_exit();
	return NULL;
}

/*
 * Start a client-side thread pool.  Each thread sends a series of
 * IPC requests.  Each request is on a new connection to the server.
 */
static int client__multiple(int argc, const char **argv, const char *path)
{
	struct multiple_thread_data *list = NULL;
	int k;
	int nr_threads = 5;
	int bytecount = 1;
	int batchsize = 10;
	int sum_join_errors = 0;
	int sum_thread_errors = 0;
	int sum_good = 0;

	const char * const multiple_usage[] = {
		N_("test-helper simple-ipc multiple [<options>]"),
		NULL
	};
	struct option multiple_options[] = {
		OPT_INTEGER(0, "bytecount", &bytecount, N_("number of bytes")),
		OPT_INTEGER(0, "threads", &nr_threads, N_("number of threads")),
		OPT_INTEGER(0, "batchsize", &batchsize, N_("number of requests per thread")),
		OPT_END()
	};

	argc = parse_options(argc, argv, NULL, multiple_options, multiple_usage, 0);

	if (bytecount < 1)
		bytecount = 1;
	if (nr_threads < 1)
		nr_threads = 1;
	if (batchsize < 1)
		batchsize = 1;

	for (k = 0; k < nr_threads; k++) {
		struct multiple_thread_data *d = xcalloc(1, sizeof(*d));
		d->next = list;
		d->path = path;
		d->bytecount = bytecount + batchsize*(k/26);
		d->batchsize = batchsize;
		d->sum_errors = 0;
		d->sum_good = 0;
		d->letter = 'A' + (k % 26);

		if (pthread_create(&d->pthread_id, NULL, multiple_thread_proc, d)) {
			warning("failed to create thread[%d] skipping remainder", k);
			free(d);
			break;
		}

		list = d;
	}

	while (list) {
		struct multiple_thread_data *d = list;

		if (pthread_join(d->pthread_id, NULL))
			sum_join_errors++;

		sum_thread_errors += d->sum_errors;
		sum_good += d->sum_good;

		list = d->next;
		free(d);
	}

	printf("client (good %d) (join %d), (errors %d)\n",
	       sum_good, sum_join_errors, sum_thread_errors);

	return (sum_join_errors + sum_thread_errors) ? 1 : 0;
}

int cmd__simple_ipc(int argc, const char **argv)
{
	const char *path = "ipc-test";

	if (argc == 2 && !strcmp(argv[1], "SUPPORTS_SIMPLE_IPC"))
		return 0;

	/*
	 * Use '!!' on all dispatch functions to map from `error()` style
	 * (returns -1) style to `test_must_fail` style (expects 1).  This
	 * makes shell error messages less confusing.
	 */

	if (argc == 2 && !strcmp(argv[1], "is-active"))
		return !!client__probe_server(path);

	if (argc >= 2 && !strcmp(argv[1], "run-daemon"))
		return !!daemon__run_server(path, argc, argv);

	if (argc >= 2 && !strcmp(argv[1], "start-daemon"))
		return !!daemon__start_server(path, argc, argv);

	/*
	 * Client commands follow.  Ensure a server is running before
	 * going any further.
	 */
	if (client__probe_server(path))
		return 1;

	if (argc >= 2 && !strcmp(argv[1], "stop-daemon"))
		return !!client__stop_server(argc, argv, path);

	if ((argc == 2 || argc == 3) && !strcmp(argv[1], "send"))
		return !!client__send_ipc(argc, argv, path);

	if (argc >= 2 && !strcmp(argv[1], "sendbytes"))
		return !!client__sendbytes(argc, argv, path);

	if (argc >= 2 && !strcmp(argv[1], "multiple"))
		return !!client__multiple(argc, argv, path);

	die("Unhandled argv[1]: '%s'", argv[1]);
}
#endif