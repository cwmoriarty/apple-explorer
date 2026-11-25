#define FUSE_USE_VERSION 31
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "apple_explorer.h"
#include <errno.h>
#include <fuse.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    aex_t          *fs;
    pthread_mutex_t lock;
    int             override_uid;
    uid_t           uid;
    int             override_gid;
    gid_t           gid;
} aex_fuse_state_t;

static aex_fuse_state_t g_state;

static void attr_to_stat(const aex_attr_t *a, struct stat *st) {
    memset(st, 0, sizeof(*st));
    st->st_ino = (ino_t)a->ino;
    st->st_mode = (mode_t)(a->mode & ~(uint32_t)0222);
    st->st_nlink = (nlink_t)(a->nlink ? a->nlink : 1U);
    st->st_uid = g_state.override_uid ? g_state.uid : (uid_t)a->uid;
    st->st_gid = g_state.override_gid ? g_state.gid : (gid_t)a->gid;
    st->st_size = (off_t)a->size;
    st->st_blocks = (blkcnt_t)((a->size + 511ULL) / 512ULL);
    st->st_atime = (time_t)a->atime_sec;
    st->st_mtime = (time_t)a->mtime_sec;
    st->st_ctime = (time_t)a->ctime_sec;
}

static int af_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi) {
    (void)fi;
    pthread_mutex_lock(&g_state.lock);
    uint64_t ino = 0;
    int rc = aex_lookup_path(g_state.fs, path, &ino);
    if (rc == 0) {
        aex_attr_t attr;
        rc = aex_getattr(g_state.fs, ino, &attr);
        if (rc == 0) attr_to_stat(&attr, stbuf);
    }
    pthread_mutex_unlock(&g_state.lock);
    return rc;
}

typedef struct {
    void           *buf;
    fuse_fill_dir_t filler;
} af_dir_pack_t;

static int af_dir_cb(void *user_ctx, const char *name, uint64_t ino, aex_ftype_t ftype) {
    af_dir_pack_t *pk = (af_dir_pack_t *)user_ctx;
    struct stat st;
    memset(&st, 0, sizeof(st));
    st.st_ino = (ino_t)ino;
    st.st_mode = (ftype == AEX_FT_DIR) ? (S_IFDIR | 0555) : (ftype == AEX_FT_SYMLINK) ? (S_IFLNK | 0777) : (S_IFREG | 0444);
    return pk->filler(pk->buf, name, &st, 0, (enum fuse_fill_dir_flags)0);
}

static int af_readdir(
    const char *path,
    void *buf,
    fuse_fill_dir_t filler,
    off_t offset,
    struct fuse_file_info *fi,
    enum fuse_readdir_flags flags
) {
    (void)offset; (void)fi; (void)flags;
    pthread_mutex_lock(&g_state.lock);
    uint64_t ino = 0;
    int rc = aex_lookup_path(g_state.fs, path, &ino);
    if (rc == 0) {
        filler(buf, ".", NULL, 0, (enum fuse_fill_dir_flags)0);
        filler(buf, "..", NULL, 0, (enum fuse_fill_dir_flags)0);
        af_dir_pack_t pk = { buf, filler };
        rc = aex_readdir(g_state.fs, ino, af_dir_cb, &pk);
    }
    pthread_mutex_unlock(&g_state.lock);
    return rc;
}

static int af_open(const char *path, struct fuse_file_info *fi) {
    if ((fi->flags & O_ACCMODE) != O_RDONLY) return -EROFS;
    pthread_mutex_lock(&g_state.lock);
    uint64_t ino = 0;
    int rc = aex_lookup_path(g_state.fs, path, &ino);
    if (rc == 0) fi->fh = ino;
    pthread_mutex_unlock(&g_state.lock);
    return rc;
}

static int af_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    if (offset < 0) return -EINVAL;
    pthread_mutex_lock(&g_state.lock);
    uint64_t ino = (fi && fi->fh != 0) ? fi->fh : 0;
    int rc = 0;
    if (ino == 0) rc = aex_lookup_path(g_state.fs, path, &ino);
    size_t got = 0;
    if (rc == 0) rc = aex_read(g_state.fs, ino, (uint64_t)offset, size, buf, &got);
    pthread_mutex_unlock(&g_state.lock);
    return (rc < 0) ? rc : (int)got;
}

static int af_readlink(const char *path, char *buf, size_t size) {
    pthread_mutex_lock(&g_state.lock);
    uint64_t ino = 0;
    int rc = aex_lookup_path(g_state.fs, path, &ino);
    if (rc == 0) {
        size_t got = 0;
        rc = aex_readlink(g_state.fs, ino, buf, size, &got);
    }
    pthread_mutex_unlock(&g_state.lock);
    return rc;
}

static int af_statfs(const char *path, struct statvfs *stbuf) {
    (void)path;
    memset(stbuf, 0, sizeof(*stbuf));
    pthread_mutex_lock(&g_state.lock);
    uint64_t tot = 0, used = 0;
    uint32_t bsz = 4096U;
    int rc = aex_statfs(g_state.fs, &tot, &used, &bsz);
    if (rc == 0 && bsz > 0) {
        stbuf->f_bsize = bsz;
        stbuf->f_frsize = bsz;
        stbuf->f_blocks = (fsblkcnt_t)(tot / bsz);
        stbuf->f_bfree = 0;
        stbuf->f_bavail = 0;
        stbuf->f_namemax = 255U;
    }
    pthread_mutex_unlock(&g_state.lock);
    return rc;
}

