/* Copyright 2013. The Regents of the University of California.
 * Copyright 2015-2021. Martin Uecker.
 * Copyright 2017-2018. Damien Nguyen.
 * All rights reserved. Use of this source code is governed by
 * a BSD-style license which can be found in the LICENSE file.
 *
 * Authors:
 * 2012-2021 Martin Uecker <martin.uecker@med.uni-goettingen.de>
 * 2017-2018 Damien Nguyen <damien.nguyen@alumni.epfl.ch>
 */

#define _GNU_SOURCE
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include "win/mman.h"
#include "win/vdprintf.h"
#else
#include <sys/mman.h>
#endif
#include <complex.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "num/multind.h"

#include "misc/version.h"
#include "misc/misc.h"
#include "misc/debug.h"

#include "io.h"





static void xdprintf(int fd, const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int ret = vdprintf(fd, fmt, ap);
	va_end(ap);

	if (ret < 0)
		error("Error writing.\n");
}



enum file_types_e file_type(const char* name)
{
	if (0 == strcmp("-", name))
		return FILE_TYPE_PIPE;

	const char *p = strrchr(name, '.');

	if ((NULL != p) && (p != name)) {

		if (0 == strcmp(p, ".ra"))
			return FILE_TYPE_RA;

		if (0 == strcmp(p, ".coo"))
			return FILE_TYPE_COO;

		if (0 == strcmp(p, ".shm"))
			return FILE_TYPE_SHM;

		if (0 == strcmp(p, ".mem"))
			return FILE_TYPE_MEM;
	}

	return FILE_TYPE_CFL;
}


struct iofile_s {

	const char* name;
	bool out;
	bool open;
	struct iofile_s* prev;
};

static struct iofile_s* iofiles = NULL;



static void io_register(const char* name, bool out, bool open)
{
	struct iofile_s* iop = iofiles;
	bool new = true;

	while (NULL != iop) {

		if (0 == strcmp(name, iop->name)) {

			if (iop->open) {

				if (out || iop->out)
					debug_printf(DP_WARN, "Overwriting file: %s\n", name);
			} else {

				if (out != iop->out)
					error("%s: Input opened for writing or output opened for reading!\n", name);

				iop->open = open;
				new = false;
			}
		}

		iop = iop->prev;
	}

	if (new) {

		PTR_ALLOC(struct iofile_s, ion);

		ion->name = strdup(name);
		ion->out = out;
		ion->open = open;
		ion->prev = iofiles;

		iofiles = PTR_PASS(ion);
	}
}

void io_register_input(const char* name)
{
	io_register(name, false, true);
}

void io_register_output(const char* name)
{
	io_register(name, true, true);
}

void io_reserve_input(const char* name)
{
	io_register(name, false, false);
}

void io_reserve_output(const char* name)
{
	io_register(name, true, false);
}

void io_unregister(const char* name)
{
	struct iofile_s** iop = &iofiles;
	struct iofile_s* io;

	while (NULL != (io = *iop)) {

		if (0 == strcmp(name, io->name)) {

			*iop = io->prev;

			xfree(io->name);
			xfree(io);

			continue;
		}

		iop = &io->prev;
	}
}


void io_memory_cleanup(void)
{
	while (NULL != iofiles)
		io_unregister(iofiles->name);
}



