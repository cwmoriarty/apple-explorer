#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "apple_explorer.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
int aex_win_mount(aex_t *fs, const char *mountpoint, const char *backend);
#endif

static const char *container_name(aex_container_kind_t k) {
    switch (k) {
        case AEX_CONTAINER_SPARSEBUNDLE: return "sparsebundle";
        case AEX_CONTAINER_SPARSEIMAGE:  return "sparseimage";
        case AEX_CONTAINER_UDIF_DMG:     return "udif-dmg";
        default:                         return "raw";
    }
}

static const char *fs_name(aex_fs_kind_t k) {
    switch (k) {
        case AEX_FS_HFS:     return "HFS";
        case AEX_FS_HFSPLUS: return "HFS+";
        case AEX_FS_APFS:    return "APFS";
        default:             return "Unknown";
    }
}

typedef struct {
    aex_t *fs;
    int    as_json;
    int    first;
} ls_ctx_t;

static int ls_cb(void *user_ctx, const char *name, uint64_t ino, aex_ftype_t ftype) {
    ls_ctx_t *lc = (ls_ctx_t *)user_ctx;
    aex_attr_t attr;
    memset(&attr, 0, sizeof(attr));
    (void)aex_getattr(lc->fs, ino, &attr);
    const char *tstr = (ftype == AEX_FT_DIR) ? "dir" : (ftype == AEX_FT_SYMLINK) ? "lnk" : "reg";
    if (lc->as_json) {
        printf("%s\n    {\"name\":\"%s\",\"ino\":%llu,\"type\":\"%s\",\"size\":%llu}",
               lc->first ? "" : ",",
               name,
               (unsigned long long)ino,
               tstr,
               (unsigned long long)attr.size);
        lc->first = 0;
    } else {
        printf("  %-4s %12llu  %s%s\n",
               tstr,
               (unsigned long long)attr.size,
               name,
               (ftype == AEX_FT_DIR) ? "/" : "");
    }
    return 0;
}

static void read_stdin_line(char *buf, size_t cap) {
    buf[0] = '\0';
    if (fgets(buf, (int)cap, stdin)) {
        size_t n = strlen(buf);
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    }
}

