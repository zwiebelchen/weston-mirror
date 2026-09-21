/*
 * Copyright © 2026 weston-mirror RAIL contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * Device redirection (MS-RDPEFS, "rdpdr" static virtual channel).
 *
 * Drives the client shares are exposed as one FUSE file system, mounted
 * in the session (default $HOME/RDP-Laufwerke, override with
 * WESTON_RDP_DRIVES_DIR). Every drive is a sub directory named after its
 * DOS name ("C", "D", ...). Each file system call is translated into a
 * rdpdr I/O request to the client via FreeRDP's asynchronous drive API;
 * the FUSE worker thread waits for the completion.
 *
 * Printers the client announces are only logged for now (name, driver,
 * flags) to decide on the print data format.
 */

#include "config.h"

#if defined(HAVE_FUSE3)

#define FUSE_USE_VERSION 312

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <fuse.h>

#include "rdp.h"

#include <freerdp/server/rdpdr.h>

/* patched copy of FreeRDP's rdpdr server, see rdpdr/rdpdr_server.c */
RdpdrServerContext *weston_rdpdr_server_context_new(HANDLE vcm);
void weston_rdpdr_server_context_free(RdpdrServerContext *context);
#include <freerdp/utils/rdpdr_utils.h>
#include <winpr/nt.h>

#include "shared/xalloc.h"

/* ---- NTSTATUS values used below (winpr/nt.h has most of them) ---- */
#ifndef STATUS_NO_MORE_FILES
#define STATUS_NO_MORE_FILES ((NTSTATUS)0x80000006)
#endif
#ifndef STATUS_END_OF_FILE
#define STATUS_END_OF_FILE ((NTSTATUS)0xC0000011)
#endif

#define RDPDR_REQUEST_TIMEOUT_SEC 30
#define ATTR_CACHE_TTL_MS 2000
#define ATTR_CACHE_BUCKETS 256
#define MAX_DRIVES 26

#define FILETIME_UNIX_EPOCH_DIFF 11644473600LL

struct rdp_drive {
	bool used;
	UINT32 device_id;
	char name[9];			/* PreferredDosName, NUL terminated */
};

struct attr_entry {
	struct attr_entry *next;
	char *path;			/* FUSE path, e.g. "/C/dir/file" */
	struct stat st;
	struct timespec stamp;
};

struct rdp_drives;

/* one outstanding rdpdr request; the FUSE thread waits on it */
struct drive_wait {
	struct wl_list link;		/* rdp_drives::pending */
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int refs;			/* waiter + outstanding completion */
	bool done;			/* completion arrived */
	bool cancelled;			/* session teardown */
	UINT32 io_status;
	UINT32 file_id;
	UINT32 length;
	char *buffer;			/* read data */

	/* directory listing */
	FILE_DIRECTORY_INFORMATION *entries;
	size_t n_entries;
	size_t cap_entries;
};

struct rdp_drives {
	RdpPeerContext *peer_ctx;
	RdpdrServerContext *rdpdr;
	char client_name[256];

	pthread_mutex_t lock;		/* drives[], pending, cache, shutdown */
	struct rdp_drive drives[MAX_DRIVES];
	struct wl_list pending;
	bool shutdown;

	struct attr_entry *cache[ATTR_CACHE_BUCKETS];

	char *mount_dir;
	struct fuse *fuse;
	pthread_t fuse_thread;
	bool fuse_thread_running;
};

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static int
nt_to_errno(UINT32 status)
{
	switch ((NTSTATUS)status) {  /* NOLINT */
	case STATUS_SUCCESS:
		return 0;
	case STATUS_NO_SUCH_FILE:
	case STATUS_OBJECT_NAME_NOT_FOUND:
	case STATUS_OBJECT_PATH_NOT_FOUND:
	case STATUS_NO_SUCH_DEVICE:
		return ENOENT;
	case STATUS_ACCESS_DENIED:
	case STATUS_CANNOT_DELETE:
		return EACCES;
	case STATUS_OBJECT_NAME_COLLISION:
		return EEXIST;
	case STATUS_DIRECTORY_NOT_EMPTY:
		return ENOTEMPTY;
	case STATUS_NOT_A_DIRECTORY:
		return ENOTDIR;
	case STATUS_FILE_IS_A_DIRECTORY:
		return EISDIR;
	case STATUS_DISK_FULL:
		return ENOSPC;
	case STATUS_SHARING_VIOLATION:
		return EBUSY;
	case STATUS_CANCELLED:
		return EINTR;
	case STATUS_NOT_SUPPORTED:
		return ENOTSUP;
	default:
		return EIO;
	}
}

static time_t
filetime_to_unix(LARGE_INTEGER ft)
{
	if (ft.QuadPart <= 0)
		return 0;
	return (time_t)(ft.QuadPart / 10000000LL - FILETIME_UNIX_EPOCH_DIFF);
}