void io_unlink_if_opened(const char* name)
{
	const struct iofile_s* iop = iofiles;

	while (NULL != iop) {

		if ( (0 == strcmp(name, iop->name)) && iop->open ) {

			enum file_types_e type = file_type(name);

			switch (type) {

			case FILE_TYPE_RA:
			case FILE_TYPE_COO:

				if (0 != unlink(name))
					error("Failed to unlink file %s\n", name);

				break;

			case FILE_TYPE_CFL:

				;

				char name_bdy[1024];

				if (1024 <= snprintf(name_bdy, 1024, "%s.cfl", name))
					error("Failed to unlink cfl file %s\n", name);

				if (0 != unlink(name_bdy))
					error("Failed to unlink file %s\n", name);

				char name_hdr[1024];

				if (1024 <= snprintf(name_hdr, 1024, "%s.hdr", name))
					error("Failed to unlink cfl file %s\n", name);

				if (0 != unlink(name_hdr))
					error("Failed to unlink file %s\n", name);

				break;

			case FILE_TYPE_SHM:

				if (0 != shm_unlink(name))
					error("Failed to unlink shared memory segment %s\n", name);

				break;

			case FILE_TYPE_PIPE:
				break;

			case FILE_TYPE_MEM:
				break;
			}

			io_unregister(name);

			break;
		}

		iop = iop->prev;
	}
}


int write_cfl_header(int fd, const char* filename, int n, const long dimensions[n])
{
	xdprintf(fd, "# Dimensions\n");

	for (int i = 0; i < n; i++)
		xdprintf(fd, "%ld ", dimensions[i]);

	xdprintf(fd, "\n");

	if (NULL != filename) {

		xdprintf(fd, "# Data\n");
		xdprintf(fd, "%s\n", filename);
	}

	if (NULL != command_line) {

		xdprintf(fd, "# Command\n");
		xdprintf(fd, "%s\n", command_line);
	}

	if (NULL != iofiles) {

		struct iofile_s* in = iofiles;

		xdprintf(fd, "# Files\n");

		while (in) {

			xdprintf(fd, " %c%s", in->out ? '>' : '<', in->name);
			in = in->prev;
		}

		xdprintf(fd, "\n");
	}

	xdprintf(fd, "# Creator\nBART %s\n", bart_version);

	return 0;
}



int read_cfl_header(int fd, char** file, int n, long dimensions[n])
{
	*file = NULL;
	char header[4097];
	memset(header, 0, 4097);

	int max = 0;

	while (max < 4096) {

		int rd;

		if (0 > (rd = read(fd, header + max, 4096 - max)))
			return -1;

		if (0 == rd)
			break;

		max += rd;
	}

	int pos = 0;
	int delta = 0;
	bool ok = false;

	while (true) {

		// skip lines not starting with '#'

		while ('#' != header[pos]) {

			if ('\0' == header[pos])
				goto out;

			if (0 != sscanf(header + pos, "%*[^\n]\n%n", &delta))
				return -1;

			if (0 == delta)
				goto out;

			pos += delta;
		}

		char keyword[32];

		if (1 == sscanf(header + pos, "# %31s\n%n", keyword, &delta)) {

			pos += delta;

			if (0 == strcmp(keyword, "Dimensions")) {

				for (int i = 0; i < n; i++)
					dimensions[i] = 1;

				long val;
				int i = 0;

				while (1 == sscanf(header + pos, "%ld%n", &val, &delta)) {

					pos += delta;

					if (i < n)
						dimensions[i] = val;
					else
					if (1 != val)
						return -1;

					i++;
				}

				if (0 != sscanf(header + pos, "\n%n", &delta))
					return -1;

				pos += delta;

				if (ok)
					return -1;

				ok = true;
			}

			if (0 == strcmp(keyword, "Data")) {

				char filename[256];

				if (1 != sscanf(header + pos, "%255s\n%n", filename, &delta))
					return -1;

				*file = strdup(filename);

				pos += delta;
			}

		} else {

			// skip this line

			if (0 != sscanf(header + pos, "%*[^\n]\n%n", &delta))
				return -1;

			if (0 == delta)
				goto out;

			pos += delta;
		}
	}

out:
	return ok ? 0 : -1;
}




