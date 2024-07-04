/*
 * Copyright (c) 2013 RIPE NCC <atlas@ripe.net>
 * Licensed under GPLv2 or later, see file LICENSE in this tarball for details.
 * condmv.c -- move a file only if the destination doesn't exist
 */
//config:config CONDMV
//config:       bool "condmv"
//config:       default n
//config:       help
//config:         condmv is used to rename a file if the destination does not exists

//applet:IF_CONDMV(APPLET(condmv, BB_DIR_ROOT, BB_SUID_DROP))

//kbuild:lib-$(CONFIG_CONDMV) += condmv.o

//usage:#define condmv_trivial_usage
//usage:       "[-A <string to append>][-f] FILE1 FILE2"
//usage:#define condmv_full_usage "\n\n"
//usage:       "Rename FILE1 to FILE2 if FILE2 does not exist\n"
//usage:     "\nOptions:"
//usage:     "\n       -A <string>     Append <string> before renaming FILE1"
//usage:     "\n       -f              Force. Move even if FILE2 does exist"

#include "libbb.h"
#include "atlas_path.h"

#define SAFE_PREFIX_FROM1_REL ATLAS_DATA_NEW_REL
#define SAFE_PREFIX_FROM2_REL ATLAS_DATA_OUT_REL
#define SAFE_PREFIX_TO1_REL ATLAS_DATA_OUT_REL
#define SAFE_PREFIX_TO2_REL ATLAS_DATA_STORAGE_REL

#define A_FLAG	(1 << 0)
#define a_FLAG	(1 << 1)
#define D_FLAG	(1 << 2)
#define f_FLAG	(1 << 3)
#define t_FLAG	(1 << 4)
#define x_FLAG	(1 << 5)

static time_t age_value;
static int cross_filesystems, append_timestamp;

static int do_dir(char *from_dir, char *to_dir);
static int do_cprm(char *from_file, char *to_file);

int condmv_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int condmv_main(int argc, char *argv[])
{
	int r;
	char *opt_add, *opt_age, *from, *to, *check;
	char *rebased_from= NULL;
	char *rebased_to= NULL;
	uint32_t opt;
	struct stat sb;
	FILE *file;
	time_t mytime;

	opt_add= NULL;
	opt_age= NULL;
	opt_complementary= NULL;	/* For when we are called by crond */
	opt= getopt32(argv, "!A:a:Dftx", &opt_add, &opt_age);

	if (opt == (uint32_t)-1)
	{
		fprintf(stderr, "condmv: bad options\n");
		return 1;
	}

	if (argc != optind + 2)
	{
		fprintf(stderr, "condmv: two arguments expected\n");
		return 1;
	}

	from= argv[optind];
	to= argv[optind+1];

	rebased_from= rebased_validated_filename(ATLAS_SPOOLDIR,
		from, SAFE_PREFIX_FROM1_REL);
	if (rebased_from == NULL)
	{
		rebased_from= rebased_validated_filename(ATLAS_SPOOLDIR,
			from, SAFE_PREFIX_FROM2_REL);
	}
	if (rebased_from == NULL)
	{
		fprintf(stderr, "insecure from file '%s'\n", from);
		goto err;
	}
	rebased_to= rebased_validated_filename(ATLAS_SPOOLDIR,
		to, SAFE_PREFIX_TO1_REL);
	if (rebased_to == NULL)
	{
		rebased_to= rebased_validated_filename(ATLAS_SPOOLDIR,
			to, SAFE_PREFIX_TO2_REL);
	}
	if (rebased_to == NULL)
	{
		rebased_to= rebased_validated_filename(ATLAS_SPOOLDIR,
			to, SAFE_PREFIX_FROM1_REL);
	}
	if (rebased_to == NULL)
	{
		fprintf(stderr, "insecure to file '%s'\n", to);
		goto err;
	}

	if (opt_age)
	{
		age_value= strtol(opt_age, &check, 0);
		if (check[0] != '\0' || age_value <= 0)
		{
			fprintf(stderr, "bad age value '%s'\n", opt_age);
			goto err;
		}
	}
	else
		age_value= 0;

	cross_filesystems= !!(opt & x_FLAG);
	append_timestamp= !!(opt & t_FLAG);

	if (opt & D_FLAG)
	{
		r= do_dir(rebased_from, rebased_to);
		free(rebased_from); rebased_from= NULL;
		free(rebased_to); rebased_to= NULL;
		return r;
	}

	if (stat(rebased_to, &sb) == 0 && !(opt & f_FLAG))
	{
		/* Destination exists */
		fprintf(stderr,
			"condmv: not moving, destination '%s' exists\n",
			rebased_to);
		goto err;
	}

	if (opt_add)
	{
		mytime = time(NULL);
		/* We have to add something to the existing file before moving
		 * to.
		 */
		file= fopen(rebased_from, "a");
		if (file == NULL)
		{
			fprintf(stderr,
				"condmv: unable to append to '%s': %s\n",
				rebased_from, strerror(errno));
			goto err;
		}
		if (fprintf(file, "%s %llu %s\n", opt_add,
			(unsigned long long)mytime, from) < 0)
		{
			fprintf(stderr,
				"condmv: unable to append to '%s': %s\n",
				rebased_from, strerror(errno));
			fclose(file);
			goto err;
		}
		if (fclose(file) != 0)
		{
			fprintf(stderr,
				"condmv: unable to close '%s': %s\n",
				rebased_from, strerror(errno));
			goto err;
		}
	}

	
	/* Make sure that rebased_from exists before trying to rename */
	if (stat(rebased_from, &sb) == -1 && errno == ENOENT)
	{
		/* Leave now to avoid a diagnostic from the rename code */
		goto err;
	}

	if (rename(rebased_from, rebased_to) != 0)
	{
		fprintf(stderr, "condmv: unable to rename '%s' to '%s': %s\n",
				rebased_from, rebased_to, strerror(errno));
		goto err;
	}

	free(rebased_from); rebased_from= NULL;
	free(rebased_to); rebased_to= NULL;
	return 0;

err:
	if (rebased_from) free(rebased_from);
	if (rebased_to) free(rebased_to);
	return 1;
}

