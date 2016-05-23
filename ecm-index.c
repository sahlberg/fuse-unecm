/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
/***************************************************************************/
/*
 * Program to generate a ECM index
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
#define _FILE_OFFSET_BITS 64

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "libunecm.h"

#define BIN_BLOCK_SIZE 2352
#define BLOCK_BYTES         0
#define BLOCK_MODE_1        1
#define BLOCK_MODE_2_FORM_1 2
#define BLOCK_MODE_2_FORM_2 3

static int ecm_read_tag(int fd, uint32_t *count, uint8_t *type, off_t *pos)
{
        uint32_t num;
        int t;
        uint8_t c;
        int bits = 5;

        if (pread(fd, &c, 1, (*pos)++) == 0) {
                return -1;
        }

        t = c & 3;
        num = (c >> 2) & 0x1F;
        while(c & 0x80) {
                if (pread(fd, &c, 1, (*pos)++) == 0) {
                        return -1;
                }
                num |= ((unsigned int)c & 0x7F) << bits;
                bits += 7;
        }
        *type = t;
        *count = num;
        return 0;
}

static void usage(void)
{
        printf("Usage: ecm-index <file>\n");
}

static uint32_t index_size;

static void add_to_index(int fd, off_t upos, uint32_t usize, off_t cpos)
{
        static off_t next;
        
        if (usize == 0) {
                off_t tmp;

                tmp = htole64(upos);
                write(fd, &tmp, sizeof(tmp));
                tmp = htole64(cpos);
                write(fd, &tmp, sizeof(tmp));

                index_size++;
                next = 65536;
                return;
        }
        while (upos + usize > next) {
                off_t tmp;

                tmp = htole64(upos);
                write(fd, &tmp, sizeof(tmp));
                tmp = htole64(cpos);
                write(fd, &tmp, sizeof(tmp));

                index_size++;
                next += 65536;
        }
}

int main(int argc, char *argv[])
{
        int ifd, ofd;
        char *ofile = NULL;
        uint8_t magic[4];
        off_t upos, cpos;
        
        if (argc != 2) {
                usage();
                exit(1);
        }

        if ((ifd = open(argv[1], O_RDONLY)) == -1) {
                printf("Failed to open ECM file : %s\n", strerror(errno));
                exit(1);
        }
        
        if (read(ifd, magic, 4) != 4 || strcmp(magic, "ECM")) {
                close(ifd);
                exit(1);
        }

        asprintf(&ofile, "%s.edi", argv[1]);
        if ((ofd = open(ofile, O_CREAT|O_WRONLY, 0644)) == -1) {
                printf("Failed to create index file %s : %s\n",
                       ofile, strerror(errno));
                free(ofile);
                close(ifd);
                exit(1);
        }
        lseek(ofd, 2 * sizeof(uint32_t), SEEK_SET);
        
        printf("Creating index file\n");
        upos = 0;
        cpos = 4;
        add_to_index(ofd, upos, 0, cpos);
        while (1) {
                uint32_t count;
                uint8_t type;
                off_t current = cpos;
                
                if (ecm_read_tag(ifd, &count, &type, &cpos) < 0) {
                        printf("Failed to read tag\n");
                        free(ofile);
                        close(ifd);
                        close(ofd);
                        exit(1);
                }
                if (count == 0xFFFFFFFF) {
                        break;
                }
                count++;
                
                switch (type) {
                case BLOCK_BYTES:
                        add_to_index(ofd, upos, count, current);
                        upos += count;
                        cpos += count;
                        break;
                case BLOCK_MODE_1:
                        add_to_index(ofd, upos, 2352 * count, current);
                        upos += 2352 * count;
                        cpos += 0x803 * count;
                        break;
                case BLOCK_MODE_2_FORM_1:
                        add_to_index(ofd, upos, 2336 * count, current);
                        upos += 2336 * count;
                        cpos += 0x804 * count;
                        break;
                case BLOCK_MODE_2_FORM_2:
                        add_to_index(ofd, upos, 2336 * count, current);
                        upos += 2336 * count;
                        cpos += 0x918 * count;
                        break;
                }
        }
        index_size = htole32(index_size);
        pwrite(ofd, &index_size, sizeof(uint32_t), 0);
        printf("Wrote %d entries to index\n", index_size);
               
        free(ofile);
        close(ifd);
        close(ofd);
        
        return 0;
}
