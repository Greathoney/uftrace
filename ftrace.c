#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <argp.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dlfcn.h>

#include "mcount.h"
#include "symbol.h"

const char *argp_program_version = "ftrace v0.1";
const char *argp_program_bug_address = "Namhyung Kim <namhyung@gmail.com>";

#define OPT_flat  301

static struct argp_option ftrace_options[] = {
	{ "library-path", 'L', "PATH", 0, "Load libraries from this PATH" },
	{ "filter", 'F', "FUNC[,FUNC,...]", 0, "Only trace those FUNCs" },
	{ "notrace", 'N', "FUNC[,FUNC,...]", 0, "Don't trace those FUNCs" },
	{ "debug", 'd', 0, 0, "Print debug messages" },
	{ "file", 'f', "FILE", 0, "Use this FILE instead of ftrace.data" },
	{ "flat", OPT_flat, 0, 0, "Use flat output format" },
	{ 0 }
};

#define FTRACE_MODE_INVALID 0
#define FTRACE_MODE_RECORD  1
#define FTRACE_MODE_REPLAY  2
#define FTRACE_MODE_LIVE    3
#define FTRACE_MODE_REPORT  4

#define FTRACE_MODE_DEFAULT  FTRACE_MODE_LIVE

struct opts {
	char *lib_path;
	char *filter;
	char *notrace;
	char *exename;
	char *filename;
	int mode;
	int flat;
	int idx;
};

static bool debug;