static void
fdi_to_stat(const FILE_DIRECTORY_INFORMATION *fdi, struct stat *st)
{
	memset(st, 0, sizeof *st);
	if (fdi->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
		st->st_mode = S_IFDIR | 0755;
		st->st_nlink = 2;
	} else {
		st->st_mode = S_IFREG |
			((fdi->FileAttributes & FILE_ATTRIBUTE_READONLY) ? 0444 : 0644);
		st->st_nlink = 1;
		st->st_size = fdi->EndOfFile.QuadPart;
	}
	st->st_uid = getuid();
	st->st_gid = getgid();
	st->st_blksize = 4096;
	st->st_blocks = (st->st_size + 511) / 512;
	st->st_mtime = filetime_to_unix(fdi->LastWriteTime);
	st->st_atime = filetime_to_unix(fdi->LastAccessTime);
	st->st_ctime = filetime_to_unix(fdi->ChangeTime);
	if (!st->st_ctime)
		st->st_ctime = st->st_mtime;
}

static void
dir_stat(struct stat *st)
{
	memset(st, 0, sizeof *st);
	st->st_mode = S_IFDIR | 0755;
	st->st_nlink = 2;
	st->st_uid = getuid();
	st->st_gid = getgid();
	st->st_mtime = st->st_atime = st->st_ctime = time(NULL);
}

static struct rdp_drives *
get_drives(void)
{
	return fuse_get_context()->private_data;
}

/*
 * Split "/C/dir/file" into the drive and the path on that drive
 * ("/dir/file"; "" for the drive root, which is how rdpdr addresses it).
 * Returns false for the mount root or an unknown drive.
 */
static bool
resolve(struct rdp_drives *d, const char *path, UINT32 *device_id,
	char *rel, size_t rel_size, bool *is_root)
{
	const char *p = path;
	const char *slash;
	size_t len;
	int i;
	bool found = false;

	*is_root = false;
	while (*p == '/')
		p++;
	if (*p == '\0') {
		*is_root = true;
		return false;
	}

	slash = strchr(p, '/');
	len = slash ? (size_t)(slash - p) : strlen(p);

	pthread_mutex_lock(&d->lock);
	for (i = 0; i < MAX_DRIVES; i++) {
		if (d->drives[i].used &&
		    strlen(d->drives[i].name) == len &&
		    strncmp(d->drives[i].name, p, len) == 0) {
			*device_id = d->drives[i].device_id;
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&d->lock);

	if (!found)
		return false;

	if (!slash || slash[1] == '\0')
		snprintf(rel, rel_size, "%s", "");
	else
		snprintf(rel, rel_size, "%s", slash);
	return true;
}

/* ---- attribute cache (readdir results, avoids a listing per stat) ---- */

static unsigned
cache_hash(const char *s)
{
	unsigned h = 5381;

	while (*s)
		h = h * 33 + (unsigned char)*s++;
	return h % ATTR_CACHE_BUCKETS;
}

static long
ms_since(const struct timespec *t)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - t->tv_sec) * 1000 + (now.tv_nsec - t->tv_nsec) / 1000000;
}

static void
cache_put(struct rdp_drives *d, const char *path, const struct stat *st)
{
	unsigned h = cache_hash(path);
	struct attr_entry *e;

	pthread_mutex_lock(&d->lock);
	for (e = d->cache[h]; e; e = e->next)
		if (strcmp(e->path, path) == 0)
			break;
	if (!e) {
		e = xzalloc(sizeof *e);
		e->path = xstrdup(path);
		e->next = d->cache[h];
		d->cache[h] = e;
	}
	e->st = *st;
	clock_gettime(CLOCK_MONOTONIC, &e->stamp);
	pthread_mutex_unlock(&d->lock);
}

static bool
cache_get(struct rdp_drives *d, const char *path, struct stat *st)
{
	unsigned h = cache_hash(path);
	struct attr_entry *e;
	bool hit = false;

	pthread_mutex_lock(&d->lock);
	for (e = d->cache[h]; e; e = e->next) {
		if (strcmp(e->path, path) == 0) {
			if (ms_since(&e->stamp) < ATTR_CACHE_TTL_MS) {
				*st = e->st;
				hit = true;
			}
			break;
		}
	}
	pthread_mutex_unlock(&d->lock);
	return hit;
}

static void
cache_clear_locked(struct rdp_drives *d)
{
	int i;

	for (i = 0; i < ATTR_CACHE_BUCKETS; i++) {
		struct attr_entry *e = d->cache[i];

		while (e) {
			struct attr_entry *next = e->next;

			free(e->path);
			free(e);
			e = next;
		}
		d->cache[i] = NULL;
	}
}

static void
cache_clear(struct rdp_drives *d)
{
	pthread_mutex_lock(&d->lock);
	cache_clear_locked(d);
	pthread_mutex_unlock(&d->lock);
}

/* ---- request / completion plumbing ---- */

/*
 * Life cycle: a waiter holds two references, one for the FUSE thread and
 * one for the completion routine; whoever drops the last frees it. The
 * FUSE thread only reads the result once "done" is set; after a timeout
 * or cancellation it must not touch the result fields any more.
 */
static struct drive_wait *
wait_new(struct rdp_drives *d)
{
	struct drive_wait *w = xzalloc(sizeof *w);

	pthread_mutex_init(&w->mutex, NULL);
	pthread_cond_init(&w->cond, NULL);
	w->refs = 2;
	pthread_mutex_lock(&d->lock);
	wl_list_insert(&d->pending, &w->link);
	pthread_mutex_unlock(&d->lock);
	return w;
}

