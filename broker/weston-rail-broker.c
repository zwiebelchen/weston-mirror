/*
 * weston-rail-broker: RDP session broker for weston-mirror RAIL
 *
 * Copyright © 2026 weston-mirror RAIL contributors
 * MIT licensed like the rest of weston (see COPYING).
 *
 * Runs as root on the RDP port and gives every user an own weston:
 *
 *  1. A new connection is terminated here (TLS). The client sends user
 *     name and password in its Client Info PDU (mstsc without NLA, with
 *     "prompt for credentials:i:1"); they are checked with PAM
 *     (service "weston-rail").
 *  2. The client is redirected (Server Redirection PDU) to this very
 *     broker with a random one-time routing token (valid 60 s).
 *  3. The client reconnects; the token is read from the X.224 connection
 *     request before TLS starts, and the untouched TCP connection is
 *     handed to the user's weston over its control socket (SCM_RIGHTS).
 *     If the user has no weston yet, one is started under the user's
 *     account with an own PAM session.
 *
 * The per-user weston runs with WESTON_RDP_CONTROL_SOCKET (no port of its
 * own) and WESTON_RDP_IDLE_EXIT_SEC, so it ends when no client is
 * connected and no application is left.
 */

#include "config.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <grp.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <security/pam_appl.h>

#include <freerdp/freerdp.h>
#include <freerdp/peer.h>
#include <freerdp/redirection.h>
#include <freerdp/settings.h>
#include <freerdp/crypto/certificate.h>
#include <freerdp/crypto/privatekey.h>
#include <winpr/ssl.h>
#include <winpr/synch.h>

#ifndef WESTON_BINARY
#define WESTON_BINARY "/usr/local/bin/weston"
#endif

#define BROKER_PAM_SERVICE "weston-rail"
#define TOKEN_LIFETIME_SEC 60
#define AUTH_TIMEOUT_SEC 60
#define HANDOFF_WAIT_SEC 20
#define TOKEN_HEX_LEN 32

#ifndef WINDOW_LEVEL_SUPPORTED_EX
#define WINDOW_LEVEL_SUPPORTED_EX 0x00000002
#endif

static struct {
	int port;
	char *cert;
	char *key;
	char *weston;
	int idle_exit_sec;
	bool allow_root;
	bool verbose;
	char *names[16];	/* --name: extra DNS names for the certificate */
	int n_names;
} cfg = {
	.port = 3389,
	.cert = "/etc/weston-rail/tls.crt",
	.key = "/etc/weston-rail/tls.key",
	.weston = WESTON_BINARY,
	.idle_exit_sec = 60,
};

struct pending {
	struct pending *next;
	char token[TOKEN_HEX_LEN + 1];
	char user[64];
	time_t expires;
};

