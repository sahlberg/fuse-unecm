/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
/***************************************************************************/
/*
** Library to unpack .ECM
** Ronnie Sahlberg <ronniesahlberg@gmail.com>
**
**
** Significantly based on unecm.c, a copy of which is included in this
** package for reference.
**
** UNECM - Decoder for ECM (Error Code Modeler) format.
** Version 1.0
** Copyright (C) 2002 Neill Corlett
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of the GNU General Public License
** as published by the Free Software Foundation; either version 2
** of the License, or (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <endian.h>
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

struct ecm {
        int fd;
        uint32_t idx_size;
        off_t *idx_data;

        off_t unpacked_offset;
        off_t ecm_offset;
        size_t skip;
        size_t unpacked_size;
};

#define LOG(...) { \
        if (logfile) { \
            FILE *fh = fopen(logfile, "a+"); \
            fprintf(fh, "[UNECM] "); \
            fprintf(fh, __VA_ARGS__); \
            fclose(fh); \
        } \
}


extern char *logfile;

/* LUTs used for computing ECC/EDC */
static uint8_t ecc_f_lut[256];
static uint8_t ecc_b_lut[256];
static uint32_t edc_lut[256];

/* Init routine */
static void eccedc_init(void)
{
        uint32_t i, j, edc;

        for(i = 0; i < 256; i++) {
                j = (i << 1) ^ (i & 0x80 ? 0x11D : 0);
                ecc_f_lut[i] = j;
                ecc_b_lut[i ^ j] = i;
                edc = i;
                for (j = 0; j < 8; j++) {
                        edc = (edc >> 1) ^ (edc & 1 ? 0xD8018001 : 0);
                }
                edc_lut[i] = edc;
        }
}


/***************************************************************************/
/*
** Compute EDC for a block
*/
static uint32_t edc_partial_computeblock(uint32_t  edc, const uint8_t  *src,
                                         uint16_t  size)
{
        while (size--) {
                edc = (edc >> 8) ^ edc_lut[(edc ^ (*src++)) & 0xFF];
        }
        return edc;
}

static void edc_computeblock(const uint8_t *src, uint16_t size, uint8_t *dest)
{
  uint32_t edc = edc_partial_computeblock(0, src, size);
  dest[0] = (edc >>  0) & 0xFF;
  dest[1] = (edc >>  8) & 0xFF;
  dest[2] = (edc >> 16) & 0xFF;
  dest[3] = (edc >> 24) & 0xFF;
}

/***************************************************************************/
/*
** Compute ECC for a block (can do either P or Q)
*/
static void ecc_computeblock(uint8_t *src,
                             uint32_t major_count,
                             uint32_t minor_count,
                             uint32_t major_mult,
                             uint32_t minor_inc,
                             uint8_t *dest)
{
        uint32_t size = major_count * minor_count;
        uint32_t major, minor;

        for(major = 0; major < major_count; major++) {
                uint32_t index = (major >> 1) * major_mult + (major & 1);
                uint8_t ecc_a = 0;
                uint8_t ecc_b = 0;

                for (minor = 0; minor < minor_count; minor++) {
                        uint8_t temp = src[index];
                        index += minor_inc;
                        if (index >= size) {
                                index -= size;
                        }
                        ecc_a ^= temp;
                        ecc_b ^= temp;
                        ecc_a = ecc_f_lut[ecc_a];
                }
                ecc_a = ecc_b_lut[ecc_f_lut[ecc_a] ^ ecc_b];
                dest[major] = ecc_a;
                dest[major + major_count] = ecc_a ^ ecc_b;
        }
}

/*
** Generate ECC P and Q codes for a block
*/
static void ecc_generate(uint8_t *sector, int zeroaddress)
{
        uint8_t address[4], i;

        /* Save the address and zero it out */
        if (zeroaddress) {
                for(i = 0; i < 4; i++) {
                        address[i] = sector[12 + i];
                        sector[12 + i] = 0;
                }
        }
        /* Compute ECC P code */
        ecc_computeblock(sector + 0xC, 86, 24,  2, 86, sector + 0x81C);
        /* Compute ECC Q code */
        ecc_computeblock(sector + 0xC, 52, 43, 86, 88, sector + 0x8C8);
        /* Restore the address */
        if (zeroaddress) {
                for(i = 0; i < 4; i++) {
                        sector[12 + i] = address[i];
                }
        }
}