int write_coo(int fd, int n, const long dimensions[n])
{
	char header[4096];
	size_t len = ARRAY_SIZE(header);
	memset(header, 0, 4096);

	int pos = 0;
	int ret;

	ret = snprintf(header + pos, len, "Type: float\nDimensions: %d\n", n);

	if ((ret < 0) || (ret >= (int)len))
		return -1;

	pos += ret;
	len -= ret;

	long start = 0;
	long stride = 1;

	for (int i = 0; i < n; i++) {

		long size = dimensions[i];

		ret = snprintf(header + pos, len, "[%ld\t%ld\t%ld\t%ld]\n", start, stride * size, size, stride);

		if ((ret < 0) || (ret >= (int)len))
			return -1;

		pos += ret;
		len -= ret;

		stride *= size;
	}

	if (4096 != write(fd, header, 4096))
		return -1;

	return 0;
}


int read_coo(int fd, int n, long dimensions[n])
{
	char header[4096];

	if (4096 != read(fd, header, 4096))
		return -1;

	int pos = 0;
	int delta = 0;

	if (0 != sscanf(header + pos, "Type: float\n%n", &delta))
		return -1;

	if (0 == delta)
		return -1;

	pos += delta;

	int dim;

	if (1 != sscanf(header + pos, "Dimensions: %d\n%n", &dim, &delta))
		return -1;

	pos += delta;

//	if (n != dim)
//		return -1;

	for (int i = 0; i < n; i++)
		dimensions[i] = 1;

	for (int i = 0; i < dim; i++) {

		long val;

		if (1 != sscanf(header + pos, "[%*d %*d %ld %*d]\n%n", &val, &delta))
			return -1;

		pos += delta;

		if (i < n)
			dimensions[i] = val;
		else
		if (1 != val)	// fail if we have to many dimensions not equal 1
			return -1;
	}

	return 0;
}





struct ra_hdr_s {

	uint64_t magic;
	uint64_t flags;
	uint64_t eltype;
	uint64_t elbyte;
	uint64_t size;
	uint64_t ndims;
};

#define RA_MAGIC_NUMBER		0x7961727261776172ULL
#define RA_FLAG_BIG_ENDIAN	(1ULL << 0)

enum ra_types {

	RA_TYPE_USER = 0,
	RA_TYPE_INT,
	RA_TYPE_UINT,
	RA_TYPE_FLOAT,
	RA_TYPE_COMPLEX,
};


#define err_assert(x)	({ if (!(x)) { debug_printf(DP_ERROR, "%s", #x); return -1; } })


int read_ra(int fd, int n, long dimensions[n])
{
	struct ra_hdr_s header;

	if (sizeof(header) != read(fd, &header, sizeof(header)))
		return -1;

	err_assert(RA_MAGIC_NUMBER == header.magic);
	err_assert(!(header.flags & RA_FLAG_BIG_ENDIAN));
	err_assert(RA_TYPE_COMPLEX == header.eltype);
	err_assert(sizeof(complex float) == header.elbyte);
	err_assert(header.ndims <= 100);

	uint64_t dims[header.ndims];

	if ((int)sizeof(dims) != read(fd, &dims, sizeof(dims)))
		return -1;

	md_singleton_dims(n, dimensions);

	for (int i = 0; i < (int)header.ndims; i++) {

		if (i < n)
			dimensions[i] = dims[i];
		else
			err_assert(1 == dims[i]);
	}

	// this can overflow, but we check in mmio
	err_assert(header.size == md_calc_size(n, dimensions) * sizeof(complex float));

	return 0;
}



int write_ra(int fd, int n, const long dimensions[n])
{
	struct ra_hdr_s header = {

		.magic = RA_MAGIC_NUMBER,
		.flags = 0ULL,
		.eltype = RA_TYPE_COMPLEX,
		.elbyte = sizeof(complex float),
		.size = md_calc_size(n, dimensions) * sizeof(complex float),
		.ndims = n,
	};

	if (sizeof(header) != write(fd, &header, sizeof(header)))
		return -1;

	uint64_t dims[n];

	for (int i = 0; i < n; i++)
		dims[i] = dimensions[i];

	if ((int)sizeof(dims) != write(fd, &dims, sizeof(dims)))
		return -1;

	return 0;
}




