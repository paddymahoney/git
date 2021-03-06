#include "cache.h"
#include "commit.h"
#include "diff.h"
#include "revision.h"
#include "builtin.h"
#include "reachable.h"
#include "parse-options.h"
#include "progress.h"
#include "dir.h"

static const char * const prune_usage[] = {
	N_("git prune [-n] [-v] [--expire <time>] [--] [<head>...]"),
	NULL
};
static int show_only;
static int verbose;
static unsigned long expire;
static int show_progress = -1;

static int prune_tmp_object(const char *path, const char *filename)
{
	const char *fullpath = mkpath("%s/%s", path, filename);
	struct stat st;
	if (lstat(fullpath, &st))
		return error("Could not stat '%s'", fullpath);
	if (st.st_mtime > expire)
		return 0;
	if (show_only || verbose)
		printf("Removing stale temporary file %s\n", fullpath);
	if (!show_only)
		unlink_or_warn(fullpath);
	return 0;
}

static int prune_object(char *path, const char *filename, const unsigned char *sha1)
{
	const char *fullpath = mkpath("%s/%s", path, filename);
	struct stat st;
	if (lstat(fullpath, &st))
		return error("Could not stat '%s'", fullpath);
	if (st.st_mtime > expire)
		return 0;
	if (show_only || verbose) {
		enum object_type type = sha1_object_info(sha1, NULL);
		printf("%s %s\n", sha1_to_hex(sha1),
		       (type > 0) ? typename(type) : "unknown");
	}
	if (!show_only)
		unlink_or_warn(fullpath);
	return 0;
}

static int prune_dir(int i, char *path)
{
	DIR *dir = opendir(path);
	struct dirent *de;

	if (!dir)
		return 0;

	while ((de = readdir(dir)) != NULL) {
		char name[100];
		unsigned char sha1[20];

		if (is_dot_or_dotdot(de->d_name))
			continue;
		if (strlen(de->d_name) == 38) {
			sprintf(name, "%02x", i);
			memcpy(name+2, de->d_name, 39);
			if (get_sha1_hex(name, sha1) < 0)
				break;

			/*
			 * Do we know about this object?
			 * It must have been reachable
			 */
			if (lookup_object(sha1))
				continue;

			prune_object(path, de->d_name, sha1);
			continue;
		}
		if (!prefixcmp(de->d_name, "tmp_obj_")) {
			prune_tmp_object(path, de->d_name);
			continue;
		}
		fprintf(stderr, "bad sha1 file: %s/%s\n", path, de->d_name);
	}
	closedir(dir);
	if (!show_only)
		rmdir(path);
	return 0;
}

static void prune_object_dir(const char *path)
{
	int i;
	for (i = 0; i < 256; i++) {
		static char dir[4096];
		sprintf(dir, "%s/%02x", path, i);
		prune_dir(i, dir);
	}
}

/*
 * Write errors (particularly out of space) can result in
 * failed temporary packs (and more rarely indexes and other
 * files beginning with "tmp_") accumulating in the object
 * and the pack directories.
 */
static void remove_temporary_files(const char *path)
{
	DIR *dir;
	struct dirent *de;

	dir = opendir(path);
	if (!dir) {
		fprintf(stderr, "Unable to open directory %s\n", path);
		return;
	}
	while ((de = readdir(dir)) != NULL)
		if (!prefixcmp(de->d_name, "tmp_"))
			prune_tmp_object(path, de->d_name);
	closedir(dir);
}

int cmd_prune(int argc, const char **argv, const char *prefix)
{
	struct rev_info revs;
	struct progress *progress = NULL;
	const struct option options[] = {
		OPT__DRY_RUN(&show_only, N_("do not remove, show only")),
		OPT__VERBOSE(&verbose, N_("report pruned objects")),
		OPT_BOOL(0, "progress", &show_progress, N_("show progress")),
		OPT_EXPIRY_DATE(0, "expire", &expire,
				N_("expire objects older than <time>")),
		OPT_END()
	};
	char *s;

	expire = ULONG_MAX;
	save_commit_buffer = 0;
	read_replace_refs = 0;
	init_revisions(&revs, prefix);

	argc = parse_options(argc, argv, prefix, options, prune_usage, 0);
	while (argc--) {
		unsigned char sha1[20];
		const char *name = *argv++;

		if (!get_sha1(name, sha1)) {
			struct object *object = parse_object_or_die(sha1, name);
			add_pending_object(&revs, object, "");
		}
		else
			die("unrecognized argument: %s", name);
	}

	if (show_progress == -1)
		show_progress = isatty(2);
	if (show_progress)
		progress = start_progress_delay("Checking connectivity", 0, 0, 2);

	mark_reachable_objects(&revs, 1, progress);
	stop_progress(&progress);
	prune_object_dir(get_object_directory());

	prune_packed_objects(show_only ? PRUNE_PACKED_DRY_RUN : 0);
	remove_temporary_files(get_object_directory());
	s = mkpathdup("%s/pack", get_object_directory());
	remove_temporary_files(s);
	free(s);
	return 0;
}
