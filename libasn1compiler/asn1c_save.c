#include "asn1c_internal.h"
#include "asn1c_compat.h"
#include "asn1c_fdeps.h"
#include "asn1c_lang.h"
#include "asn1c_misc.h"
#include "asn1c_save.h"
#include "asn1c_out.h"

static int asn1c_dump_streams(arg_t *arg, asn1c_fdeps_t *, int, int, char **);
static int asn1c_print_streams(arg_t *arg);
static int asn1c_save_streams(arg_t *arg, asn1c_fdeps_t *, int, int, char **);
static int asn1c_copy_over(arg_t *arg, char *path);
static int identical_files(const char *fname1, const char *fname2);
static int generate_pdu_collection_file(arg_t *arg);
static int generate_preamble(arg_t *, FILE *, int argc, int optc, char **argv);

int
asn1c_save_compiled_output(arg_t *arg, const char *datadir,
		int argc, int optc, char **argv) {
	asn1c_fdeps_t *deps = 0;
	asn1c_fdeps_t *dlist;
	asn1p_module_t *mod;
	FILE *mkf;	/* Makefile.am.sample */
	int i;

	deps = asn1c_read_file_dependencies(arg, datadir);
	if(!deps && datadir) {
		WARNING("Cannot read file-dependencies information "
			"from %s\n", datadir);
	}

	TQ_FOR(mod, &(arg->asn->modules), mod_next) {
		TQ_FOR(arg->expr, &(mod->members), next) {
			if(asn1_lang_map[arg->expr->meta_type]
				[arg->expr->expr_type].type_cb) {
				if(asn1c_dump_streams(arg, deps,
						argc, optc, argv))
					return -1;
			}
		}
	}

	/*
	 * Dump out the Makefile template and the rest of the support code.
	 */
	if((arg->flags & A1C_PRINT_COMPILED)
	|| (arg->flags & A1C_OMIT_SUPPORT_CODE)) {
		return 0;	/* Finished */
	}

	mkf = asn1c_open_file("Makefile.am", ".sample", 0);
	if(mkf == NULL) {
		perror("Makefile.am.sample");
		return -1;
	}

	fprintf(mkf, "ASN_MODULE_SOURCES=");
	TQ_FOR(mod, &(arg->asn->modules), mod_next) {
		TQ_FOR(arg->expr, &(mod->members), next) {
			if(asn1_lang_map[arg->expr->meta_type]
				[arg->expr->expr_type].type_cb) {
				fprintf(mkf, "\t\\\n\t%s.c",
				arg->expr->Identifier);
			}
		}
	}
	fprintf(mkf, "\n\nASN_MODULE_HEADERS=");
	TQ_FOR(mod, &(arg->asn->modules), mod_next) {
		TQ_FOR(arg->expr, &(mod->members), next) {
			if(asn1_lang_map[arg->expr->meta_type]
				[arg->expr->expr_type].type_cb) {
				fprintf(mkf, "\t\\\n\t%s.h",
				arg->expr->Identifier);
			}
		}
	}
	fprintf(mkf, "\n\n");

	/*
	 * Move necessary skeleton files and add them to Makefile.am.sample.
	 */
	dlist = asn1c_deps_makelist(deps);
	if(dlist) {
		char buf[8129];
		char *dir_end;
		size_t dlen = strlen(datadir);

		assert(dlen < (sizeof(buf) / 2 - 2));
		memcpy(buf, datadir, dlen);
		dir_end = buf + dlen;
		*dir_end++ = '/';

		for(i = 0; i < dlist->el_count; i++) {
			char *fname = dlist->elements[i]->filename;
			char *dotH;

			assert(strlen(fname) < (sizeof(buf) / 2));
			strcpy(dir_end, fname);

			if(asn1c_copy_over(arg, buf) == -1) {
				fprintf(mkf, ">>>ABORTED<<<");
				fclose(mkf);
				return -1;
			}
			dotH = strrchr(fname, 'h');
			if(dotH && fname<dotH && dotH[-1] == '.' && !dotH[1]) {
				fprintf(mkf, "ASN_MODULE_HEADERS+=%s\n", fname);
			} else {
				fprintf(mkf, "ASN_MODULE_SOURCES+=%s\n", fname);
			}
		}
	}

	if(arg->flags & A1C_PDU_AUTO) {
		fprintf(mkf, "ASN_MODULE_SOURCES+=pdu_collection.c\n");
		if(generate_pdu_collection_file(arg))
			return -1;
	}

	fprintf(mkf, "\n\n"
		"lib_LTLIBRARIES=libsomething.la\n"
		"libsomething_la_SOURCES="
			"$(ASN_MODULE_SOURCES) $(ASN_MODULE_HEADERS)\n"
		"\n"
		"# This file may be used as an input for make(3)\n"
		"# Remove the lines below to convert it into a pure .am file\n"
		"TARGET = progname\n"
		"CFLAGS += -I.\n"
		"OBJS=${ASN_MODULE_SOURCES:.c=.o} $(TARGET).o\n"
		"\nall: $(TARGET)\n"
		"\n$(TARGET): ${OBJS}"
		"\n\t$(CC) $(CFLAGS) -o $(TARGET) ${OBJS} $(LDFLAGS) $(LIBS)\n"
		"\n.SUFFIXES:"
		"\n.SUFFIXES: .c .o\n"
		"\n.c.o:"
		"\n\t$(CC) $(CFLAGS) -o $@ -c $<\n"
		"\nclean:"
		"\n\trm -f $(TARGET)"
		"\n\trm -f $(OBJS)\n"
		"\nregen: regenerate-from-asn1-source\n"
		"\nregenerate-from-asn1-source:\n\t"
	);

	for(i = 0; i < argc; i++)
		fprintf(mkf, "%s%s", i ? " " : "", argv[i]);
	fprintf(mkf, "\n\n");

	fclose(mkf);
	fprintf(stderr, "Generated Makefile.am.sample\n");

	return 0;
}

