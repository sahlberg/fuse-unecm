/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
/***************************************************************************/
/*
 * ECM FUSE Module
 * by Ronnie Sahlberg <ronniesahlberg@gmail.com>
 *
 * This is an overlay filesystem to transparently uncompress ECM files
 * created by ECM-COMPRESS and which have a matching .ecm.edi index file.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#define _GNU_SOURCE
#define FUSE_USE_VERSION 26
#define _FILE_OFFSET_BITS 64

#include <dirent.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <getopt.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <unistd.h>

#include "libunecm.h"
#include <tdb.h>

#define discard_const(ptr) ((void *)((intptr_t)(ptr)))

#define LOG(...) {                          \
        if (logfile) { \
            FILE *fh = fopen(logfile, "a+"); \
            fprintf(fh, "[UNECM] "); \
            fprintf(fh, __VA_ARGS__); \
            fclose(fh); \
        } \
}

struct file {
        struct ecm *ecm;
        int fd;
};

static char *logfile;

static struct tdb_context *tdb;

/* descriptor for the underlying directory */
static int dir_fd;

/* This function takes a path to a file and returns true if this needs
 * ecm unpacking.
 * For a file <file> we need to unpack the file if
 *   <file>         does not exist
 *   <file>.ecm     exists
 *   <file>.ecm.edi exists
 * In that situation READDIR will just turn a single instance for the name
 * <file> and hide the entries for <file>.ecm and <file>.ecm.edi
 *
 * IF all three files exist, we do not mutate any of the READDIR data
 * and return all three names. Then we just redirect all I/O to the unpacked
 * file. The reason we do not hide the <file>.ecm and <file>.ecm.edi is to make
 * it possible for the user to see that something is odd/wrong and possibly
 * take action. (action == delete the unpacked file, it is redundant.)
 */
static int need_ecm_uncompress(const char *file) {
        char stripped[PATH_MAX];
        char tmp[PATH_MAX];
        struct stat st;
        TDB_DATA key, data;
        uint8_t ret = 1;

        LOG("NEED_ECM_UNCOMPRESS [%s]\n", file);
        key.dptr = discard_const(file);
        key.dsize = strlen(file);
        data = tdb_fetch(tdb, key);
        if (data.dptr) {
                uint8_t val = data.dptr[0];
                free(data.dptr);
                return val;
        }

        snprintf(stripped, PATH_MAX,"%s", file);
        if (strlen(stripped) > 4 &&
            !strcmp(stripped + strlen(stripped) - 4, ".edi")) {
                stripped[strlen(stripped) - 4] = 0;
        }
        if (strlen(stripped) > 4 &&
            !strcmp(stripped + strlen(stripped) - 4, ".ecm")) {
                stripped[strlen(stripped) - 4] = 0;
        }

        if (fstatat(dir_fd, stripped, &st, AT_NO_AUTOMOUNT) == 0) {
                ret = 0;
                goto finished;
        }
        snprintf(tmp, PATH_MAX, "%s.ecm", stripped);
        if (fstatat(dir_fd, tmp, &st, AT_NO_AUTOMOUNT) != 0) {
                ret = 0;
                goto finished;
        }
        snprintf(tmp, PATH_MAX, "%s.ecm.edi", stripped);
        if (fstatat(dir_fd, tmp, &st, AT_NO_AUTOMOUNT) != 0) {
                ret = 0;
                goto finished;
        }
        
finished:
        data.dptr = &ret;
        data.dsize = 1;
        tdb_store(tdb, key, data, TDB_REPLACE);
        return ret;
}

static int fuse_unecm_read(const char *path, char *buf, size_t size,
                           off_t offset, struct fuse_file_info *ffi)
{
        struct file *file;
        int ret;
        
        if (path[0] == '/') {
                path++;
        }

        file = (void *)ffi->fh;
        if (file->ecm) {
                ret = ecm_read(file->ecm, buf, offset, size);
                if (ret == -1) {
                        LOG("READ read [%s] %jd:%zu %s\n", path, offset, size, strerror(errno));
                        return -errno;
                }
                LOG("READ [%s] %jd:%zu %d\n", path, offset, size, ret);
                return ret;
        }
        
        /* Passthrough to underlying filesystem */
        ret = pread(file->fd, buf, size, offset);
        return (ret == -1) ? -errno : ret;
}