struct session {
	struct session *next;
	char user[64];
	uid_t uid;
	pid_t pid;		/* session keeper process */
	char ctl[108];		/* weston control socket */
	time_t started;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct pending *pendings;
static struct session *sessions;

/* ------------------------------------------------------------------ */

static void
logmsg(const char *fmt, ...)
{
	va_list ap;
	char ts[32];
	time_t now = time(NULL);

	strftime(ts, sizeof ts, "%H:%M:%S", localtime(&now));
	fprintf(stderr, "[%s] ", ts);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static void
debugmsg(const char *fmt, ...)
{
	va_list ap;

	if (!cfg.verbose)
		return;
	va_start(ap, fmt);
	fprintf(stderr, "    ");
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static bool
valid_username(const char *u)
{
	size_t i;

	if (!u || !*u || strlen(u) >= 64 || u[0] == '-')
		return false;
	for (i = 0; u[i]; i++) {
		unsigned char c = (unsigned char)u[i];

		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
			return false;
	}
	return true;
}

/* ------------------------------------------------------------------ */
/* PAM                                                                 */
/* ------------------------------------------------------------------ */

struct pam_creds {
	const char *user;
	const char *password;
};

static int
pam_conv_cb(int num, const struct pam_message **msg, struct pam_response **resp,
	    void *data)
{
	struct pam_creds *c = data;
	struct pam_response *r;
	int i;

	if (num <= 0 || num > PAM_MAX_NUM_MSG)
		return PAM_CONV_ERR;
	r = calloc(num, sizeof *r);
	if (!r)
		return PAM_BUF_ERR;
	for (i = 0; i < num; i++) {
		switch (msg[i]->msg_style) {
		case PAM_PROMPT_ECHO_OFF:
			r[i].resp = strdup(c->password ? c->password : "");
			break;
		case PAM_PROMPT_ECHO_ON:
			r[i].resp = strdup(c->user ? c->user : "");
			break;
		case PAM_ERROR_MSG:
		case PAM_TEXT_INFO:
			debugmsg("PAM: %s", msg[i]->msg);
			break;
		default:
			free(r);
			return PAM_CONV_ERR;
		}
	}
	*resp = r;
	return PAM_SUCCESS;
}

static bool
pam_check(const char *user, const char *password, const char *rhost)
{
	struct pam_creds creds = { user, password };
	struct pam_conv conv = { pam_conv_cb, &creds };
	pam_handle_t *pamh = NULL;
	int rc;

	rc = pam_start(BROKER_PAM_SERVICE, user, &conv, &pamh);
	if (rc != PAM_SUCCESS) {
		logmsg("PAM: pam_start failed: %s", pam_strerror(pamh, rc));
		return false;
	}
	if (rhost)
		pam_set_item(pamh, PAM_RHOST, rhost);
	rc = pam_authenticate(pamh, PAM_DISALLOW_NULL_AUTHTOK);
	if (rc == PAM_SUCCESS)
		rc = pam_acct_mgmt(pamh, PAM_DISALLOW_NULL_AUTHTOK);
	if (rc != PAM_SUCCESS)
		logmsg("login of '%s' from %s refused: %s", user,
		       rhost ? rhost : "?", pam_strerror(pamh, rc));
	pam_end(pamh, rc);
	return rc == PAM_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* tokens                                                              */
/* ------------------------------------------------------------------ */

static bool
token_create(const char *user, char out[TOKEN_HEX_LEN + 1])
{
	unsigned char raw[TOKEN_HEX_LEN / 2];
	struct pending *p;
	int i;

	if (getrandom(raw, sizeof raw, 0) != (ssize_t)sizeof raw)
		return false;
	for (i = 0; i < (int)sizeof raw; i++)
		sprintf(out + 2 * i, "%02x", raw[i]);
	out[TOKEN_HEX_LEN] = '\0';

	p = calloc(1, sizeof *p);
	if (!p)
		return false;
	memcpy(p->token, out, sizeof p->token);
	snprintf(p->user, sizeof p->user, "%s", user);
	p->expires = time(NULL) + TOKEN_LIFETIME_SEC;
	pthread_mutex_lock(&lock);
	p->next = pendings;
	pendings = p;
	pthread_mutex_unlock(&lock);
	return true;
}

/* one-time: a matching token is removed */
static bool
token_take(const char *token, char user[64])
{
	struct pending **pp, *p;
	time_t now = time(NULL);
	bool found = false;

	pthread_mutex_lock(&lock);
	for (pp = &pendings; (p = *pp);) {
		if (p->expires < now) {
			*pp = p->next;
			free(p);
			continue;
		}
		if (!found && strcmp(p->token, token) == 0) {
			snprintf(user, 64, "%s", p->user);
			*pp = p->next;
			free(p);
			found = true;
			continue;
		}
		pp = &p->next;
	}
	pthread_mutex_unlock(&lock);
	return found;
}

/* ------------------------------------------------------------------ */
/* per-user weston                                                     */
/* ------------------------------------------------------------------ */

static void
read_default_lang(char *out, size_t size)
{
	FILE *f = fopen("/etc/default/locale", "re");
	char line[256];

	snprintf(out, size, "C.UTF-8");
	if (!f)
		return;
	while (fgets(line, sizeof line, f)) {
		char *v;

		if (strncmp(line, "LANG=", 5) != 0)
			continue;
		v = line + 5;
		v[strcspn(v, "\r\n")] = '\0';
		if (*v == '"' || *v == '\'') {
			v++;
			v[strcspn(v, "\"'")] = '\0';
		}
		if (*v)
			snprintf(out, size, "%s", v);
	}
	fclose(f);
}

static bool
copy_file_for_user(const char *src, const char *dst, uid_t uid, gid_t gid)
{
	char buf[8192];
	int in, out;
	ssize_t n;
	bool ok = true;

	in = open(src, O_RDONLY | O_CLOEXEC);
	if (in < 0)
		return false;
	unlink(dst);
	out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (out < 0) {
		close(in);
		return false;
	}
	while ((n = read(in, buf, sizeof buf)) > 0)
		if (write(out, buf, n) != n)
			ok = false;
	if (fchown(out, uid, gid) < 0)
		ok = false;
	close(in);
	close(out);
	return ok;
}

/* runs in the session keeper process (root), never returns */
static void
session_keeper(const struct passwd *pw, const char *ctl)
{
	struct pam_creds creds = { pw->pw_name, NULL };
	struct pam_conv conv = { pam_conv_cb, &creds };
	pam_handle_t *pamh = NULL;
	char rundir[64], logpath[128], certpath[128], keypath[128], lang[64];
	char **penv = NULL;
	bool session_open = false;
	struct stat st;
	pid_t pid;
	int rc, status = 0;

	setsid();

	rc = pam_start(BROKER_PAM_SERVICE, pw->pw_name, &conv, &pamh);
	if (rc == PAM_SUCCESS) {
		pam_set_item(pamh, PAM_TTY, "weston-rail");
		pam_setcred(pamh, PAM_ESTABLISH_CRED);
		if (pam_open_session(pamh, PAM_SILENT) == PAM_SUCCESS)
			session_open = true;
		else
			logmsg("session for %s: no PAM session (continuing)", pw->pw_name);
		penv = pam_getenvlist(pamh);
	}

	/* pam_systemd creates the runtime directory; without logind do it */
	snprintf(rundir, sizeof rundir, "/run/user/%u", (unsigned)pw->pw_uid);
	if (stat(rundir, &st) < 0) {
		mkdir("/run/user", 0755);
		if (mkdir(rundir, 0700) == 0 && chown(rundir, pw->pw_uid, pw->pw_gid) < 0)
			logmsg("session for %s: cannot chown %s", pw->pw_name, rundir);
	}

	/* same TLS identity as the broker, so the client sees one certificate */
	snprintf(certpath, sizeof certpath, "%s/weston-rail-tls.crt", rundir);
	snprintf(keypath, sizeof keypath, "%s/weston-rail-tls.key", rundir);
	if (!copy_file_for_user(cfg.cert, certpath, pw->pw_uid, pw->pw_gid) ||
	    !copy_file_for_user(cfg.key, keypath, pw->pw_uid, pw->pw_gid))
		logmsg("session for %s: cannot provide the TLS certificate", pw->pw_name);
	snprintf(logpath, sizeof logpath, "%s/weston-rail.log", rundir);
	read_default_lang(lang, sizeof lang);

	pid = fork();
	if (pid == 0) {
		char *argv[16];
		char cert_opt[160], key_opt[160], log_opt[160], idle[16];
		int argc = 0, fd, i;

		if (initgroups(pw->pw_name, pw->pw_gid) < 0 ||
		    setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0 ||
		    setuid(0) == 0) {
			logmsg("session for %s: cannot drop privileges", pw->pw_name);
			_exit(1);
		}

		clearenv();
		for (i = 0; penv && penv[i]; i++)
			putenv(penv[i]);
		setenv("HOME", pw->pw_dir, 1);
		setenv("USER", pw->pw_name, 1);
		setenv("LOGNAME", pw->pw_name, 1);
		setenv("SHELL", pw->pw_shell && *pw->pw_shell ? pw->pw_shell : "/bin/sh", 1);
		setenv("PATH", "/usr/local/bin:/usr/bin:/bin", 1);
		setenv("LANG", lang, 1);
		setenv("XDG_RUNTIME_DIR", rundir, 1);
		setenv("XDG_SESSION_TYPE", "wayland", 1);
		setenv("WESTON_RDP_CONTROL_SOCKET", ctl, 1);
		snprintf(idle, sizeof idle, "%d", cfg.idle_exit_sec);
		setenv("WESTON_RDP_IDLE_EXIT_SEC", idle, 1);

		if (chdir(pw->pw_dir) < 0 && chdir("/") < 0)
			_exit(1);
		fd = open(logpath, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
		if (fd >= 0) {
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
		}
		fd = open("/dev/null", O_RDONLY);
		if (fd >= 0)
			dup2(fd, STDIN_FILENO);

		snprintf(cert_opt, sizeof cert_opt, "--rdp-tls-cert=%s", certpath);
		snprintf(key_opt, sizeof key_opt, "--rdp-tls-key=%s", keypath);
		snprintf(log_opt, sizeof log_opt, "--log=%s", logpath);
		if (access("/usr/bin/dbus-run-session", X_OK) == 0) {
			argv[argc++] = "/usr/bin/dbus-run-session";
			argv[argc++] = "--";
		}
		argv[argc++] = cfg.weston;
		argv[argc++] = "--backend=rdp-backend.so";
		argv[argc++] = "--shell=rdprail-shell.so";
		argv[argc++] = "--logger-scopes=log,rdp-backend,rdprail-shell";
		argv[argc++] = log_opt;
		if (access(certpath, R_OK) == 0 && access(keypath, R_OK) == 0) {
			argv[argc++] = cert_opt;
			argv[argc++] = key_opt;
		}
		argv[argc] = NULL;
		execv(argv[0], argv);
		logmsg("session for %s: cannot start %s: %s", pw->pw_name, argv[0],
		       strerror(errno));
		_exit(127);
	}

	if (pid > 0)
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
			;
	unlink(ctl);
	unlink(certpath);
	unlink(keypath);
	if (pamh) {
		if (session_open)
			pam_close_session(pamh, PAM_SILENT);
		pam_setcred(pamh, PAM_DELETE_CRED);
		pam_end(pamh, PAM_SUCCESS);
	}
	_exit(WIFEXITED(status) ? WEXITSTATUS(status) : 1);
}

static struct session *
session_find_locked(const char *user)
{
	struct session *s;

	for (s = sessions; s; s = s->next)
		if (!strcmp(s->user, user))
			return s;
	return NULL;
}

/* returns the control socket path of the user's (possibly new) weston */
static bool
session_get(const char *user, char ctl[108])
{
	struct passwd pwbuf, *pw = NULL;
	char buf[4096];
	struct session *s;
	pid_t pid;

	pthread_mutex_lock(&lock);
	s = session_find_locked(user);
	if (s) {
		memcpy(ctl, s->ctl, 108);
		pthread_mutex_unlock(&lock);
		return true;
	}

	if (getpwnam_r(user, &pwbuf, buf, sizeof buf, &pw) != 0 || !pw) {
		pthread_mutex_unlock(&lock);
		logmsg("no such user '%s'", user);
		return false;
	}
	if (pw->pw_uid == 0 && !cfg.allow_root) {
		pthread_mutex_unlock(&lock);
		logmsg("sessions for root are not allowed (--allow-root)");
		return false;
	}

	s = calloc(1, sizeof *s);
	if (!s) {
		pthread_mutex_unlock(&lock);
		return false;
	}
	snprintf(s->user, sizeof s->user, "%s", user);
	s->uid = pw->pw_uid;
	s->started = time(NULL);
	snprintf(s->ctl, sizeof s->ctl, "/run/user/%u/weston-rail.sock",
		 (unsigned)pw->pw_uid);

	pid = fork();
	if (pid == 0)
		session_keeper(pw, s->ctl);	/* no return */
	if (pid < 0) {
		pthread_mutex_unlock(&lock);
		free(s);
		return false;
	}
	s->pid = pid;
	s->next = sessions;
	sessions = s;
	memcpy(ctl, s->ctl, 108);
	pthread_mutex_unlock(&lock);
	logmsg("session for %s started (uid %u)", user, (unsigned)pw->pw_uid);
	return true;
}

static void *
reaper_thread(void *data)
{
	(void)data;
	for (;;) {
		int status;
		pid_t pid = waitpid(-1, &status, 0);

		if (pid < 0) {
			sleep(1);
			continue;
		}
		pthread_mutex_lock(&lock);
		for (struct session **pp = &sessions, *s; (s = *pp); pp = &s->next) {
			if (s->pid == pid) {
				logmsg("session for %s ended", s->user);
				*pp = s->next;
				free(s);
				break;
			}
		}
		pthread_mutex_unlock(&lock);
	}
	return NULL;
}

/* hand the TCP connection to the user's weston */
static bool
send_fd(const char *ctl, int fd)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	union {
		struct cmsghdr hdr;
		char buf[CMSG_SPACE(sizeof(int))];
	} c;
	char byte = 'F', reply[8];
	struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
	struct msghdr msg = {
		.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = c.buf, .msg_controllen = sizeof c.buf,
	};
	struct cmsghdr *cmsg;
	time_t deadline = time(NULL) + HANDOFF_WAIT_SEC;
	int s = -1;

	snprintf(addr.sun_path, sizeof addr.sun_path, "%s", ctl);
	/* a freshly started weston needs a moment to create the socket */
	while (time(NULL) < deadline) {
		s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (s < 0)
			return false;
		if (connect(s, (struct sockaddr *)&addr, sizeof addr) == 0)
			break;
		close(s);
		s = -1;
		usleep(200 * 1000);
	}
	if (s < 0)
		return false;

	memset(c.buf, 0, sizeof c.buf);
	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
	if (sendmsg(s, &msg, 0) != 1) {
		close(s);
		return false;
	}
	if (read(s, reply, sizeof reply) < 2 || strncmp(reply, "OK", 2) != 0) {
		close(s);
		return false;
	}
	close(s);
	return true;
}

static void
handoff(int fd, const char *user)
{
	char ctl[108];

	if (!session_get(user, ctl) || !send_fd(ctl, fd))
		logmsg("could not hand the connection of %s to the session", user);
	else
		logmsg("connection of %s handed to the session", user);
	close(fd);
}

/* ------------------------------------------------------------------ */
/* authentication phase (FreeRDP server)                               */
/* ------------------------------------------------------------------ */

struct auth_ctx {
	rdpContext _p;
	char rhost[64];
	char local_addr[64];
	bool redirected;
	bool failed;
};

static void
split_user(const char *in, char *out, size_t size)
{
	const char *p = strrchr(in, '\\');	/* DOMAIN\user */

	if (p)
		in = p + 1;
	snprintf(out, size, "%s", in);
	out[strcspn(out, "@")] = '\0';		/* user@domain */
}

static BOOL
auth_post_connect(freerdp_peer *peer)
{
	struct auth_ctx *ctx = (struct auth_ctx *)peer->context;
	rdpSettings *settings = peer->context->settings;
	const char *u = freerdp_settings_get_string(settings, FreeRDP_Username);
	const char *pw = freerdp_settings_get_string(settings, FreeRDP_Password);
	char user[64], token[TOKEN_HEX_LEN + 1], lb[64];
	rdpRedirection *r;
	UINT32 missing = 0;
	BOOL ok;

	if (!u || !*u || !pw || !*pw) {
		logmsg("client %s sent no credentials (.rdp: \"prompt for "
		       "credentials:i:1\", \"enablecredsspsupport:i:0\")", ctx->rhost);
		ctx->failed = true;
		return FALSE;
	}
	split_user(u, user, sizeof user);
	if (!valid_username(user)) {
		logmsg("client %s: invalid user name", ctx->rhost);
		ctx->failed = true;
		return FALSE;
	}
	if (!pam_check(user, pw, ctx->rhost)) {
		ctx->failed = true;
		return FALSE;
	}
	logmsg("user %s authenticated from %s", user, ctx->rhost);

	if (!token_create(user, token)) {
		ctx->failed = true;
		return FALSE;
	}
	/* FreeRDP writes the load balance info as "Cookie: msts=<value>\r\n",
	 * which the client sends back verbatim as X.224 routing token */
	snprintf(lb, sizeof lb, "%s", token);

	r = redirection_new();
	if (!r) {
		ctx->failed = true;
		return FALSE;
	}
	/* no target address: the client reconnects to the address it used,
	 * i.e. this broker (MS-RDPBCGR passes the routing token only then) */
	redirection_set_flags(r, LB_LOAD_BALANCE_INFO | LB_USERNAME);
	redirection_set_session_id(r, 0);
	redirection_set_byte_option(r, LB_LOAD_BALANCE_INFO, (const BYTE *)lb, strlen(lb));
	redirection_set_string_option(r, LB_USERNAME, u);
	if (!redirection_settings_are_valid(r, &missing))
		debugmsg("redirection settings incomplete (0x%x)", missing);
	ok = peer->SendServerRedirection(peer, r);
	redirection_free(r);
	if (!ok) {
		logmsg("sending the redirection to %s failed", ctx->rhost);
		ctx->failed = true;
		return FALSE;
	}
	debugmsg("redirected %s with a routing token", user);
	ctx->redirected = true;
	return TRUE;
}

static BOOL
auth_activate(freerdp_peer *peer)
{
	(void)peer;
	return TRUE;
}

static bool
load_tls(rdpSettings *settings)
{
	rdpCertificate *cert = freerdp_certificate_new_from_file(cfg.cert);
	rdpPrivateKey *key = freerdp_key_new_from_file(cfg.key);

	if (!cert || !key) {
		if (cert)
			freerdp_certificate_free(cert);
		if (key)
			freerdp_key_free(key);
		return false;
	}
	if (!freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerCertificate, cert, 1)) {
		freerdp_certificate_free(cert);
		freerdp_key_free(key);
		return false;
	}
	if (!freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerRsaKey, key, 1)) {
		freerdp_key_free(key);
		return false;
	}
	return true;
}

static void
addr_to_string(const struct sockaddr_storage *ss, char *out, size_t size)
{
	char buf[INET6_ADDRSTRLEN] = "?";

	if (ss->ss_family == AF_INET)
		inet_ntop(AF_INET, &((const struct sockaddr_in *)ss)->sin_addr, buf, sizeof buf);
	else if (ss->ss_family == AF_INET6)
		inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)ss)->sin6_addr, buf, sizeof buf);
	/* IPv4 clients on the dual-stack socket */
	if (!strncmp(buf, "::ffff:", 7) && strchr(buf + 7, '.'))
		memmove(buf, buf + 7, strlen(buf + 7) + 1);
	snprintf(out, size, "%s", buf);
}