static void
wait_put(struct drive_wait *w)
{
	bool last;

	pthread_mutex_lock(&w->mutex);
	last = --w->refs == 0;
	pthread_mutex_unlock(&w->mutex);
	if (!last)
		return;
	pthread_mutex_destroy(&w->mutex);
	pthread_cond_destroy(&w->cond);
	free(w->buffer);
	free(w->entries);
	free(w);
}

/* completion side: publish the result and drop the completion reference */
static void
wait_complete(struct drive_wait *w, UINT32 io_status)
{
	pthread_mutex_lock(&w->mutex);
	w->io_status = io_status;
	w->done = true;
	pthread_cond_signal(&w->cond);
	pthread_mutex_unlock(&w->mutex);
	wait_put(w);
}

/*
 * FUSE side: wait for the completion. Returns 0 when the result is
 * available, a negative errno otherwise. The caller always ends with
 * wait_put().
 */
static int
wait_for(struct rdp_drives *d, struct drive_wait *w, UINT send_result)
{
	struct timespec deadline;
	int rc = 0;
	bool ok;

	pthread_mutex_lock(&d->lock);
	if (send_result != CHANNEL_RC_OK || d->shutdown) {
		wl_list_remove(&w->link);
		wl_list_init(&w->link);
		pthread_mutex_unlock(&d->lock);
		/* the completion reference is leaked on purpose: a request may
		 * have been queued before sending failed, and a late completion
		 * must not find freed memory */
		return -EIO;
	}
	pthread_mutex_unlock(&d->lock);

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += RDPDR_REQUEST_TIMEOUT_SEC;

	pthread_mutex_lock(&w->mutex);
	while (!w->done && !w->cancelled && rc == 0)
		rc = pthread_cond_timedwait(&w->cond, &w->mutex, &deadline);
	ok = w->done;
	pthread_mutex_unlock(&w->mutex);

	pthread_mutex_lock(&d->lock);
	wl_list_remove(&w->link);
	wl_list_init(&w->link);
	pthread_mutex_unlock(&d->lock);

	return ok ? 0 : -EIO;
}

static struct rdp_drives *
drives_from_context(RdpdrServerContext *context)
{
	return context->data;
}

static void
on_simple_complete(RdpdrServerContext *context, void *cb, UINT32 io_status)
{
	(void)context;
	wait_complete(cb, io_status);
}

static void
on_query_directory_complete(RdpdrServerContext *context, void *cb,
			    UINT32 io_status, FILE_DIRECTORY_INFORMATION *fdi)
{
	struct drive_wait *w = cb;

	(void)context;
	if (io_status == STATUS_SUCCESS) {
		/* one call per entry, the listing continues */
		if (fdi) {
			pthread_mutex_lock(&w->mutex);
			{
				if (w->n_entries == w->cap_entries) {
					w->cap_entries = w->cap_entries ? w->cap_entries * 2 : 64;
					w->entries = xrealloc(w->entries,
							      w->cap_entries * sizeof *w->entries);
				}
				w->entries[w->n_entries++] = *fdi;
			}
			pthread_mutex_unlock(&w->mutex);
		}
		return;
	}
	/* STATUS_NO_MORE_FILES ends a successful listing */
	wait_complete(w, io_status == (UINT32)STATUS_NO_MORE_FILES ? (UINT32)STATUS_SUCCESS : io_status);
}

static void
on_open_complete(RdpdrServerContext *context, void *cb, UINT32 io_status,
		 UINT32 device_id, UINT32 file_id)
{
	struct drive_wait *w = cb;

	(void)context;
	(void)device_id;
	w->file_id = file_id;
	wait_complete(w, io_status);
}

static void
on_read_complete(RdpdrServerContext *context, void *cb, UINT32 io_status,
		 const char *buffer, UINT32 length)
{
	struct drive_wait *w = cb;

	(void)context;
	pthread_mutex_lock(&w->mutex);
	if (io_status == STATUS_SUCCESS && length) {
		w->buffer = xmalloc(length);
		memcpy(w->buffer, buffer, length);
		w->length = length;
	}
	pthread_mutex_unlock(&w->mutex);
	wait_complete(w, io_status);
}

static void
on_write_complete(RdpdrServerContext *context, void *cb, UINT32 io_status,
		  UINT32 bytes_written)
{
	struct drive_wait *w = cb;

	(void)context;
	w->length = bytes_written;
	wait_complete(w, io_status);
}

/* ---- synchronous wrappers for the FUSE threads ---- */

static int
drv_list(struct rdp_drives *d, UINT32 dev, const char *rel,
	 struct drive_wait **out)
{
	struct drive_wait *w = wait_new(d);
	int rc;

	rc = wait_for(d, w, d->rdpdr->DriveQueryDirectory(d->rdpdr, w, dev, rel));
	if (rc == 0 && w->io_status != STATUS_SUCCESS)
		rc = -nt_to_errno(w->io_status);
	if (rc < 0) {
		wait_put(w);
		return rc;
	}
	*out = w;
	return 0;
}