static void print_usage(const char *prog) {
    printf(
        "apple-explorer %s - HFS, HFS+, APFS, FileVault 2, and Time Machine Explorer\n\n"
        "Usage: %s <command> <target> [args] [options]\n\n"
        "Commands:\n"
        "  probe <target> [--json]                  Inspect disk, .dmg, or .sparsebundle\n"
        "  partitions <target> [--json]             List GPT, APM, or MBR partitions\n"
        "  volumes <target> [--json]                List APFS container volumes or HFS volume\n"
        "  snapshots <target> [--json]              List APFS snapshots and Time Machine dates\n"
        "  ls <target> [path] [--json]              List directory entries (default: /)\n"
        "  cat <target> <path>                      Write file contents to stdout\n"
        "  extract <target> <src_path> <dest_path>  Extract file, folder, or backup snapshot\n"
        "  mount <target> <mountpoint>              Mount via WinFsp/ProjFS (Win) or FUSE3\n\n"
        "Options:\n"
        "  --partition <N>        Select partition index explicitly\n"
        "  --volume <N>           Select APFS volume index explicitly\n"
        "  --snapshot <name>      Pin root to an APFS snapshot or Time Machine date\n"
        "  --password <pw>        Passphrase for FileVault 2 or encrypted .dmg/.sparsebundle\n"
        "  --password-stdin       Read passphrase from standard input\n"
        "  --backend <mode>       Windows mount backend: auto | winfsp | projfs\n"
        "  --json                 Output structured JSON\n",
        aex_version(), prog
    );
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 0;
    }

    const char *cmd = argv[1];
    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-v") == 0 || strcmp(cmd, "version") == 0) {
        printf("apple-explorer %s\n", aex_version());
        return 0;
    }
    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "help") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    aex_open_options_t opts = { -1, -1, NULL, NULL };
    char pw_buf[512] = "";
    int as_json = 0;
    const char *backend = "auto";
    const char *pos[4] = { NULL, NULL, NULL, NULL };
    int npos = 0;

    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--json") == 0) as_json = 1;
        else if (strcmp(a, "--partition") == 0 && i + 1 < argc) opts.partition_index = atoi(argv[++i]);
        else if (strcmp(a, "--volume") == 0 && i + 1 < argc) opts.volume_index = atoi(argv[++i]);
        else if (strcmp(a, "--snapshot") == 0 && i + 1 < argc) opts.snapshot_name = argv[++i];
        else if (strcmp(a, "--password") == 0 && i + 1 < argc) opts.password = argv[++i];
        else if (strcmp(a, "--password-stdin") == 0) {
            read_stdin_line(pw_buf, sizeof(pw_buf));
            opts.password = pw_buf;
        } else if (strcmp(a, "--backend") == 0 && i + 1 < argc) backend = argv[++i];
        else if (a[0] != '-' && npos < 4) pos[npos++] = a;
    }

    if (npos < 1) {
        print_usage(argv[0]);
        return 1;
    }
    const char *target = pos[0];

    if (strcmp(cmd, "probe") == 0) {
        aex_probe_info_t info;
        int rc = aex_probe_path(target, opts.password, &info);
        if (rc != 0) {
            fprintf(stderr, "probe failed on %s: %s\n", target, aex_strerror(rc));
            return 1;
        }
        if (as_json) {
            printf("{\n"
                   "  \"container\": \"%s\",\n"
                   "  \"containerEncrypted\": %s,\n"
                   "  \"virtualSize\": %llu,\n"
                   "  \"bandSize\": %u,\n"
                   "  \"partitions\": %u,\n"
                   "  \"volumes\": %u,\n"
                   "  \"fileSystem\": \"%s\",\n"
                   "  \"label\": \"%s\",\n"
                   "  \"hasBackupsDb\": %s,\n"
                   "  \"hasApfsSnapshots\": %s\n"
                   "}\n",
                   container_name(info.container_kind),
                   info.container_encrypted ? "true" : "false",
                   (unsigned long long)info.virtual_size,
                   info.band_size,
                   info.partition_count,
                   info.volume_count,
                   fs_name(info.primary_fs),
                   info.primary_label,
                   info.has_backups_db ? "true" : "false",
                   info.has_apfs_snapshots ? "true" : "false");
        } else {
            printf("Target:         %s\n", target);
            printf("  Container:    %s%s\n", container_name(info.container_kind), info.container_encrypted ? " (encrypted)" : "");
            printf("  Virtual Size: %llu bytes\n", (unsigned long long)info.virtual_size);
            if (info.band_size > 0) printf("  Band Size:    %u bytes\n", info.band_size);
            printf("  File System:  %s\n", fs_name(info.primary_fs));
            printf("  Label:        %s\n", info.primary_label[0] ? info.primary_label : "(untitled)");
            printf("  Partitions:   %u\n", info.partition_count);
            printf("  Volumes:      %u\n", info.volume_count);
            if (info.has_backups_db) printf("  Time Machine: HFS+ Backups.backupdb detected\n");
            if (info.has_apfs_snapshots) printf("  Snapshots:    APFS snapshots / Time Machine timeline detected\n");
        }
        return 0;
    }

    if (strcmp(cmd, "partitions") == 0) {
        aex_stream_t *st = NULL;
        int rc = aex_stream_open_path(target, opts.password, &st);
        if (rc != 0) {
            fprintf(stderr, "failed to open %s: %s\n", target, aex_strerror(rc));
            return 1;
        }
        aex_partition_desc_t parts[32];
        size_t cnt = 0;
        rc = aex_list_partitions(st, parts, 32U, &cnt);
        aex_stream_close(st);
        if (rc != 0) {
            fprintf(stderr, "no partitions found on %s\n", target);
            return 1;
        }
        if (as_json) {
            printf("[");
            for (size_t i = 0; i < cnt && i < 32U; i++) {
                printf("%s\n  {\"index\":%u,\"scheme\":\"%s\",\"offset\":%llu,\"size\":%llu,\"fs\":\"%s\",\"name\":\"%s\"}",
                       i ? "," : "", parts[i].index, parts[i].scheme,
                       (unsigned long long)parts[i].offset_bytes,
                       (unsigned long long)parts[i].size_bytes,
                       fs_name(parts[i].detected_fs), parts[i].name);
            }
            printf("\n]\n");
        } else {
            for (size_t i = 0; i < cnt && i < 32U; i++) {
                printf("  [%u] %-5s %-8s offset=%llu size=%llu \"%s\"\n",
                       parts[i].index, parts[i].scheme, fs_name(parts[i].detected_fs),
                       (unsigned long long)parts[i].offset_bytes,
                       (unsigned long long)parts[i].size_bytes,
                       parts[i].name);
            }
        }
        return 0;
    }

    if (strcmp(cmd, "volumes") == 0) {
        aex_volume_desc_t vols[16];
        size_t cnt = 0;
        int rc = aex_list_volumes_path(target, opts.password, vols, 16U, &cnt);
        if (rc != 0) {
            fprintf(stderr, "failed to list volumes on %s: %s\n", target, aex_strerror(rc));
            return 1;
        }
        if (as_json) {
            printf("[");
            for (size_t i = 0; i < cnt && i < 16U; i++) {
                printf("%s\n  {\"index\":%u,\"kind\":\"%s\",\"name\":\"%s\",\"role\":\"%s\",\"encrypted\":%s,\"snapshots\":%u}",
                       i ? "," : "", vols[i].index, fs_name(vols[i].kind), vols[i].name,
                       vols[i].role, vols[i].is_encrypted ? "true" : "false", vols[i].snapshot_count);
            }
            printf("\n]\n");
        } else {
            for (size_t i = 0; i < cnt && i < 16U; i++) {
                printf("  [%u] %-5s %-12s \"%s\"%s (snapshots: %u)\n",
                       vols[i].index, fs_name(vols[i].kind), vols[i].role, vols[i].name,
                       vols[i].is_encrypted ? " [FileVault2]" : "", vols[i].snapshot_count);
            }
        }
        return 0;
    }

    aex_t *fs = NULL;
    int rc = aex_open_path(target, &opts, &fs);
    if (rc != 0) {
        fprintf(stderr, "failed to open filesystem on %s: %s\n", target, aex_strerror(rc));
        return 1;
    }

    if (strcmp(cmd, "snapshots") == 0) {
        aex_snapshot_desc_t snaps[128];
        size_t cnt = 0;
        (void)aex_list_snapshots(fs, snaps, 128U, &cnt);
        if (as_json) {
            printf("[");
            for (size_t i = 0; i < cnt && i < 128U; i++) {
                printf("%s\n  {\"xid\":%llu,\"timestamp\":%lld,\"name\":\"%s\"}",
                       i ? "," : "", (unsigned long long)snaps[i].xid,
                       (long long)snaps[i].timestamp_sec, snaps[i].name);
            }
            printf("\n]\n");
        } else if (cnt == 0) {
            printf("No APFS snapshots on volume \"%s\".\n", aex_volume_name(fs));
        } else {
            for (size_t i = 0; i < cnt && i < 128U; i++) {
                printf("  xid=%-8llu %s\n", (unsigned long long)snaps[i].xid, snaps[i].name);
            }
        }
        aex_close(fs);
        return 0;
    }

    if (strcmp(cmd, "ls") == 0) {
        const char *lpath = pos[1] ? pos[1] : "/";
        uint64_t ino = 0;
        rc = aex_lookup_path(fs, lpath, &ino);
        if (rc != 0) {
            fprintf(stderr, "path not found '%s': %s\n", lpath, aex_strerror(rc));
            aex_close(fs);
            return 1;
        }
        ls_ctx_t lc = { fs, as_json, 1 };
        if (as_json) printf("[");
        else printf("Volume \"%s\" (%s) — %s:\n", aex_volume_name(fs), fs_name(aex_fs_kind(fs)), lpath);
        rc = aex_readdir(fs, ino, ls_cb, &lc);
        if (as_json) printf("\n]\n");
        aex_close(fs);
        return (rc == 0) ? 0 : 1;
    }

    if (strcmp(cmd, "cat") == 0) {
        if (!pos[1]) {
            fprintf(stderr, "Usage: %s cat <target> <path>\n", argv[0]);
            aex_close(fs);
            return 1;
        }
        uint64_t ino = 0;
        rc = aex_lookup_path(fs, pos[1], &ino);
        aex_attr_t attr;
        if (rc == 0) rc = aex_getattr(fs, ino, &attr);
        if (rc != 0) {
            fprintf(stderr, "cat failed on '%s': %s\n", pos[1], aex_strerror(rc));
            aex_close(fs);
            return 1;
        }
        uint8_t buf[65536];
        uint64_t off = 0;
        while (off < attr.size) {
            size_t got = 0;
            rc = aex_read(fs, ino, off, sizeof(buf), buf, &got);
            if (rc != 0 || got == 0) break;
            fwrite(buf, 1U, got, stdout);
            off += got;
        }
        aex_close(fs);
        return (rc == 0) ? 0 : 1;
    }

    if (strcmp(cmd, "extract") == 0) {
        if (!pos[1] || !pos[2]) {
            fprintf(stderr, "Usage: %s extract <target> <src_path> <dest_path>\n", argv[0]);
            aex_close(fs);
            return 1;
        }
        rc = aex_extract_path(fs, pos[1], pos[2]);
        aex_close(fs);
        if (rc != 0) {
            fprintf(stderr, "extract failed: %s\n", aex_strerror(rc));
            return 1;
        }
        printf("Extracted '%s' -> '%s'\n", pos[1], pos[2]);
        return 0;
    }

    if (strcmp(cmd, "mount") == 0) {
        if (!pos[1]) {
            fprintf(stderr, "Usage: %s mount <target> <mountpoint_or_drive>\n", argv[0]);
            aex_close(fs);
            return 1;
        }
#ifdef _WIN32
        rc = aex_win_mount(fs, pos[1], backend);
        aex_close(fs);
        return (rc == 0) ? 0 : 1;
#else
        (void)backend;
        aex_close(fs);
        fprintf(stderr, "On macOS/Linux, mount using: apple-explorer-mount <target> <mountpoint>\n");
        return 1;
#endif
    }

    aex_close(fs);
    print_usage(argv[0]);
    return 1;
}