static void
authenticate(int fd)
{
	struct sockaddr_storage ss;
	socklen_t sl = sizeof ss;
	freerdp_peer *peer;
	struct auth_ctx *ctx;
	rdpSettings *settings;
	time_t deadline = time(NULL) + AUTH_TIMEOUT_SEC, redirected_at = 0;

	peer = freerdp_peer_new(fd);
	if (!peer) {
		close(fd);
		return;
	}
	peer->ContextSize = sizeof(struct auth_ctx);
	if (!freerdp_peer_context_new(peer)) {
		freerdp_peer_free(peer);
		return;
	}
	ctx = (struct auth_ctx *)peer->context;
	settings = peer->context->settings;

	if (getpeername(fd, (struct sockaddr *)&ss, &sl) == 0)
		addr_to_string(&ss, ctx->rhost, sizeof ctx->rhost);
	sl = sizeof ss;
	if (getsockname(fd, (struct sockaddr *)&ss, &sl) == 0)
		addr_to_string(&ss, ctx->local_addr, sizeof ctx->local_addr);

	if (!load_tls(settings)) {
		logmsg("cannot load %s / %s", cfg.cert, cfg.key);
		goto out;
	}
	freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, FALSE);
	freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, TRUE);
	freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, FALSE);
	freerdp_settings_set_uint32(settings, FreeRDP_OsMajorType, OSMAJORTYPE_UNIX);
	freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32);
	/* a RemoteApp client must see a RemoteApp capable server */
	freerdp_settings_set_bool(settings, FreeRDP_RemoteApplicationMode, TRUE);
	freerdp_settings_set_uint32(settings, FreeRDP_RemoteApplicationSupportLevel,
				    RAIL_LEVEL_SUPPORTED | RAIL_LEVEL_HANDSHAKE_EX_SUPPORTED);
	freerdp_settings_set_uint32(settings, FreeRDP_RemoteWndSupportLevel,
				    WINDOW_LEVEL_SUPPORTED_EX);
	freerdp_settings_set_uint32(settings, FreeRDP_RemoteAppNumIconCaches, 3);
	freerdp_settings_set_uint32(settings, FreeRDP_RemoteAppNumIconCacheEntries, 12);
	freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, TRUE);

	peer->PostConnect = auth_post_connect;
	peer->Activate = auth_activate;

	if (!peer->Initialize(peer)) {
		logmsg("client %s: initialization failed", ctx->rhost);
		goto out;
	}

	while (time(NULL) < deadline) {
		HANDLE handles[32];
		DWORD n = peer->GetEventHandles(peer, handles, 32);

		if (n == 0)
			break;
		WaitForMultipleObjects(n, handles, FALSE, 1000);
		if (!peer->CheckFileDescriptor(peer))
			break;
		if (ctx->failed)
			break;
		if (ctx->redirected) {
			/* the client disconnects itself after the redirection */
			if (!redirected_at)
				redirected_at = time(NULL);
			else if (time(NULL) - redirected_at > 10)
				break;
		}
	}