static int
drv_open(struct rdp_drives *d, UINT32 dev, const char *rel, UINT32 access,
	 UINT32 disposition, UINT32 *file_id)
{
	struct drive_wait *w = wait_new(d);
	int rc;

	rc = wait_for(d, w, d->rdpdr->DriveOpenFile(d->rdpdr, w, dev, rel,
						       access, disposition));
	if (rc == 0) {
		rc = -nt_to_errno(w->io_status);
		*file_id = w->file_id;
	}
	wait_put(w);
	return rc;
}

static int
drv_close(struct rdp_drives *d, UINT32 dev, UINT32 file_id)
{
	struct drive_wait *w = wait_new(d);
	int rc;

	rc = wait_for(d, w, d->rdpdr->DriveCloseFile(d->rdpdr, w, dev, file_id));
	if (rc == 0)
		rc = -nt_to_errno(w->io_status);
	wait_put(w);
	return rc;
}

#define DRV_SIMPLE(name, call)						\
static int								\
name(struct rdp_drives *d, UINT32 dev, const char *a, const char *b)	\
{									\
	struct drive_wait *w = wait_new(d);				\
	int rc;								\
	(void)b;							\
	rc = wait_for(d, w, call);					\
	if (rc == 0)							\
		rc = -nt_to_errno(w->io_status);			\
	wait_put(w);							\
	return rc;							\
}

DRV_SIMPLE(drv_mkdir, d->rdpdr->DriveCreateDirectory(d->rdpdr, w, dev, a))
DRV_SIMPLE(drv_rmdir, d->rdpdr->DriveDeleteDirectory(d->rdpdr, w, dev, a))
DRV_SIMPLE(drv_unlink, d->rdpdr->DriveDeleteFile(d->rdpdr, w, dev, a))
DRV_SIMPLE(drv_rename, d->rdpdr->DriveRenameFile(d->rdpdr, w, dev, a, b))

/* ------------------------------------------------------------------ */
/* FUSE operations                                                     */
/* ------------------------------------------------------------------ */

static void
split_parent(const char *rel, char *parent, size_t parent_size,
	     const char **base)
{
	const char *slash = strrchr(rel, '/');

	if (!slash) {
		snprintf(parent, parent_size, "%s", "");
		*base = rel;
		return;
	}
	snprintf(parent, parent_size, "%.*s", (int)(slash - rel), rel);
	*base = slash + 1;
}

static int
op_getattr(const char *path, struct stat *st, struct fuse_file_info *fi)
{
	struct rdp_drives *d = get_drives();
	char rel[PATH_MAX], parent[PATH_MAX];
	const char *base;
	struct drive_wait *w;
	UINT32 dev;
	bool is_root;
	size_t i;
	int rc;

	(void)fi;
	if (!resolve(d, path, &dev, rel, sizeof rel, &is_root)) {
		if (is_root) {
			dir_stat(st);
			return 0;
		}
		return -ENOENT;
	}
	if (rel[0] == '\0') {		/* drive root */
		dir_stat(st);
		return 0;
	}
	if (cache_get(d, path, st))
		return 0;

	/* no "query information" in the API: list the parent and pick it */
	split_parent(rel, parent, sizeof parent, &base);
	rc = drv_list(d, dev, parent, &w);
	if (rc < 0)
		return rc;

	rc = -ENOENT;
	for (i = 0; i < w->n_entries; i++) {
		if (strcasecmp(w->entries[i].FileName, base) == 0) {
			fdi_to_stat(&w->entries[i], st);
			rc = 0;
			break;
		}
	}
	wait_put(w);
	if (rc == 0)
		cache_put(d, path, st);
	return rc;
}

static int
op_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset,
	   struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
	struct rdp_drives *d = get_drives();
	char rel[PATH_MAX], child[PATH_MAX * 2];
	struct drive_wait *w;
	UINT32 dev;
	bool is_root;
	size_t i;
	int rc;

	(void)offset;
	(void)fi;
	(void)flags;

	filler(buf, ".", NULL, 0, 0);
	filler(buf, "..", NULL, 0, 0);

	if (!resolve(d, path, &dev, rel, sizeof rel, &is_root)) {
		if (!is_root)
			return -ENOENT;
		pthread_mutex_lock(&d->lock);
		for (i = 0; i < MAX_DRIVES; i++)
			if (d->drives[i].used)
				filler(buf, d->drives[i].name, NULL, 0, 0);
		pthread_mutex_unlock(&d->lock);
		return 0;
	}

	rc = drv_list(d, dev, rel, &w);
	if (rc < 0)
		return rc;

	for (i = 0; i < w->n_entries; i++) {
		const char *name = w->entries[i].FileName;
		struct stat st;

		if (!strcmp(name, ".") || !strcmp(name, ".."))
			continue;
		fdi_to_stat(&w->entries[i], &st);
		snprintf(child, sizeof child, "%s/%s",
			 strcmp(path, "/") ? path : "", name);
		cache_put(d, child, &st);
		filler(buf, name, &st, 0, 0);
	}
	wait_put(w);
	return 0;
}

static UINT32
access_from_flags(int flags)
{
	switch (flags & O_ACCMODE) {
	case O_WRONLY:
		return GENERIC_WRITE;
	case O_RDWR:
		return GENERIC_READ | GENERIC_WRITE;
	default:
		return GENERIC_READ;
	}
}