static int fuse_unecm_release(const char *path, struct fuse_file_info *ffi)
{
        struct file *file = (struct file *)ffi->fh;
        
        if (path[0] == '/') {
                path++;
        }

        LOG("RELEASE [%s]\n", path);

        if (file == NULL) {
                return 0;
        }
        if (file->ecm) {
                ecm_close_file(file->ecm);
        }
        if (file->fd != -1) {
                close(file->fd);
        }
        free(file);

        return 0;
}

static int fuse_unecm_open(const char *path, struct fuse_file_info *ffi)
{
        struct stat st;
        int ret;
        struct file *file = malloc(sizeof(struct file));

        file->ecm = NULL;
        file->fd = -1;

        if (path[0] == '/') {
                path++;
        }

        ret = fstatat(dir_fd, path, &st, AT_NO_AUTOMOUNT);
        if (ret && errno == ENOENT) {
                if (need_ecm_uncompress(path)) {
                        char tmp[PATH_MAX];

                        snprintf(tmp, PATH_MAX, "%s.ecm", path);
                        file->ecm = ecm_open_file(dir_fd, tmp);
                        if (file->ecm == NULL) {
                                free(file);
                                LOG("Failed to open ECM [%s]\n", path);
                                return -ENOENT;
                        }

                        ffi->fh = (uint64_t)file;
                        return 0;
                }
        }

        file->fd = openat(dir_fd, path, O_RDONLY);
        if (file->fd == -1) {
                free(file);
                LOG("OPEN FD [%s] %s\n", path, strerror(errno));
                return -errno;
        }
        ffi->fh = (uint64_t)file;
        LOG("OPEN FD [%s] SUCCESS\n", path);
        return 0;
}

/* returns the size of the uncompressed file, or 0 if it could not be
 * determined.
 */
static off_t get_uncompressed_size(const char *path)
{
        struct ecm *ecm;
        size_t pos;

        ecm = ecm_open_file(dir_fd, path);
        if (ecm == NULL) {
                LOG("Failed to open ECM file %s in get_uncompressed_size\n",
                    path);
                return 0;
        }
        pos = ecm_get_file_size(ecm);
        ecm_close_file(ecm);
        return pos;
}

static int fuse_unecm_getattr(const char *path, struct stat *stbuf)
{
        int ret;

        if (path[0] == '/') {
                path++;
        }

        ret = fstatat(dir_fd, path, stbuf, AT_NO_AUTOMOUNT|AT_EMPTY_PATH);
        if (ret && errno == ENOENT) {
                if (need_ecm_uncompress(path)) {
                        char tmp[PATH_MAX];
                        int fd;

                        snprintf(tmp, PATH_MAX, "%s.ecm", path);
                        ret = fstatat(dir_fd, tmp, stbuf, AT_NO_AUTOMOUNT);
                        if (ret) {
                                LOG("GETATTR [%s] %s\n", path, strerror(errno));
                                return -errno;
                        }

                        stbuf->st_size = get_uncompressed_size(tmp);
                        LOG("GETATTR [%s] SUCCESS\n", path);
                        return 0;
                }
        }
        if (ret) {
                LOG("GETATTR [%s] SUCCESS\n", path);
                return -errno;
        }
        LOG("GETATTR [%s] SUCCESS\n", path);
        return 0;
}