out:
	peer->Disconnect(peer);
	freerdp_peer_context_free(peer);
	freerdp_peer_free(peer);
}

/* ------------------------------------------------------------------ */
/* connection dispatch                                                 */
/* ------------------------------------------------------------------ */

/*
 * Peek at the X.224 connection request without consuming it. A routing
 * token "Cookie: msts=<token>" marks the second connection after the
 * redirection.
 */
static bool
peek_token(int fd, char token[TOKEN_HEX_LEN + 1])
{
	unsigned char buf[1024];
	size_t need = 4, have = 0, i;
	time_t deadline = time(NULL) + 10;
	const char *p;

	while (have < need && time(NULL) < deadline) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		ssize_t n;

		if (poll(&pfd, 1, 1000) <= 0)
			continue;
		n = recv(fd, buf, sizeof buf, MSG_PEEK);
		if (n <= 0)
			return false;
		have = (size_t)n;
		if (have >= 4 && buf[0] == 3)	/* TPKT */
			need = ((size_t)buf[2] << 8 | buf[3]);
		if (need > sizeof buf)
			need = sizeof buf;
		if (have < need)
			usleep(20 * 1000);
	}
	if (cfg.verbose) {
		char dump[200];
		size_t k, m = 0;

		for (k = 0; k < have && m + 5 < sizeof dump; k++)
			m += (size_t)snprintf(dump + m, sizeof dump - m,
					      (buf[k] >= 32 && buf[k] < 127) ? "%c" : "\\x%02x", buf[k]);
		debugmsg("connection request (%zu bytes): %s", have, dump);
	}
	if (have < 11)
		return false;

	/* search "msts=" in the variable part of the X.224 CR */
	for (i = 0; i + 5 < have; i++) {
		if (memcmp(buf + i, "msts=", 5) != 0)
			continue;
		p = (const char *)buf + i + 5;
		if (i + 5 + TOKEN_HEX_LEN > have)
			return false;
		memcpy(token, p, TOKEN_HEX_LEN);
		token[TOKEN_HEX_LEN] = '\0';
		for (int k = 0; k < TOKEN_HEX_LEN; k++)
			if (!strchr("0123456789abcdef", token[k]))
				return false;
		return true;
	}
	return false;
}