static int
op_open(const char *path, struct fuse_file_info *fi)
{
	struct rdp_drives *d = get_drives();
	char rel[PATH_MAX];
	UINT32 dev, file_id = 0;
	bool is_root;
	int rc;

	if (!resolve(d, path, &dev, rel, sizeof rel, &is_root))
		return -ENOENT;

	rc = drv_open(d, dev, rel, access_from_flags(fi->flags),
		      (fi->flags & O_TRUNC) ? FILE_OVERWRITE : FILE_OPEN,
		      &file_id);
	if (rc < 0)
		return rc;
	if (fi->flags & O_TRUNC)
		cache_clear(d);
	fi->fh = ((uint64_t)dev << 32) | file_id;
	return 0;
}

static int
op_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	struct rdp_drives *d = get_drives();
	char rel[PATH_MAX];
	UINT32 dev, file_id = 0, disposition;
	bool is_root;
	int rc;

	(void)mode;
	if (!resolve(d, path, &dev, rel, sizeof rel, &is_root))
		return -EACCES;

	if (fi->flags & O_EXCL)
		disposition = FILE_CREATE;
	else if (fi->flags & O_TRUNC)
		disposition = FILE_OVERWRITE_IF;
	else
		disposition = FILE_OPEN_IF;

	rc = drv_open(d, dev, rel, access_from_flags(fi->flags) | GENERIC_READ,
		      disposition, &file_id);
	if (rc < 0)
		return rc;
	cache_clear(d);
	fi->fh = ((uint64_t)dev << 32) | file_id;
	return 0;
}

static int
op_read(const char *path, char *buf, size_t size, off_t offset,
	struct fuse_file_info *fi)
{
	struct rdp_drives *d = get_drives();
	struct drive_wait *w;
	UINT32 dev = fi->fh >> 32, file_id = fi->fh & 0xffffffff;
	int rc;

	(void)path;
	if (offset < 0 || offset > UINT32_MAX)
		return -EFBIG;	/* rdpdr offsets are 32 bit in FreeRDP's API */

	w = wait_new(d);
	rc = wait_for(d, w, d->rdpdr->DriveReadFile(d->rdpdr, w, dev, file_id,
						       (UINT32)size, (UINT32)offset));
	if (rc < 0) {
		wait_put(w);
		return rc;
	}
	if (w->io_status == (UINT32)STATUS_END_OF_FILE) {
		rc = 0;
	} else if (w->io_status != STATUS_SUCCESS) {
		rc = -nt_to_errno(w->io_status);
	} else {
		rc = (int)(w->length < size ? w->length : size);
		memcpy(buf, w->buffer, rc);
	}
	wait_put(w);
	return rc;
}

static int
op_write(const char *path, const char *buf, size_t size, off_t offset,
	 struct fuse_file_info *fi)
{
	struct rdp_drives *d = get_drives();
	struct drive_wait *w;
	UINT32 dev = fi->fh >> 32, file_id = fi->fh & 0xffffffff;
	int rc;

	(void)path;
	if (offset < 0 || offset + (off_t)size > UINT32_MAX)
		return -EFBIG;

	w = wait_new(d);
	rc = wait_for(d, w, d->rdpdr->DriveWriteFile(d->rdpdr, w, dev, file_id,
							buf, (UINT32)size,
							(UINT32)offset));
	if (rc < 0) {
		wait_put(w);
		return rc;
	}
	rc = w->io_status == STATUS_SUCCESS ? (int)w->length
					    : -nt_to_errno(w->io_status);
	wait_put(w);
	cache_clear(d);
	return rc;
}

static int
op_release(const char *path, struct fuse_file_info *fi)
{
	struct rdp_drives *d = get_drives();

	(void)path;
	drv_close(d, fi->fh >> 32, fi->fh & 0xffffffff);
	return 0;
}

#define RESOLVE_OR(errval)						\
	struct rdp_drives *d = get_drives();				\
	char rel[PATH_MAX];						\
	UINT32 dev;							\
	bool is_root;							\
	if (!resolve(d, path, &dev, rel, sizeof rel, &is_root) ||	\
	    rel[0] == '\0')						\
		return errval

static int
op_mkdir(const char *path, mode_t mode)
{
	RESOLVE_OR(-EACCES);
	int rc;

	(void)mode;
	rc = drv_mkdir(d, dev, rel, NULL);
	cache_clear(d);
	if (rc < 0 && rc != -EEXIST) {
		/* FreeRDP clients create the directory but report a stale
		 * error from their directory handling; trust what is there */
		struct stat st;

		if (op_getattr(path, &st, NULL) == 0 && S_ISDIR(st.st_mode))
			rc = 0;
	}
	return rc;
}

static int
op_rmdir(const char *path)
{
	RESOLVE_OR(-EACCES);
	int rc;

	rc = drv_rmdir(d, dev, rel, NULL);
	cache_clear(d);
	return rc;
}

static int
op_unlink(const char *path)
{
	RESOLVE_OR(-EACCES);
	int rc;

	rc = drv_unlink(d, dev, rel, NULL);
	cache_clear(d);
	return rc;
}