static int do_dir(char *from_dir, char *to_dir)
{
	int r, error;
	size_t len, extra_len;
	time_t now;
	DIR *dir;
	struct dirent *de;
	char *from_file, *new_from_file;
	char *to_file, *new_to_file;
	size_t from_file_len, to_file_len;
	struct stat sb;

	from_file= NULL;
	from_file_len= 0;
	to_file= NULL;
	to_file_len= 0;

	dir= opendir(from_dir);
	if (dir == NULL)
	{
		fprintf(stderr, "condmv: unable to open dir '%s': %s\n",
				from_dir, strerror(errno));
		return 1;
	}

	now= time (NULL);	/* For age_value */

	error= 0;	/* Assume no failures */
	while (de= readdir(dir), de != NULL)
	{
		len= strlen(from_dir) + 1 + strlen(de->d_name) + 1;
		if (len > from_file_len)
		{
			new_from_file= realloc(from_file, len);
			if (new_from_file == NULL)
			{
				fprintf(stderr,
				"condmv: out of memory (from_file)\n");
				error= 1;
				break;
			}
			from_file= new_from_file; new_from_file= NULL;
			from_file_len= len;
		}
		snprintf(from_file, from_file_len, "%s/%s",
			from_dir, de->d_name);
		r= stat(from_file, &sb);
		if (r == -1)
		{
			fprintf(stderr, "condmv: stat %s failed: %s\n",
				from_file, strerror(errno));
			error= 1;
			break;
		}
		if (!S_ISREG(sb.st_mode))
		{
			/* Skip non-regular objects */
			continue;
		}

		if (age_value)
		{
			if (sb.st_mtime + age_value > now)
				continue;
		}

		if (append_timestamp)
		{
			/* A unix timestamp is currently 10 characters.
			 * Allocate an extra 16 characters to have enough
			 * space, also for the separator.
			 */
			extra_len= 16;
		}
		else
			extra_len= 0;
		len= strlen(to_dir) + 1 + strlen(de->d_name) + extra_len + 1;
		if (len > to_file_len)
		{
			new_to_file= realloc(to_file, len);
			if (new_to_file == NULL)
			{
				fprintf(stderr,
				"condmv: out of memory (to_file)\n");
				error= 1;
				break;
			}
			to_file= new_to_file; new_to_file= NULL;
			to_file_len= len;
		}
		if (append_timestamp)
		{
			snprintf(to_file, to_file_len, "%s/%s.%lu",
				to_dir, de->d_name, (unsigned long)now);
		}
		else
		{
			snprintf(to_file, to_file_len, "%s/%s",
				to_dir, de->d_name);
		}

		/* Make sure to_file doesn't exist */
		r= stat(to_file, &sb);
		if (r == 0 || (r == -1 && errno != ENOENT))
		{
			/* Something wrong with to_file */
			continue;
		}

		if (cross_filesystems)
		{
			r= do_cprm(from_file, to_file);
			if (r == 0)
			{
				/* Okay, next one */
				continue;
			}
			error= 1;
			break;
		}

		r= rename(from_file, to_file);
		if (r == -1)
		{
			fprintf(stderr,
				"condmv: rename %s to %s failed: %s\n",
				from_file, to_file, strerror(errno));
			error= 1;
			break;
		}
	}

	closedir(dir);

	if (from_file)
	{
		free(from_file);
		from_file= NULL;
	}
	if (to_file)
	{
		free(to_file);
		to_file= NULL;
	}

	return error;
}

static int do_cprm(char *from_file, char *to_file)
{
	FILE *fp_in, *fp_out;
	size_t len_in, len_out;
	char buf[1024];

	fp_in= fopen(from_file, "rb");
	if (fp_in == NULL)
	{
		fprintf(stderr, "condmv: cannot open '%s' for reading: %s\n",
			from_file, strerror(errno));
		return 1;
	}

	fp_out= fopen(to_file, "wb");
	if (fp_out == NULL)
	{
		fprintf(stderr, "condmv: cannot open '%s' for writing: %s\n",
			to_file, strerror(errno));
		fclose(fp_in); fp_in= NULL;
		return 1;
	}
	
	for (;;)
	{
		len_in= fread(buf, 1, sizeof(buf), fp_in);
		if (len_in == 0)
			break;	/* EOF or error */
		
		len_out= fwrite(buf, 1, len_in, fp_out);
		if (len_out != len_in)
		{
			fprintf(stderr,
				"condmv: error writing to '%s': %s\n",
				to_file, strerror(errno));
			fclose(fp_in); fp_in= NULL;
			fclose(fp_out); fp_out= NULL;
			unlink(to_file);
			return 1;
		}
	}

	if (ferror(fp_in))
	{
		fprintf(stderr,
			"condmv: error reading from '%s': %s\n",
			from_file, strerror(errno));
		fclose(fp_in); fp_in= NULL;
		fclose(fp_out); fp_out= NULL;
		unlink(to_file);
		return 1;
	}

	fclose(fp_in); fp_in= NULL;
	fclose(fp_out); fp_out= NULL;
	unlink(from_file);

	return 0;
}