/*
 * Dump the streams.
 */
static int
asn1c_dump_streams(arg_t *arg, asn1c_fdeps_t *deps, int argc, int optc, char **argv)  {
	if(arg->flags & A1C_PRINT_COMPILED) {
		return asn1c_print_streams(arg);
	} else {
		return asn1c_save_streams(arg, deps, argc, optc, argv);
	}
}

static int
asn1c_print_streams(arg_t *arg)  {
	compiler_streams_t *cs = arg->expr->data;
	asn1p_expr_t *expr = arg->expr;
	int i;

	for(i = 1; i < OT_MAX; i++) {
		out_chunk_t *ot;
		if(TQ_FIRST(&cs->destination[i].chunks) == NULL)
			continue;

		printf("\n/*** <<< %s [%s] >>> ***/\n\n",
			_compiler_stream2str[i],
			expr->Identifier);

		TQ_FOR(ot, &(cs->destination[i].chunks), next) {
			fwrite(ot->buf, ot->len, 1, stdout);
		}
	}

	return 0;
}

static int
asn1c_save_streams(arg_t *arg, asn1c_fdeps_t *deps, int argc, int optc, char **argv) {
	asn1p_expr_t *expr = arg->expr;
	compiler_streams_t *cs = expr->data;
	out_chunk_t *ot;
	FILE *fp_c, *fp_h;
	char *tmpname_c, *tmpname_h;
	char *name_buf;
	char *header_id;
	const char *c_retained = "";
	const char *h_retained = "";

	if(cs == NULL) {
		fprintf(stderr, "Cannot compile %s at line %d\n",
			expr->Identifier, expr->_lineno);
		return -1;
	}

	fp_c = asn1c_open_file(expr->Identifier, ".c", &tmpname_c);
	fp_h = asn1c_open_file(expr->Identifier, ".h", &tmpname_h);
	if(fp_c == NULL || fp_h == NULL) {
		if(fp_c) { unlink(tmpname_c); free(tmpname_c); fclose(fp_c); }
		if(fp_h) { unlink(tmpname_h); free(tmpname_h); fclose(fp_h); }
		return -1;
	}

	generate_preamble(arg, fp_c, argc, optc, argv);
	generate_preamble(arg, fp_h, argc, optc, argv);

	header_id = asn1c_make_identifier(0, expr->Identifier, NULL);
	fprintf(fp_h,
		"#ifndef\t_%s_H_\n"
		"#define\t_%s_H_\n"
		"\n", header_id, header_id);

	fprintf(fp_h, "\n#include <asn_application.h>\n");

#define	SAVE_STREAM(fp, idx, msg, actdep)	do {			\
	if(TQ_FIRST(&(cs->destination[idx].chunks)) && *msg)		\
		fprintf(fp, "\n/* %s */\n", msg);			\
	TQ_FOR(ot, &(cs->destination[idx].chunks), next) {		\
		if(actdep) asn1c_activate_dependency(deps, 0, ot->buf);	\
		fwrite(ot->buf, ot->len, 1, fp);			\
	}								\
} while(0)

	SAVE_STREAM(fp_h, OT_INCLUDES,	"Including external dependencies", 1);

	fprintf(fp_h, "\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n");
	SAVE_STREAM(fp_h, OT_DEPS,	"Dependencies", 0);
	SAVE_STREAM(fp_h, OT_FWD_DECLS,	"Forward declarations", 0);
	SAVE_STREAM(fp_h, OT_TYPE_DECLS, expr->Identifier, 0);
	SAVE_STREAM(fp_h, OT_FUNC_DECLS,"Implementation", 0);
	fprintf(fp_h, "\n#ifdef __cplusplus\n}\n#endif\n");

	if(!(arg->flags & A1C_NO_INCLUDE_DEPS))
	SAVE_STREAM(fp_h, OT_POST_INCLUDE, "Referred external types", 1);

	fprintf(fp_h, "\n#endif\t/* _%s_H_ */\n", header_id);

	fprintf(fp_c, "#include <asn_internal.h>\n\n");
	fprintf(fp_c, "#include \"%s.h\"\n\n", expr->Identifier);
	if(arg->flags & A1C_NO_INCLUDE_DEPS)
		SAVE_STREAM(fp_c, OT_POST_INCLUDE, "", 1);
	TQ_FOR(ot, &(cs->destination[OT_CTABLES].chunks), next)
		fwrite(ot->buf, ot->len, 1, fp_c);
	TQ_FOR(ot, &(cs->destination[OT_CODE].chunks), next)
		fwrite(ot->buf, ot->len, 1, fp_c);
	TQ_FOR(ot, &(cs->destination[OT_STAT_DEFS].chunks), next)
		fwrite(ot->buf, ot->len, 1, fp_c);

	assert(OT_MAX == 10);	/* Protection from reckless changes */

	fclose(fp_c);
	fclose(fp_h);

	name_buf = alloca(strlen(expr->Identifier) + 3);

	sprintf(name_buf, "%s.c", expr->Identifier);
	if(identical_files(name_buf, tmpname_c)) {
		c_retained = " (contents unchanged)";
		unlink(tmpname_c);
	} else {
		if(rename(tmpname_c, name_buf)) {
			unlink(tmpname_c);
			perror(tmpname_c);
			free(tmpname_c);
			free(tmpname_h);
			return -1;
		}
	}

	sprintf(name_buf, "%s.h", expr->Identifier);
	if(identical_files(name_buf, tmpname_h)) {
		h_retained = " (contents unchanged)";
		unlink(tmpname_h);
	} else {
		if(rename(tmpname_h, name_buf)) {
			unlink(tmpname_h);
			perror(tmpname_h);
			free(tmpname_c);
			free(tmpname_h);
			return -1;
		}
	}

	free(tmpname_c);
	free(tmpname_h);

	fprintf(stderr, "Compiled %s.c%s\n",
		expr->Identifier, c_retained);
	fprintf(stderr, "Compiled %s.h%s\n",
		expr->Identifier, h_retained);
	return 0;
}