static void *
connection_thread(void *data)
{
	int fd = (int)(intptr_t)data;
	char token[TOKEN_HEX_LEN + 1], user[64];

	if (peek_token(fd, token)) {
		if (token_take(token, user)) {
			handoff(fd, user);
			return NULL;
		}
		logmsg("unknown or expired routing token, authenticating again");
	}
	authenticate(fd);
	return NULL;
}

/* ------------------------------------------------------------------ */

static void
san_add(char *san, size_t size, const char *type, const char *value)
{
	char entry[300];

	if (!value || !*value)
		return;
	snprintf(entry, sizeof entry, "%s:%s", type, value);
	/* no duplicates */
	if (strstr(san, entry))
		return;
	if (*san)
		strncat(san, ",", size - strlen(san) - 1);
	strncat(san, entry, size - strlen(san) - 1);
}

/*
 * Self-signed certificate valid for every name the server is reached by:
 * host name, fully qualified name, all local IP addresses and the names
 * given with --name (e.g. an external DNS name). mstsc checks the name it
 * connected to against the subjectAltName entries.
 */
static bool
ensure_certificate(void)
{
	char hostname[256] = "weston-rail", subj[300], san[4096] = "", ext[4200];
	struct addrinfo hints = { .ai_flags = AI_CANONNAME }, *ai = NULL;
	struct ifaddrs *ifa = NULL, *i;
	pid_t pid;
	int status, k;

	if (access(cfg.cert, R_OK) == 0 && access(cfg.key, R_OK) == 0)
		return true;

	gethostname(hostname, sizeof hostname - 1);
	san_add(san, sizeof san, "DNS", hostname);
	if (getaddrinfo(hostname, NULL, &hints, &ai) == 0 && ai && ai->ai_canonname)
		san_add(san, sizeof san, "DNS", ai->ai_canonname);
	if (ai)
		freeaddrinfo(ai);
	for (k = 0; k < cfg.n_names; k++)
		san_add(san, sizeof san, "DNS", cfg.names[k]);
	if (getifaddrs(&ifa) == 0) {
		for (i = ifa; i; i = i->ifa_next) {
			char buf[INET6_ADDRSTRLEN];

			if (!i->ifa_addr)
				continue;
			if (i->ifa_addr->sa_family == AF_INET)
				inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr,
					  buf, sizeof buf);
			else if (i->ifa_addr->sa_family == AF_INET6 &&
				 !IN6_IS_ADDR_LINKLOCAL(&((struct sockaddr_in6 *)i->ifa_addr)->sin6_addr))
				inet_ntop(AF_INET6, &((struct sockaddr_in6 *)i->ifa_addr)->sin6_addr,
					  buf, sizeof buf);
			else
				continue;
			san_add(san, sizeof san, "IP", buf);
		}
		freeifaddrs(ifa);
	}

	/* the name users type first, so that it shows up as the subject */
	snprintf(subj, sizeof subj, "/CN=%s", cfg.n_names ? cfg.names[0] : hostname);
	snprintf(ext, sizeof ext, "subjectAltName=%s", san);
	logmsg("creating a self-signed certificate %s for %s", cfg.cert, san);
	mkdir("/etc/weston-rail", 0755);
	pid = fork();
	if (pid == 0) {
		int devnull = open("/dev/null", O_WRONLY);

		if (devnull >= 0) {
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
		}
		umask(077);
		execlp("openssl", "openssl", "req", "-x509", "-newkey", "rsa:2048",
		       "-nodes", "-days", "3650", "-subj", subj,
		       "-addext", ext,
		       "-addext", "extendedKeyUsage=serverAuth",
		       "-keyout", cfg.key, "-out", cfg.cert, (char *)NULL);
		_exit(127);
	}
	if (pid < 0 || waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0) {
		logmsg("cannot create the certificate (is openssl installed?)");
		return false;
	}
	chmod(cfg.key, 0600);
	chmod(cfg.cert, 0644);
	return true;
}