static void af_destroy(void *priv) {
    (void)priv;
    if (g_state.fs) {
        aex_close(g_state.fs);
        g_state.fs = NULL;
    }
    pthread_mutex_destroy(&g_state.lock);
}

static const struct fuse_operations k_ops = {
    .getattr  = af_getattr,
    .readlink = af_readlink,
    .open     = af_open,
    .read     = af_read,
    .statfs   = af_statfs,
    .readdir  = af_readdir,
    .destroy  = af_destroy,
};

static void filter_mount_options(const char *raw_opts, char *out_opts, size_t out_cap) {
    snprintf(out_opts, out_cap, "ro");
    if (!raw_opts || !raw_opts[0]) return;
    char *copy = strdup(raw_opts);
    if (!copy) return;
    char *saveptr = NULL;
    for (char *tok = strtok_r(copy, ",", &saveptr); tok != NULL; tok = strtok_r(NULL, ",", &saveptr)) {
        if (strncmp(tok, "uid=", 4) == 0) {
            g_state.override_uid = 1;
            g_state.uid = (uid_t)strtoul(tok + 4, NULL, 10);
        } else if (strncmp(tok, "gid=", 4) == 0) {
            g_state.override_gid = 1;
            g_state.gid = (gid_t)strtoul(tok + 4, NULL, 10);
        } else if (strcmp(tok, "allow_other") == 0 ||
                   strcmp(tok, "default_permissions") == 0 ||
                   strncmp(tok, "volname=", 8) == 0) {
#ifndef __APPLE__
            if (strncmp(tok, "volname=", 8) == 0) continue;
#endif
            size_t cur = strlen(out_opts);
            snprintf(out_opts + cur, out_cap - cur, ",%s", tok);
        }
    }
    free(copy);
}

int main(int argc, char **argv) {
    memset(&g_state, 0, sizeof(g_state));
    pthread_mutex_init(&g_state.lock, NULL);

    aex_open_options_t opts = { -1, -1, NULL, NULL };
    char pw_buf[512] = "";
    const char *target = NULL;
    const char *mountpoint = NULL;
    int foreground = 0;
    int debug = 0;
    char filtered_opts[512];
    snprintf(filtered_opts, sizeof(filtered_opts), "ro");

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-f") == 0) foreground = 1;
        else if (strcmp(a, "-d") == 0) { debug = 1; foreground = 1; }
        else if (strcmp(a, "-r") == 0) { /* always ro */ }
        else if (strcmp(a, "--partition") == 0 && i + 1 < argc) opts.partition_index = atoi(argv[++i]);
        else if (strcmp(a, "--volume") == 0 && i + 1 < argc) opts.volume_index = atoi(argv[++i]);
        else if (strcmp(a, "--snapshot") == 0 && i + 1 < argc) opts.snapshot_name = argv[++i];
        else if (strcmp(a, "--password-stdin") == 0) {
            if (fgets(pw_buf, sizeof(pw_buf), stdin)) {
                size_t n = strlen(pw_buf);
                while (n > 0 && (pw_buf[n - 1] == '\n' || pw_buf[n - 1] == '\r')) pw_buf[--n] = '\0';
                opts.password = pw_buf;
            }
        } else if (strcmp(a, "-o") == 0 && i + 1 < argc) {
            filter_mount_options(argv[++i], filtered_opts, sizeof(filtered_opts));
        } else if (strcmp(a, "--version") == 0 || strcmp(a, "-V") == 0) {
            printf("apple-explorer-mount %s\n", aex_version());
            return 0;
        } else if (a[0] != '-') {
            if (!target) target = a;
            else if (!mountpoint) mountpoint = a;
        }
    }

    if (!target || !mountpoint) {
        fprintf(stderr, "Usage: %s [-f] [--volume N] [--snapshot name] [--password-stdin] [-o opts] <target> <mountpoint>\n", argv[0]);
        return 1;
    }

    int rc = aex_open_path(target, &opts, &g_state.fs);
    memset(pw_buf, 0, sizeof(pw_buf));
    if (rc != 0) {
        fprintf(stderr, "%s: failed to open %s: %s\n", argv[0], target, aex_strerror(rc));
        return 1;
    }

    char *fuse_argv[8];
    int fuse_argc = 0;
    fuse_argv[fuse_argc++] = argv[0];
    if (foreground) fuse_argv[fuse_argc++] = "-f";
    if (debug) fuse_argv[fuse_argc++] = "-d";
    fuse_argv[fuse_argc++] = "-o";
    fuse_argv[fuse_argc++] = filtered_opts;
    fuse_argv[fuse_argc++] = (char *)mountpoint;
    fuse_argv[fuse_argc] = NULL;

    return fuse_main(fuse_argc, fuse_argv, &k_ops, NULL);
}