/*
** Generate ECC/EDC information for a sector (must be 2352 = 0x930 bytes)
** Returns 0 on success
*/
static void eccedc_generate(uint8_t *sector, int type) {
        int i;

        switch(type) {
        case BLOCK_MODE_1: /* Mode 1 */
                /* Compute EDC */
                edc_computeblock(sector + 0x00, 0x810, sector + 0x810);
                /* Write out zero bytes */
                for(i = 0; i < 8; i++) {
                        sector[0x814 + i] = 0;
                }
                /* Generate ECC P/Q codes */
                ecc_generate(sector, 0);
                break;
        case BLOCK_MODE_2_FORM_1: /* Mode 2 form 1 */
                /* Compute EDC */
                edc_computeblock(sector + 0x10, 0x808, sector + 0x818);
                /* Generate ECC P/Q codes */
                ecc_generate(sector, 1);
                break;
        case BLOCK_MODE_2_FORM_2: /* Mode 2 form 2 */
                /* Compute EDC */
                edc_computeblock(sector + 0x10, 0x91C, sector + 0x92C);
                break;
        }
}

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

static void ecm_seek(struct ecm *ecm, off_t offset)
{
        int idx = offset / 65536;

        ecm->unpacked_offset = ecm->idx_data[2 * idx];
        ecm->ecm_offset = ecm->idx_data[2 * idx + 1];
        ecm->skip = 0;

        while (1) {
                off_t current = ecm->ecm_offset;
                uint8_t ecm_type;
                uint32_t ecm_len;
                int u_len, e_len;

                ecm_read_tag(ecm->fd, &ecm_len, &ecm_type, &current);
                if (ecm_len == 0xFFFFFFFF) {
                        return;
                }
                ecm_len++;

                switch (ecm_type) {
                case BLOCK_BYTES:
                        u_len = ecm_len;
                        e_len = ecm_len;
                        break;
                case BLOCK_MODE_1:
                        u_len = BIN_BLOCK_SIZE * ecm_len;
                        e_len = 0x803 * ecm_len;
                        break;
                case BLOCK_MODE_2_FORM_1:
                        u_len = 2336 * ecm_len;
                        e_len = 0x804 * ecm_len;
                        break;
                case BLOCK_MODE_2_FORM_2:
                        u_len = 2336 * ecm_len;
                        e_len = 0x918 * ecm_len;
                        break;
                }

                if (offset < ecm->unpacked_offset + u_len) {
                        break;
                }

                ecm->unpacked_offset += u_len;
                ecm->ecm_offset = current + e_len;
        }
        ecm->skip = offset - ecm->unpacked_offset;
}

struct ecm *ecm_open_file(int dir_fd, const char *file)
{
        struct ecm *ecm;
        uint8_t magic[4];
        int idx_fd, i, len;
        char *idx_file;
        
        ecm = malloc(sizeof(struct ecm));
        if (ecm == NULL) {
                return NULL;
        }

        ecm->fd = openat(dir_fd, file, 0);
        if (ecm->fd == -1) {
                free(ecm);
                return NULL;
        }

        if (read(ecm->fd, magic, 4) != 4 || strcmp(magic, "ECM")) {
                close(ecm->fd);
                free(ecm);
                return NULL;
        }

        asprintf(&idx_file, "%s.edi", file);
        idx_fd = openat(dir_fd, idx_file, 0);
        free(idx_file);

        if (idx_fd == -1) {
                close(ecm->fd);
                free(ecm);
                return NULL;
        }
        
        read(idx_fd, &ecm->idx_size, sizeof(uint32_t));
        ecm->idx_size = le32toh(ecm->idx_size);

        len = 2 * ecm->idx_size * sizeof(off_t);
        ecm->idx_data = malloc(len);
        if (ecm->idx_data == NULL) {
                close(idx_fd);
                close(ecm->fd);
                free(ecm);
                return NULL;
        }

        lseek(idx_fd, 2 * sizeof(uint32_t), SEEK_SET);
        if (read(idx_fd, ecm->idx_data, len) != len) {
                close(idx_fd);
                close(ecm->fd);
                free(ecm->idx_data);
                free(ecm);
                return NULL;
        }

        for (i = 0; i < ecm->idx_size; i ++) {
                ecm->idx_data[2 * i] = le64toh(ecm->idx_data[2 * i]);
                ecm->idx_data[2 * i + 1] = le64toh(ecm->idx_data[2 * i + 1]);
        }
        close(idx_fd);