static int
generate_preamble(arg_t *arg, FILE *fp, int argc, int optc, char **argv) {
	fprintf(fp,
	"/*\n"
	" * Generated by asn1c-" VERSION " (http://lionet.info/asn1c)\n"
	" * From ASN.1 module \"%s\"\n"
	" * \tfound in \"%s\"\n",
		arg->expr->module->ModuleName,
		arg->expr->module->source_file_name);
	if(optc > 1) {
		int i;
		fprintf(fp, " * \t`asn1c ");
		for(i = 1; i < optc; i++)
			fprintf(fp, "%s%s", i>1?" ":"", argv[i]);
		fprintf(fp, "`\n");
	}
	fprintf(fp, " */\n\n");
	return 0;
}

static int
identical_files(const char *fname1, const char *fname2) {
	char buf[2][4096];
	FILE *fp1, *fp2;
	size_t olen, nlen;
	int retval = 1;	/* Files are identical */

#ifndef	WIN32
	struct stat sb;

	if(lstat(fname1, &sb) || !S_ISREG(sb.st_mode)
	|| lstat(fname2, &sb) || !S_ISREG(sb.st_mode)) {
		return 0;	/* Files are not identical */
	}
#endif

	fp1 = fopen(fname1, "r");
	if(!fp1) { return 0; }
	fp2 = fopen(fname2, "r");
	if(!fp2) { fclose(fp1); return 0; }

	while((olen = fread(buf[0], 1, sizeof(buf[0]), fp1))) {
		nlen = fread(buf[1], 1, olen, fp2);
		if(nlen != olen || memcmp(buf[0], buf[1], nlen)) {
			retval = 0;
			break;
		}
	}
	nlen = fread(buf[1], 1, 1, fp2);
	if(nlen) retval = 0;

	fclose(fp1);
	fclose(fp2);
	return retval;
}