static int fuse_unecm_readdir(const char *path, void *buf,
                              fuse_fill_dir_t filler,
                              off_t offset, struct fuse_file_info *fi)
{
        DIR *dir;
        struct dirent *ent;
        int fd;

        if (path[0] == '/') {
                path++;
        }
        if (path[0] == '\0') {
                path = ".";
        }

        LOG("READDIR [%s]\n", path);

        fd = openat(dir_fd, path, O_DIRECTORY);
        dir = fdopendir(fd);
        if (dir == NULL) {
                return -errno;
        }
        while ((ent = readdir(dir)) != NULL) {
                struct ecm *e = NULL;
                char full_path[PATH_MAX];

                if (strcmp(path, ".")) {
                        snprintf(full_path, PATH_MAX, "%s/%s",
                                 path, ent->d_name);
                } else {
                        strcpy(full_path, ent->d_name);
                }

                if (need_ecm_uncompress(full_path)) {
                        char tmp[PATH_MAX];

                        snprintf(tmp, PATH_MAX, "%s", ent->d_name);
                        if (strlen(tmp) > 8 &&
                            !strcmp(tmp + strlen(tmp) - 8, ".ecm.edi")) {
                                tmp[strlen(tmp) - 8] = 0;
                                filler(buf, tmp, NULL, 0);
                        }
                        continue;
                }

                filler(buf, ent->d_name, NULL, 0);
        }
        closedir(dir);
        return 0;
}

static int fuse_unecm_statfs(const char *path, struct statvfs* stbuf)
{
        return fstatvfs(dir_fd, stbuf);
}

static struct fuse_operations unecm_oper = {
        .getattr        = fuse_unecm_getattr,
        .open           = fuse_unecm_open,
        .release        = fuse_unecm_release,
        .read           = fuse_unecm_read,
        .readdir        = fuse_unecm_readdir,
        .statfs         = fuse_unecm_statfs,
};

static void print_usage(char *name)
{
        printf("Usage: %s [-?|--help] [-a|--allow-other] "
               "[-m|--mountpoint=mountpoint] "
               "[-l|--logfile=<file> ", name);
        exit(0);
}

int main(int argc, char *argv[])
{
        int c, ret = 0, opt_idx = 0;
        char *mnt = NULL;
        static struct option long_opts[] = {
                { "help", no_argument, 0, '?' },
                { "allow-other", no_argument, 0, 'a' },
                { "logfile", required_argument, 0, 'l' },
                { "mountpoint", required_argument, 0, 'm' },
                { NULL, 0, 0, 0 }
        };
        int fuse_unecm_argc = 6;
        char *fuse_unecm_argv[16] = {
                "fuse-unecm",
                "<export>",
                "-omax_write=32768",
                "-ononempty",
                "-s",
                "-odefault_permissions",
                NULL,
                NULL,
                NULL,
                NULL,
                NULL,
                NULL,
                NULL,
                NULL,
                NULL,
                NULL,
        };
        char fs_name[1024], fs_type[1024];
        
        while ((c = getopt_long(argc, argv, "?hal:m:", long_opts,
                    &opt_idx)) > 0) {
                switch (c) {
                case 'h':
                case '?':
                        print_usage(argv[0]);
                        return 0;
                case 'a':
                        fuse_unecm_argv[fuse_unecm_argc++] = "-oallow_other";
                        break;
                case 'l':
                        logfile = strdup(optarg);
                        break;
                case 'm':
                        mnt = strdup(optarg);
                        break;
                }
        }

        snprintf(fs_name, sizeof(fs_name), "-ofsname=%s", mnt);
        fuse_unecm_argv[fuse_unecm_argc++] = fs_name;

        snprintf(fs_type, sizeof(fs_type), "-osubtype=UNECM");
        fuse_unecm_argv[fuse_unecm_argc++] = fs_type;

        if (mnt == NULL) {
                fprintf(stderr, "-m was not specified.\n");
                print_usage(argv[0]);
                ret = 10;
                exit(1);
        }

        dir_fd = open(mnt, O_DIRECTORY);
        fuse_unecm_argv[1] = mnt;

        tdb = tdb_open(NULL, 10000001, TDB_INTERNAL, O_RDWR, 0);
        if (tdb == NULL) {
                printf("Failed to open TDB\n");
                exit(1);
        }

        return fuse_main(fuse_unecm_argc, fuse_unecm_argv, &unecm_oper, NULL);
}