static int
op_rename(const char *path, const char *to, unsigned int flags)
{
	RESOLVE_OR(-EACCES);
	char rel_to[PATH_MAX];
	UINT32 dev_to;
	bool root_to;
	int rc;

	if (flags & RENAME_EXCHANGE)
		return -ENOTSUP;
	if (!resolve(d, to, &dev_to, rel_to, sizeof rel_to, &root_to) ||
	    rel_to[0] == '\0')
		return -EACCES;
	if (dev_to != dev)
		return -EXDEV;

	rc = drv_rename(d, dev, rel, rel_to);
	/* POSIX rename replaces an existing target, Windows does not */
	if (rc == -EEXIST && !(flags & RENAME_NOREPLACE)) {
		if (drv_unlink(d, dev, rel_to, NULL) == 0)
			rc = drv_rename(d, dev, rel, rel_to);
	}
	cache_clear(d);
	return rc;
}

static int
op_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
	RESOLVE_OR(-EACCES);
	struct stat st;
	UINT32 file_id;
	int rc;

	(void)fi;
	if (size == 0) {
		/* the only truncation the API can express: overwrite */
		rc = drv_open(d, dev, rel, GENERIC_WRITE, FILE_OVERWRITE, &file_id);
		if (rc == 0)
			drv_close(d, dev, file_id);
		cache_clear(d);
		return rc;
	}
	rc = op_getattr(path, &st, NULL);
	if (rc == 0 && st.st_size == size)
		return 0;
	return -ENOTSUP;
}

static int
op_utimens(const char *path, const struct timespec tv[2],
	   struct fuse_file_info *fi)
{
	(void)path;
	(void)tv;
	(void)fi;
	return 0;	/* not expressible, accept so cp/tar do not fail */
}

static int
op_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	(void)path;
	(void)mode;
	(void)fi;
	return 0;
}

static int
op_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi)
{
	(void)path;
	(void)uid;
	(void)gid;
	(void)fi;
	return 0;
}

static int
op_statfs(const char *path, struct statvfs *sv)
{
	(void)path;
	memset(sv, 0, sizeof *sv);
	sv->f_bsize = 4096;
	sv->f_frsize = 4096;
	sv->f_blocks = 1ULL << 28;	/* not queried from the client */
	sv->f_bfree = 1ULL << 27;
	sv->f_bavail = 1ULL << 27;
	sv->f_namemax = 255;
	return 0;
}

static void *
op_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
	(void)conn;
	cfg->kernel_cache = 0;
	cfg->attr_timeout = 1.0;
	cfg->entry_timeout = 1.0;
	cfg->negative_timeout = 0.0;
	cfg->use_ino = 0;
	return fuse_get_context()->private_data;
}

static const struct fuse_operations rdp_drive_ops = {
	.init = op_init,
	.getattr = op_getattr,
	.readdir = op_readdir,
	.open = op_open,
	.create = op_create,
	.read = op_read,
	.write = op_write,
	.release = op_release,
	.mkdir = op_mkdir,
	.rmdir = op_rmdir,
	.unlink = op_unlink,
	.rename = op_rename,
	.truncate = op_truncate,
	.utimens = op_utimens,
	.chmod = op_chmod,
	.chown = op_chown,
	.statfs = op_statfs,
};

/* ------------------------------------------------------------------ */
/* mount handling                                                      */
/* ------------------------------------------------------------------ */

static void *
fuse_thread_main(void *data)
{
	struct rdp_drives *d = data;
	struct fuse_loop_config *cfg = fuse_loop_cfg_create();

	fuse_loop_cfg_set_max_threads(cfg, 8);
	fuse_loop_mt(d->fuse, cfg);
	fuse_loop_cfg_destroy(cfg);
	return NULL;
}

static void
force_unmount(const char *dir)
{
	pid_t pid = fork();

	if (pid == 0) {
		execlp("fusermount3", "fusermount3", "-u", "-z", dir, (char *)NULL);
		_exit(127);
	}
	if (pid > 0)
		waitpid(pid, NULL, 0);
}

static char *
default_mount_dir(void)
{
	const char *env = getenv("WESTON_RDP_DRIVES_DIR");
	const char *home = getenv("HOME");
	char *dir;

	if (env && *env)
		return xstrdup(env);
	if (!home || !*home)
		home = "/tmp";
	if (asprintf(&dir, "%s/RDP-Laufwerke", home) < 0)
		return NULL;
	return dir;
}