static error_t parse_option(int key, char *arg, struct argp_state *state)
{
	struct opts *opts = state->input;

	switch (key) {
	case 'L':
		opts->lib_path = arg;
		break;

	case 'F':
		opts->filter = arg;
		break;

	case 'N':
		opts->notrace = arg;
		break;

	case 'd':
		debug = true;
		break;

	case 'f':
		opts->filename = arg;
		break;

	case OPT_flat:
		opts->flat = 1;
		break;

	case ARGP_KEY_ARG:
		if (state->arg_num == 0) {
			if (!strcmp("record", arg))
				opts->mode = FTRACE_MODE_RECORD;
			else if (!strcmp("replay", arg))
				opts->mode = FTRACE_MODE_REPLAY;
			else if (!strcmp("live", arg))
				opts->mode = FTRACE_MODE_LIVE;
			else
				return ARGP_ERR_UNKNOWN;
			break;
		}
		return ARGP_ERR_UNKNOWN;

	case ARGP_KEY_ARGS:
		if (opts->mode == FTRACE_MODE_INVALID)
			opts->mode = FTRACE_MODE_DEFAULT;

		opts->exename = state->argv[state->next];
		opts->idx = state->next;
		break;

	case ARGP_KEY_NO_ARGS:
	case ARGP_KEY_END:
		if (state->arg_num < 1 || opts->exename == NULL)
			argp_usage(state);
		break;

	default:
		return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static int command_record(int argc, char *argv[], struct opts *opts);
static int command_replay(int argc, char *argv[], struct opts *opts);
static int command_live(int argc, char *argv[], struct opts *opts);
static int command_report(int argc, char *argv[], struct opts *opts);

int main(int argc, char *argv[])
{
	struct opts opts = {
		.mode = FTRACE_MODE_INVALID,
	};
	struct argp argp = {
		.options = ftrace_options,
		.parser = parse_option,
		.args_doc = "[record|replay|report] <command> [args...]",
		.doc = "ftrace -- a function tracer",
	};

	argp_parse(&argp, argc, argv, ARGP_IN_ORDER, NULL, &opts);

	switch (opts.mode) {
	case FTRACE_MODE_RECORD:
		command_record(argc, argv, &opts);
		break;
	case FTRACE_MODE_REPLAY:
		command_replay(argc, argv, &opts);
		break;
	case FTRACE_MODE_LIVE:
		command_live(argc, argv, &opts);
		break;
	case FTRACE_MODE_REPORT:
		command_report(argc, argv, &opts);
		break;
	case FTRACE_MODE_INVALID:
		break;
	}

	return 0;
}

static void build_addrlist(char *buf, char *symlist)
{
	char *p = symlist;
	char *fname = strtok(p, ",:");

	buf[0] = '\0';
	while (fname) {
		struct sym *sym = find_symname(fname);

		if (sym) {
			char tmp[64];

			snprintf(tmp, sizeof(tmp), "%s%#lx",
				 p ? "" : ":", sym->addr);
			strcat(buf, tmp);
		}
		p = NULL;
		fname = strtok(p, ",:");
	}
}

static void setup_child_environ(struct opts *opts)
{
	char buf[4096];
	const char *old_preload = getenv("LD_PRELOAD");
	const char *old_audit = getenv("LD_AUDIT");
	const char *lib_path = opts->lib_path ?: ".";

	snprintf(buf, sizeof(buf), "%s/%s", lib_path, "libmcount.so");
	if (old_preload) {
		strcat(buf, ":");
		strcat(buf, old_preload);
	}
	setenv("LD_PRELOAD", buf, 1);

	snprintf(buf, sizeof(buf), "%s/%s", lib_path, "librtld-audit.so");
	if (old_audit) {
		strcat(buf, ":");
		strcat(buf, old_audit);
	}
	setenv("LD_AUDIT", buf, 1);

	if (opts->filter) {
		build_addrlist(buf, opts->filter);
		setenv("FTRACE_FILTER", buf, 1);
	}

	if (opts->notrace) {
		build_addrlist(buf, opts->notrace);
		setenv("FTRACE_NOTRACE", buf, 1);
	}

	if (strcmp(opts->filename, FTRACE_FILE_NAME))
		setenv("FTRACE_FILE", opts->filename, 1);

	if (debug)
		setenv("FTRACE_DEBUG", "1", 1);
}

static const char mcount_msg[] =
	"ERROR: Can't find '%s' symbol in the '%s'.\n"
	"It seems not to be compiled with -pg flag which generates traceable code.\n"
	"Please check your binary file.\n";

static int command_record(int argc, char *argv[], struct opts *opts)
{
	int pid;
	int status;
	char oldname[512];
	struct sym *sym;

	/* backup old 'ftrace.data' file */
	if (strcmp(FTRACE_FILE_NAME, opts->filename) == 0) {
		snprintf(oldname, sizeof(oldname), "%s.old", opts->filename);

		/* don't care about the failure */
		rename(opts->filename, oldname);
	}

	if (load_symtab(opts->exename) < 0)
		return -1;

	sym = find_symname("mcount");
	if (sym == NULL /* || sym->size != 0 */) {
		printf(mcount_msg, "mcount", opts->exename);
		return -1;
	}

	fflush(stdout);

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}

	if (pid == 0) {
		setup_child_environ(opts);

		/*
		 * I don't think the traced binary is in PATH.
		 * So use plain 'execv' rather than 'execvp'.
		 */
		execv(opts->exename, &argv[opts->idx]);
		abort();
	}

	waitpid(pid, &status, 0);
	if (WIFSIGNALED(status)) {
		printf("child (%s) was terminated by signal: %d\n",
		       opts->exename, WTERMSIG(status));
	}

	if (access(opts->filename, F_OK) < 0) {
		printf("Cannot generate data file\n");
		return -1;
	}
	return 0;
}

static FILE *open_data_file(const char *filename, const char *exename)
{
	FILE *fp;
	struct ftrace_file_header header;

	fp = fopen(filename, "rb");
	if (fp == NULL) {
		if (errno == ENOENT) {
			printf("ERROR: Can't find %s file!\n"
			       "Was '%s' compiled with -pg flag and ran ftrace record?\n",
			       filename, exename);
		} else {
			perror("ftrace");
		}
		goto out;
	}

	fread(&header, sizeof(header), 1, fp);
	if (memcmp(header.magic, FTRACE_MAGIC_STR, FTRACE_MAGIC_LEN)) {
		printf("invalid magic string found!\n");
		fclose(fp);
		fp = NULL;
		goto out;
	}
	if (header.version != FTRACE_VERSION) {
		printf("invalid vergion number found!\n");
		fclose(fp);
		fp = NULL;
	}

out:
	return fp;
}

static int print_flat_rstack(struct mcount_ret_stack *rstack, FILE *fp)
{
	static int count;
	struct sym *parent = find_symtab(rstack->parent_ip);
	struct sym *child = find_symtab(rstack->child_ip);
	const char *parent_name = parent ? parent->name : NULL;
	const char *child_name = child ? child->name : NULL;

	if (parent_name == NULL) {
		Dl_info info;

		dladdr((void *)rstack->parent_ip, &info);
		parent_name = info.dli_sname ?: "unknown";
	}
	if (child_name == NULL) {
		Dl_info info;

		dladdr((void *)rstack->child_ip, &info);
		child_name = info.dli_sname ?: "unknown";
	}

	if (rstack->end_time == 0) {
		printf("[%d] %d/%d: ip (%s -> %s), time (%lu)\n",
		       count++, rstack->tid, rstack->depth, parent_name,
		       child_name, rstack->start_time);
	} else {
		printf("[%d] %d/%d: ip (%s <- %s), time (%lu:%lu)\n",
		       count++, rstack->tid, rstack->depth, parent_name,
		       child_name, rstack->end_time,
		       rstack->end_time - rstack->start_time);
	}
	return 0;
}

static int print_graph_rstack(struct mcount_ret_stack *rstack, FILE *fp)
{
	struct sym *sym = find_symtab(rstack->child_ip);

	if (rstack->end_time == 0) {
		fpos_t pos;
		struct mcount_ret_stack rstack_next;

		fgetpos(fp, &pos);

		if (fread(&rstack_next, sizeof(rstack_next), 1, fp) != 1) {
			perror("error reading rstack");
			return -1;
		}

		if (rstack_next.depth == rstack->depth &&
		    rstack_next.end_time != 0) {
			/* leaf function - also consume return record */
			printf("%4lu usec [%5d] | %*s%s();\n",
			       rstack_next.end_time - rstack->start_time,
			       rstack->tid, rstack->depth * 2, "",
			       sym ? sym->name : "unknown");
		} else {
			/* function entry */
			printf("%9s [%5d] | %*s%s() {\n", "",
			       rstack->tid, rstack->depth * 2, "",
			       sym ? sym->name : "unknown");

			/* need to re-process return record */
			fsetpos(fp, &pos);
		}
	} else {
		/* function exit */
		printf("%4lu usec [%5d] | %*s} /* %s */\n",
		       rstack->end_time - rstack->start_time,
		       rstack->tid, rstack->depth * 2, "",
		       sym ? sym->name : "unknown");
	}
	return 0;
}

static int command_replay(int argc, char *argv[], struct opts *opts)
{
	FILE *fp;
	int ret;
	struct mcount_ret_stack rstack;

	fp = open_data_file(opts->filename, opts->exename);
	if (fp == NULL)
		return -1;

	ret = load_symtab(opts->exename);
	if (ret < 0)
		goto out;

	while (fread(&rstack, sizeof(rstack), 1, fp) == 1) {
		if (opts->flat)
			ret = print_flat_rstack(&rstack, fp);
		else
			ret = print_graph_rstack(&rstack, fp);

		if (ret)
			break;
	}

	unload_symtab();
out:
	fclose(fp);

	return ret;
}

static int command_live(int argc, char *argv[], struct opts *opts)
{
	char template[32] = "/tmp/ftrace-live-XXXXXX";
	int fd = mkstemp(template);
	if (fd < 0) {
		perror("live command cannot be run");
		return -1;
	}
	close(fd);

	opts->filename = template;

	if (command_record(argc, argv, opts) == 0)
		command_replay(argc, argv, opts);

	unlink(opts->filename);

	return 0;
}

static int command_report(int argc, char *argv[], struct opts *opts)
{
	return 0;
}
