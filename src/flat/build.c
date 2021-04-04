/* Copyright (C) 2019 Nunuhara Cabbage <nunuhara@haniwa.technology>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include "system4.h"
#include "system4/buffer.h"
#include "system4/cg.h"
#include "system4/ex.h"
#include "system4/file.h"
#include "system4/flat.h"
#include "system4/string.h"
#include "alice.h"

enum {
	LOPT_OUTPUT = 256,
};

static void buffer_write_file(struct buffer *buf, const char *path)
{
	size_t len;
	uint8_t *file = file_read(path, &len);
	if (!file)
		ALICE_ERROR("reading '%s': %s", path, strerror(errno));
	buffer_write_bytes(buf, file, len);
	free(file);
}

static void pad_align(struct buffer *b)
{
	while (b->index & 3) {
		uint8_t zero = 0;
		buffer_write_bytes(b, &zero, 1);
	}
}

static void deserialize_binary(struct buffer *b, struct string *s)
{
	if (s->size % 2 != 0)
		ALICE_ERROR("Serialized binary data has odd size");

	size_t off = b->index;
	buffer_skip(b, 4);

	for (int i = 0; i < s->size; i += 2) {
		uint8_t n;
		sscanf(s->text+i, "%02hhx", &n);
		buffer_write_bytes(b, &n, 1);
	}

	buffer_write_int32_at(b, off, b->index - off - 4);
	pad_align(b);
}

static void write_libl_files(struct buffer *b, struct ex_table *libl)
{
	// validate fields
	if (libl->nr_fields != 5)
		ALICE_ERROR("Wrong number of columns in 'libl' table");
	if (libl->fields[0].type != EX_STRING)
		ALICE_ERROR("Wrong type for column 'unknown' in 'libl' table");
	if (libl->fields[1].type != EX_INT)
		ALICE_ERROR("Wrong type for column 'type' in 'libl' table");
	if (libl->fields[2].type != EX_INT)
		ALICE_ERROR("Wrong type for column 'has_front' in 'libl' table");
	if (libl->fields[3].type != EX_INT)
		ALICE_ERROR("Wrong type for column 'front' in 'libl' table");
	if (libl->fields[4].type != EX_STRING)
		ALICE_ERROR("Wrong type");

	buffer_write_int32(b, libl->nr_rows);

	for (unsigned i = 0; i < libl->nr_rows; i++) {
		deserialize_binary(b, libl->rows[i][0].s);
		buffer_write_int32(b, libl->rows[i][1].i);

		const char *path = libl->rows[i][4].s->text;
		if (libl->rows[i][2].i) {
			buffer_write_int32(b, file_size(path) + 4);
			buffer_write_int32(b, libl->rows[i][3].i);
		} else {
			buffer_write_int32(b, file_size(path));
		}
		buffer_write_file(b, path);
		pad_align(b);
	}
}

static void write_talt_files(struct buffer *b, struct ex_table *talt)
{
	// validate fields
	if (talt->nr_fields != 2)
		ALICE_ERROR("Wrong number of columns in 'talt' table");
	if (talt->fields[0].type != EX_STRING)
		ALICE_ERROR("Wrong type for column 'unknown' in 'talt' table");
	if (talt->fields[1].type != EX_TABLE)
		ALICE_ERROR("Wrong type for column 'meta' in 'talt' table");
	if (talt->fields[1].nr_subfields != 5)
		ALICE_ERROR("Wrong number of columns in 'talt.meta' table");
	if (talt->fields[1].subfields[0].type != EX_STRING)
		ALICE_ERROR("Wrong type for column 'uk1' in 'talt.meta' table");
	if (talt->fields[1].subfields[1].type != EX_INT)
		ALICE_ERROR("Wrong type for column 'uk2' in 'talt.meta' table");
	if (talt->fields[1].subfields[2].type != EX_INT)
		ALICE_ERROR("Wrong type for column 'uk3' in 'talt.meta' table");
	if (talt->fields[1].subfields[3].type != EX_INT)
		ALICE_ERROR("Wrong type for column 'uk4' in 'talt.meta' table");
	if (talt->fields[1].subfields[4].type != EX_INT)
		ALICE_ERROR("Wrong type for column 'uk5' in 'talt.meta' table");

	buffer_write_int32(b, talt->nr_rows);

	for (unsigned i = 0; i < talt->nr_rows; i++) {
		const char *path = talt->rows[i][0].s->text;
		buffer_write_int32(b, file_size(path));
		buffer_write_file(b, path);
		pad_align(b);

		struct ex_table *meta = talt->rows[i][1].t;
		buffer_write_int32(b, meta->nr_rows);
		for (unsigned i = 0; i < meta->nr_rows; i++) {
			deserialize_binary(b, meta->rows[i][0].s);
			buffer_write_int32(b, meta->rows[i][1].i);
			buffer_write_int32(b, meta->rows[i][2].i);
			buffer_write_int32(b, meta->rows[i][3].i);
			buffer_write_int32(b, meta->rows[i][4].i);
		}
	}
}

static struct flat_archive *build_flat(struct ex *ex)
{
	struct buffer b;
	struct flat_archive *flat = flat_new();
	buffer_init(&b, NULL, 0);

	if (ex_get_int(ex, "elna", 0)) {
		flat->elna.present = true;
		flat->elna.off = b.index;
		flat->elna.size = 0;
		buffer_write_bytes(&b, (uint8_t*)"ELNA", 4);
		buffer_write_int32(&b, 0);
	}

	struct string *flat_path = ex_get_string(ex, "flat");
	if (!flat_path)
		ALICE_ERROR("'flat' path missing from .flat manifest");
	flat->flat.present = true;
	flat->flat.off = b.index;
	buffer_write_file(&b, flat_path->text);
	flat->flat.size = b.index - flat->flat.off - 8;
	free_string(flat_path);

	struct string *tmnl_path = ex_get_string(ex, "tmnl");
	if (tmnl_path) {
		flat->tmnl.present = true;
		flat->tmnl.off = b.index;
		buffer_write_file(&b, tmnl_path->text);
		flat->tmnl.size = b.index - flat->tmnl.off - 8;
		free_string(tmnl_path);
	}

	struct string *mtlc_path = ex_get_string(ex, "mtlc");
	if (!mtlc_path)
		ALICE_ERROR("'mtlc' path missing from .flat manifest");
	flat->mtlc.present = true;
	flat->mtlc.off = b.index;
	buffer_write_file(&b, mtlc_path->text);
	flat->mtlc.size = b.index - flat->mtlc.off - 8;
	free_string(mtlc_path);

	struct ex_table *libl = ex_get_table(ex, "libl");
	if (!libl)
		ALICE_ERROR("'libl' table missing from .flat manifest");
	flat->libl.present = true;
	flat->libl.off = b.index;
	buffer_write_bytes(&b, (uint8_t*)"LIBL", 4);
	buffer_write_int32(&b, 0);
	write_libl_files(&b, libl);
	// write the section size now that we know it
	buffer_write_int32_at(&b, flat->libl.off + 4, b.index - flat->libl.off - 8);

	struct ex_table *talt = ex_get_table(ex, "talt");
	if (talt) {
		flat->talt.present = true;
		flat->talt.off = b.index;
		buffer_write_bytes(&b, (uint8_t*)"TALT", 4);
		buffer_write_int32(&b, 0);
		write_talt_files(&b, talt);
		// write the section size now that we know it
		buffer_write_int32_at(&b, flat->talt.off + 4, b.index - flat->talt.off - 8);
	}

	flat->data_size = b.index;
	flat->data = b.buf;
	return flat;
}

int command_flat_build(int argc, char *argv[])
{
	char *output_file = NULL;

	while (1) {
		int c = alice_getopt(argc, argv, &cmd_flat_build);
		if (c == -1)
			break;
		switch (c) {
		case 'o':
		case LOPT_OUTPUT:
			output_file = optarg;
			break;
		}
	}

	argc -= optind;
	argv += optind;

	// check argument count
	if (argc != 1) {
		USAGE_ERROR(&cmd_flat_build, "Wrong number of arguments");
	}

	// open .txtex file
	struct ex *ex = ex_parse_file(argv[0]);
	if (!ex) {
		ALICE_ERROR("Failed to read flat manifest file: %s", argv[0]);
	}

	FILE *out = checked_fopen(output_file ? output_file : "out.flat", "wb");
	chdir_to_file(argv[0]); // for relative paths
	struct flat_archive *flat = build_flat(ex);
	checked_fwrite(flat->data, flat->data_size, out);
	fclose(out);

	archive_free(&flat->ar);
	ex_free(ex);
	return 0;
}

struct command cmd_flat_build = {
	.name = "build",
	.usage = "[options] <input-file>",
	.description = "Build a .flat file",
	.parent = &cmd_flat,
	.fun = command_flat_build,
	.options = {
		{ "output", 'o', "Specify the output file", required_argument, LOPT_OUTPUT },
		{ 0 }
	}
};