static bool
drives_mount(struct rdp_drives *d)
{
	char *argv[] = { "weston-rdp-drives", NULL };
	struct fuse_args args = FUSE_ARGS_INIT(1, argv);
	int attempt;

	d->mount_dir = default_mount_dir();
	if (!d->mount_dir)
		return false;
	if (mkdir(d->mount_dir, 0700) < 0 && errno != EEXIST) {
		weston_log("RDP drives: cannot create %s: %s\n",
			   d->mount_dir, strerror(errno));
		return false;
	}

	d->fuse = fuse_new(&args, &rdp_drive_ops, sizeof rdp_drive_ops, d);
	fuse_opt_free_args(&args);
	if (!d->fuse) {
		weston_log("RDP drives: fuse_new failed\n");
		return false;
	}

	for (attempt = 0; attempt < 2; attempt++) {
		if (fuse_mount(d->fuse, d->mount_dir) == 0)
			break;
		/* stale mount from a crashed session ("transport endpoint
		 * is not connected"): lazy unmount and retry once */
		force_unmount(d->mount_dir);
	}
	if (attempt == 2) {
		weston_log("RDP drives: cannot mount %s (is /dev/fuse available? "
			   "In a Proxmox LXC enable Options -> Features -> FUSE)\n",
			   d->mount_dir);
		fuse_destroy(d->fuse);
		d->fuse = NULL;
		return false;
	}

	if (pthread_create(&d->fuse_thread, NULL, fuse_thread_main, d) != 0) {
		fuse_unmount(d->fuse);
		fuse_destroy(d->fuse);
		d->fuse = NULL;
		return false;
	}
	d->fuse_thread_running = true;
	weston_log("RDP drives: client drives available under %s\n", d->mount_dir);
	return true;
}

/* ------------------------------------------------------------------ */
/* rdpdr device callbacks (rdpdr channel thread)                       */
/* ------------------------------------------------------------------ */

static UINT
on_client_name(RdpdrServerContext *context, size_t len, const char *name)
{
	struct rdp_drives *d = drives_from_context(context);

	snprintf(d->client_name, sizeof d->client_name, "%.*s",
		 (int)len, name ? name : "");
	weston_log("RDP rdpdr: client name '%s'\n", d->client_name);
	return CHANNEL_RC_OK;
}

static UINT
on_drive_create(RdpdrServerContext *context, const RdpdrDevice *device)
{
	struct rdp_drives *d = drives_from_context(context);
	int i, slot = -1;
	char name[9];

	snprintf(name, sizeof name, "%.8s", device->PreferredDosName);
	/* strip the trailing ':' some clients include */
	if (name[0] && name[strlen(name) - 1] == ':')
		name[strlen(name) - 1] = '\0';

	pthread_mutex_lock(&d->lock);
	for (i = 0; i < MAX_DRIVES; i++) {
		if (!d->drives[i].used) {
			slot = i;
			break;
		}
	}
	if (slot >= 0) {
		d->drives[slot].used = true;
		d->drives[slot].device_id = device->DeviceId;
		snprintf(d->drives[slot].name, sizeof d->drives[slot].name,
			 "%s", name);
	}
	cache_clear_locked(d);
	pthread_mutex_unlock(&d->lock);

	weston_log("RDP rdpdr: drive '%s' (device %u) from client '%s'%s\n",
		   name, device->DeviceId, d->client_name,
		   slot < 0 ? " ignored, too many drives" : "");
	return CHANNEL_RC_OK;
}

static UINT
on_drive_delete(RdpdrServerContext *context, UINT32 device_id)
{
	struct rdp_drives *d = drives_from_context(context);
	int i;

	pthread_mutex_lock(&d->lock);
	for (i = 0; i < MAX_DRIVES; i++) {
		if (d->drives[i].used && d->drives[i].device_id == device_id) {
			weston_log("RDP rdpdr: drive '%s' removed\n",
				   d->drives[i].name);
			d->drives[i].used = false;
		}
	}
	cache_clear_locked(d);
	pthread_mutex_unlock(&d->lock);
	return CHANNEL_RC_OK;
}

/* MS-RDPEPC 2.2.2.1 DR_PRN_DEVICE_ANNOUNCE device data */
static void
utf16_field_to_utf8(const BYTE *p, UINT32 bytes, char *out, size_t out_size)
{
	if (!bytes || !out_size) {
		if (out_size)
			out[0] = '\0';
		return;
	}
	if (ConvertWCharNToUtf8((const WCHAR *)p, bytes / sizeof(WCHAR),
				out, out_size) < 0)
		out[0] = '\0';
}

static UINT
on_printer_create(RdpdrServerContext *context, const RdpdrDevice *device)
{
	struct rdp_drives *d = drives_from_context(context);
	const BYTE *p = device->DeviceData;
	UINT32 len = device->DeviceDataLength;
	UINT32 flags, pnp_len, drv_len, name_len;
	char driver[256], name[256];

	if (!p || len < 24) {
		weston_log("RDP rdpdr: printer (device %u) without data\n",
			   device->DeviceId);
		return CHANNEL_RC_OK;
	}
	flags = p[0] | p[1] << 8 | p[2] << 16 | (UINT32)p[3] << 24;
	/* CodePage at 4 */
	pnp_len = p[8] | p[9] << 8 | p[10] << 16 | (UINT32)p[11] << 24;
	drv_len = p[12] | p[13] << 8 | p[14] << 16 | (UINT32)p[15] << 24;
	name_len = p[16] | p[17] << 8 | p[18] << 16 | (UINT32)p[19] << 24;
	/* CachedFieldsLen at 20, strings follow at 24 */

	if (24ULL + pnp_len + drv_len + name_len > len) {
		weston_log("RDP rdpdr: printer (device %u) with malformed data\n",
			   device->DeviceId);
		return CHANNEL_RC_OK;
	}
	utf16_field_to_utf8(p + 24 + pnp_len, drv_len, driver, sizeof driver);
	utf16_field_to_utf8(p + 24 + pnp_len + drv_len, name_len, name, sizeof name);

	weston_log("RDP rdpdr: printer '%s' driver '%s' (device %u) flags 0x%x%s%s%s from '%s'\n",
		   name, driver, device->DeviceId, flags,
		   (flags & 0x02) ? " DEFAULT" : "",
		   (flags & 0x10) ? " XPSFORMAT" : "",
		   (flags & 0x08) ? " TSPRINTER" : "",
		   d->client_name);
	return CHANNEL_RC_OK;
}