        // Find out what the uncompressed size is
        ecm->unpacked_size = ecm->idx_data[2 * (ecm->idx_size -1)];
        while (1) {
                uint8_t buf[4096];
                ssize_t count;

                ecm_seek(ecm, ecm->unpacked_size);
                count = ecm_read(ecm, buf, ecm->unpacked_size, 4096);
                if (count == 0) {
                        break;
                }
                ecm->unpacked_size += count;
        }

        return ecm;
}

void ecm_close_file(struct ecm *ecm)
{
        close(ecm->fd);
        free(ecm->idx_data);
        free(ecm);
}

static int ecm_unpack_block(struct ecm *ecm, char *sector, int len)
{
        uint8_t ecm_type;
        uint32_t ecm_len;
        off_t ecm_offset = ecm->ecm_offset;
        uint8_t buf[2352];
        int idx;
        size_t skip;
        static int first_time = 1;
        
        if (first_time) {
                eccedc_init();
                first_time =0;
        }
        
        if (ecm_read_tag(ecm->fd, &ecm_len, &ecm_type, &ecm_offset) < 0) {
                return -1;
        }
        if (ecm_len == 0xFFFFFFFF) {
                return 0;
        }
        ecm_len++;

        switch (ecm_type) {
        case BLOCK_BYTES:
                if (ecm_type != BLOCK_BYTES) {
                        return -1;
                }
                skip = ecm->skip;

                if (len > ecm_len - skip) {
                        len = ecm_len - skip;
                }
                
                return pread(ecm->fd, sector, len, ecm_offset + skip);

        case BLOCK_MODE_1:
                idx = ecm->skip / BIN_BLOCK_SIZE;
                skip = ecm->skip % BIN_BLOCK_SIZE;

                memset(buf, 0, 16);
                memset(buf + 1, 0xFF, 10);

                buf[0x0F] = 0x01;
                pread(ecm->fd, buf + 0x00C, 0x003,
                      ecm_offset + idx * 0x803);
                pread(ecm->fd, buf + 0x010, 0x800,
                      ecm_offset + 0x003 + idx * 0x803);
                eccedc_generate(buf, 1);

                if (len > BIN_BLOCK_SIZE - skip) {
                        len = BIN_BLOCK_SIZE - skip;
                }
                memcpy(sector, buf + 16 + skip, len);
                return len;
                
        case BLOCK_MODE_2_FORM_1:
                idx = ecm->skip / 2336;
                skip = ecm->skip % 2336;

                memset(buf, 0, 16);
                memset(buf + 1, 0xFF, 10);

                buf[0x0F] = 0x02;
                pread(ecm->fd, buf + 0x014, 0x804,
                      ecm_offset + idx * 0x804);
                buf[0x10] = buf[0x14];
                buf[0x11] = buf[0x15];
                buf[0x12] = buf[0x16];
                buf[0x13] = buf[0x17];
                eccedc_generate(buf, 2);

                if (len > 2336 - skip) {
                        len = 2336 - skip;
                }
                memcpy(sector, buf + 16 + skip, len);
                return len;

        case BLOCK_MODE_2_FORM_2:
                idx = ecm->skip / 2336;
                skip = ecm->skip % 2336;

                memset(buf, 0, 16);
                memset(buf + 1, 0xFF, 10);

                buf[0x0F] = 0x02;
                pread(ecm->fd, buf + 0x014, 0x918,
                      ecm_offset + idx * 0x918);
                buf[0x10] = buf[0x14];
                buf[0x11] = buf[0x15];
                buf[0x12] = buf[0x16];
                buf[0x13] = buf[0x17];
                eccedc_generate(buf, 3);

                if (len > 2336 - skip) {
                        len = 2336 - skip;
                }
                memcpy(sector, buf + 16 + skip, len);
                return len;

        default:
                printf("Can not unpack type %d\n", ecm_type);
                exit(10);
        }
}

ssize_t ecm_read(struct ecm *ecm, char *buf, off_t offset, size_t len)
{
        ssize_t total = 0;
        
        while (len) {
                int count;

                ecm_seek(ecm, offset);
                count = ecm_unpack_block(ecm, buf, len);
                if (!count) {
                        return total;
                }
                total  += count;
                offset += count;
                buf    += count;
                len    -= count;
        }
        return total;
}

size_t ecm_get_file_size(struct ecm *ecm)
{
        return ecm->unpacked_size;
}