/*
 * Copy file for real.
 */
static int
real_copy(const char *src, const char *dst) {
	unsigned char buf[4096];
	char *tmpname;
	FILE *fpsrc, *fpdst;
	size_t len;
	int retval = 0;

	if(identical_files(src, dst))
		return retval;	/* Success, no need to copy for real. */

	fpsrc = fopen(src, "r");
	if(!fpsrc) { errno = EIO; return -1; }
	fpdst = asn1c_open_file(dst, "", &tmpname);
	if(!fpdst) { fclose(fpsrc); errno = EIO; return -1; }

	while(!feof(fpsrc)) {
		len = fread(buf, 1, sizeof(buf), fpsrc);
		if(fwrite(buf, 1, len, fpdst) != len) {
			perror(tmpname);
			errno = EIO;
			retval = -1;
			break;
		}
	}
	fclose(fpsrc);
	fclose(fpdst);

	/* Check if copied correctly, and rename into a permanent name */
	if(retval) {
		unlink(tmpname);
	} else if(rename(tmpname, dst)) {
		unlink(tmpname);
		perror(tmpname);
		retval = -1;
	}
	free(tmpname);
	return retval;
}

static int
asn1c_copy_over(arg_t *arg, char *path) {
	char *fname;
#ifdef	WIN32
	int use_real_copy = 1;
#else
	int use_real_copy = (arg->flags & A1C_SKELETONS_COPY);
#endif

	fname = a1c_basename(path);
	if(!fname
	|| (use_real_copy ? real_copy(path, fname) : symlink(path, fname))
	) {
		if(errno == EEXIST) {
			struct stat sb1, sb2;
			if(stat(path, &sb1) == 0
			&& stat(fname, &sb2) == 0
			&& sb1.st_dev == sb2.st_dev
			&& sb1.st_ino == sb2.st_ino) {
				/*
				 * Nothing to do.
				 */
				fprintf(stderr,
					"File %s is already here as %s\n",
					path, fname);
				return 1;
			} else {
				fprintf(stderr,
					"Retaining local %s (%s suggested)\n",
					fname, path);
				return 1;
			}
		} else if(errno == ENOENT) {
			/* Ignore this */
			return 0;
		} else {
			fprintf(stderr, "%s %s -> %s failed: %s\n",
				use_real_copy ? "Copy" : "Symlink",
				path, fname, strerror(errno));
			return -1;
		}
	}

	fprintf(stderr, "%s %s\t-> %s\n",
		use_real_copy ? "Copied" : "Symlinked", path, fname);

	return 1;
}


static int
generate_pdu_collection_file(arg_t *arg) {
	asn1p_module_t *mod;
	FILE *fp;

	fp = asn1c_open_file("pdu_collection", ".c", 0);
	if(fp == NULL) {
		perror("pdu_collection.c");
		return -1;
	}

	fprintf(fp,
		"/*\n"
		" * Generated by asn1c-" VERSION " (http://lionet.info/asn1c)\n"
		" */\n\n");
	fprintf(fp, "struct asn_TYPE_descriptor_s;\t"
			"/* Forward declaration */\n\n");

	TQ_FOR(mod, &(arg->asn->modules), mod_next) {
		TQ_FOR(arg->expr, &(mod->members), next) {
			if(arg->expr->_type_referenced
			|| !asn1_lang_map[arg->expr->meta_type]
				[arg->expr->expr_type].type_cb)
				continue;
			fprintf(fp, "extern struct asn_TYPE_descriptor_s "
				"asn_DEF_%s;\n",
				asn1c_make_identifier(0, arg->expr->Identifier,
					NULL));
		}
	}

	fprintf(fp, "\n\n");
	fprintf(fp, "struct asn_TYPE_descriptor_s *asn_pdu_collection[] = {\n");
	TQ_FOR(mod, &(arg->asn->modules), mod_next) {
		int mod_printed = 0;
		TQ_FOR(arg->expr, &(mod->members), next) {
			if(arg->expr->_type_referenced
			|| !asn1_lang_map[arg->expr->meta_type]
				[arg->expr->expr_type].type_cb)
				continue;
			if(!mod_printed++)
			fprintf(fp, "\t/* From module %s in %s */\n",
				arg->expr->module->ModuleName,
				arg->expr->module->source_file_name);
			fprintf(fp, "\t&asn_DEF_%s,\t\n",
				asn1c_make_identifier(0, arg->expr->Identifier,
					NULL));
		}
	}

	fprintf(fp, "\t0\n};\n\n");

	fclose(fp);
	fprintf(stderr, "Generated pdu_collection.c\n");

	return 0;
}