static UINT
on_printer_delete(RdpdrServerContext *context, UINT32 device_id)
{
	(void)context;
	weston_log("RDP rdpdr: printer (device %u) removed\n", device_id);
	return CHANNEL_RC_OK;
}

/* ------------------------------------------------------------------ */
/* public entry points (compositor thread)                             */
/* ------------------------------------------------------------------ */

void
rdp_drives_init(RdpPeerContext *peer_ctx)
{
	struct rdp_drives *d;
	RdpdrServerContext *rdpdr;

	if (peer_ctx->drives || !peer_ctx->vcm)
		return;
	if (getenv("WESTON_RDP_DISABLE_DRIVES")) {
		weston_log("RDP drives: disabled by WESTON_RDP_DISABLE_DRIVES\n");
		return;
	}

	rdpdr = weston_rdpdr_server_context_new(peer_ctx->vcm);
	if (!rdpdr)
		return;

	d = xzalloc(sizeof *d);
	d->peer_ctx = peer_ctx;
	d->rdpdr = rdpdr;
	pthread_mutex_init(&d->lock, NULL);
	wl_list_init(&d->pending);

	rdpdr->data = d;
	rdpdr->rdpcontext = &peer_ctx->_p;
	rdpdr->supported = RDPDR_DTYP_FILESYSTEM | RDPDR_DTYP_PRINT;
	rdpdr->ReceiveClientNameRequest = on_client_name;
	rdpdr->OnDriveCreate = on_drive_create;
	rdpdr->OnDriveDelete = on_drive_delete;
	rdpdr->OnPrinterCreate = on_printer_create;
	rdpdr->OnPrinterDelete = on_printer_delete;
	rdpdr->OnDriveCreateDirectoryComplete = on_simple_complete;
	rdpdr->OnDriveDeleteDirectoryComplete = on_simple_complete;
	rdpdr->OnDriveQueryDirectoryComplete = on_query_directory_complete;
	rdpdr->OnDriveOpenFileComplete = on_open_complete;
	rdpdr->OnDriveReadFileComplete = on_read_complete;
	rdpdr->OnDriveWriteFileComplete = on_write_complete;
	rdpdr->OnDriveCloseFileComplete = on_simple_complete;
	rdpdr->OnDriveDeleteFileComplete = on_simple_complete;
	rdpdr->OnDriveRenameFileComplete = on_simple_complete;

	peer_ctx->drives = d;

	if (!drives_mount(d))
		weston_log("RDP drives: drive redirection unavailable, printers are still logged\n");

	if (rdpdr->Start(rdpdr) != CHANNEL_RC_OK) {
		/* the client did not join the rdpdr channel (nothing shared) */
		weston_log("RDP rdpdr: channel not available\n");
		rdp_drives_destroy(peer_ctx);
	}
}

void
rdp_drives_destroy(RdpPeerContext *peer_ctx)
{
	struct rdp_drives *d = peer_ctx->drives;
	struct drive_wait *w, *tmp;

	if (!d)
		return;
	peer_ctx->drives = NULL;

	/* wake every FUSE thread waiting for the client */
	pthread_mutex_lock(&d->lock);
	d->shutdown = true;
	wl_list_for_each_safe(w, tmp, &d->pending, link) {
		pthread_mutex_lock(&w->mutex);
		w->cancelled = true;
		pthread_cond_signal(&w->cond);
		pthread_mutex_unlock(&w->mutex);
	}
	pthread_mutex_unlock(&d->lock);

	if (d->fuse) {
		fuse_exit(d->fuse);
		fuse_unmount(d->fuse);
		if (d->fuse_thread_running)
			pthread_join(d->fuse_thread, NULL);
		fuse_destroy(d->fuse);
		d->fuse = NULL;
		weston_log("RDP drives: %s unmounted\n", d->mount_dir);
	}

	/* no completion callbacks after this */
	d->rdpdr->Stop(d->rdpdr);
	weston_rdpdr_server_context_free(d->rdpdr);

	/* the FUSE workers are joined, so nothing is pending any more;
	 * completion references of requests that never finished are leaked
	 * deliberately (a few bytes per lost request at session end) */

	cache_clear(d);
	pthread_mutex_destroy(&d->lock);
	free(d->mount_dir);
	free(d);
}

#else /* !HAVE_FUSE3 */

#include "rdp.h"

void
rdp_drives_init(RdpPeerContext *peer_ctx)
{
	(void)peer_ctx;
}

void
rdp_drives_destroy(RdpPeerContext *peer_ctx)
{
	(void)peer_ctx;
}

#endif