static void
usage(void)
{
	fprintf(stderr,
		"usage: weston-rail-broker [options]\n"
		"  -p, --port=PORT        RDP port (3389)\n"
		"  -c, --cert=FILE        TLS certificate (/etc/weston-rail/tls.crt)\n"
		"  -k, --key=FILE         TLS private key (/etc/weston-rail/tls.key)\n"
		"  -w, --weston=PATH      weston binary (%s)\n"
		"  -i, --idle-exit=SEC    end a session this long after the last client\n"
		"                         and the last application are gone (60)\n"
		"  -n, --name=DNSNAME     extra name for the generated certificate, e.g. the\n"
		"                         external DNS name (repeatable; delete\n"
		"                         /etc/weston-rail/tls.* to regenerate)\n"
		"      --allow-root       allow sessions for root\n"
		"  -v, --verbose\n", WESTON_BINARY);
}

int
main(int argc, char *argv[])
{
	static const struct option opts[] = {
		{ "port", required_argument, NULL, 'p' },
		{ "cert", required_argument, NULL, 'c' },
		{ "key", required_argument, NULL, 'k' },
		{ "weston", required_argument, NULL, 'w' },
		{ "idle-exit", required_argument, NULL, 'i' },
		{ "allow-root", no_argument, NULL, 'R' },
		{ "name", required_argument, NULL, 'n' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};
	struct sockaddr_in6 addr = { .sin6_family = AF_INET6 };
	pthread_t tid;
	int lfd, on = 1, off = 0, opt;
	bool bound = false;

	while ((opt = getopt_long(argc, argv, "p:c:k:w:i:n:vh", opts, NULL)) != -1) {
		switch (opt) {
		case 'p': cfg.port = atoi(optarg); break;
		case 'c': cfg.cert = optarg; break;
		case 'k': cfg.key = optarg; break;
		case 'w': cfg.weston = optarg; break;
		case 'i': cfg.idle_exit_sec = atoi(optarg); break;
		case 'R': cfg.allow_root = true; break;
		case 'n':
			if (cfg.n_names < 16)
				cfg.names[cfg.n_names++] = optarg;
			break;
		case 'v': cfg.verbose = true; break;
		default: usage(); return opt == 'h' ? 0 : 2;
		}
	}
	if (geteuid() != 0) {
		fprintf(stderr, "weston-rail-broker must run as root\n");
		return 1;
	}
	signal(SIGPIPE, SIG_IGN);
	setvbuf(stderr, NULL, _IOLBF, 0);

	/* systemd starts services without HOME; FreeRDP derives its
	 * configuration path from it and refuses to create a peer context */
	if (!getenv("HOME") || !*getenv("HOME")) {
		struct passwd *pw = getpwuid(geteuid());

		setenv("HOME", pw && pw->pw_dir ? pw->pw_dir : "/root", 1);
	}
	winpr_InitializeSSL(WINPR_SSL_INIT_DEFAULT);

	if (!ensure_certificate())
		return 1;
	if (access(cfg.weston, X_OK) != 0) {
		logmsg("weston not found at %s (--weston)", cfg.weston);
		return 1;
	}

	/* dual stack if IPv6 is available, IPv4 only otherwise (e.g. an
	 * LXC container without IPv6) */
	lfd = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (lfd >= 0) {
		setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
		setsockopt(lfd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
		addr.sin6_addr = in6addr_any;
		addr.sin6_port = htons(cfg.port);
		bound = bind(lfd, (struct sockaddr *)&addr, sizeof addr) == 0;
		if (!bound) {
			close(lfd);
			lfd = -1;
		}
	}
	if (lfd < 0) {
		struct sockaddr_in addr4 = { .sin_family = AF_INET };

		lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (lfd < 0) {
			perror("socket");
			return 1;
		}
		setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
		addr4.sin_addr.s_addr = htonl(INADDR_ANY);
		addr4.sin_port = htons(cfg.port);
		bound = bind(lfd, (struct sockaddr *)&addr4, sizeof addr4) == 0;
	}
	if (!bound || listen(lfd, 32) < 0) {
		logmsg("cannot listen on port %d: %s", cfg.port, strerror(errno));
		return 1;
	}
	pthread_create(&tid, NULL, reaper_thread, NULL);
	pthread_detach(tid);
	logmsg("weston-rail-broker listening on port %d", cfg.port);

	for (;;) {
		int fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);

		if (fd < 0) {
			if (errno != EINTR)
				usleep(100 * 1000);
			continue;
		}
		/* the connection is passed to weston later: no CLOEXEC there,
		 * weston receives its own duplicate via SCM_RIGHTS anyway */
		if (pthread_create(&tid, NULL, connection_thread, (void *)(intptr_t)fd) != 0) {
			close(fd);
			continue;
		}
		pthread_detach(tid);
	}
}
