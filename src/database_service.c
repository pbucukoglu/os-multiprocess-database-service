#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/uio.h>   // writev: write multiple buffers to the pipe in a single call

/*------------------------------------------------------------
 * Process pipeline:
 *   1) Manager   : coordinates all stages
 *   2) Extractor : scans lines via mmap, writes matching lines to a pipe
 *   3) Sorter    : reads from pipe, filters empty lines, numeric sort by 5th column
 *   4) Reporter  : prints the number of records in the output file (wc -l)
 *
 * Core syscalls/tools used:
 *   mmap, fork, pipe, execvp, writev, dup2
 *-----------------------------------------------------------*/

typedef struct {
    char *input_file;
    char *output_file;
    int   num_workers;
    char *keyword;
} program_args_t;

/* Prototypes */
static void  parse_arguments(int argc, char *argv[], program_args_t *args);
static void *map_input_file(const char *filename, size_t *file_size);
static void  manager_process(program_args_t *args, void *shared, size_t fsize);
static void  extractor_process(void *shared, size_t fsize, int wid, int nworkers,
                               const char *keyword, int pipe_write_fd);
static void  sorter_process(const char *output_file, int pipe_read_fd);
static void  reporter_process(const char *output_file);
static void  unmap_file(void *addr, size_t len);

/* Check if buffer contains keyword (case-insensitive using strncasecmp). */
static int contains_keyword(const char *buf, size_t len, const char *kw) {
    size_t kwlen = strlen(kw);
    if (kwlen == 0 || len < kwlen) return 0;
    for (size_t i = 0; i + kwlen <= len; ++i)
        if (strncasecmp(buf + i, kw, kwlen) == 0)
            return 1;
    return 0;
}

/*------------------------------------------------------------
 * main:
 *  Parses arguments, mmaps the input file, runs the manager.
 *-----------------------------------------------------------*/
int main(int argc, char *argv[]) {
    program_args_t args;
    parse_arguments(argc, argv, &args);

    size_t fsize = 0;
    void *shared = map_input_file(args.input_file, &fsize);
    if (shared == MAP_FAILED) { perror("mmap"); exit(EXIT_FAILURE); }

    manager_process(&args, shared, fsize);
    unmap_file(shared, fsize);
    return EXIT_SUCCESS;
}

/*------------------------------------------------------------
 * parse_arguments:
 *  Validate CLI arguments and fill args struct.
 *-----------------------------------------------------------*/
static void parse_arguments(int argc, char *argv[], program_args_t *args) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <input_file> <output_file> <num_of_workers> <keyword>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    args->input_file  = argv[1];
    args->output_file = argv[2];
    args->num_workers = atoi(argv[3]);
    args->keyword     = argv[4];
    if (args->num_workers <= 0) {
        fprintf(stderr, "num_of_workers must be positive\n");
        exit(EXIT_FAILURE);
    }
}

/*------------------------------------------------------------
 * map_input_file:
 *  Open file in O_RDONLY, get its size, mmap it with MAP_SHARED|PROT_READ.
 *  All child processes share the same read-only region.
 *-----------------------------------------------------------*/
static void *map_input_file(const char *filename, size_t *file_size) {
    int fd = open(filename, O_RDONLY);
    if (fd == -1) { perror("open input"); exit(EXIT_FAILURE); }

    struct stat st;
    if (fstat(fd, &st) == -1) { perror("fstat"); close(fd); exit(EXIT_FAILURE); }
    if (st.st_size == 0) {
        fprintf(stderr, "Input file is empty\n");
        close(fd);
        exit(EXIT_FAILURE);
    }

    *file_size = (size_t)st.st_size;
    void *addr = mmap(NULL, *file_size, PROT_READ, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) { perror("mmap"); close(fd); exit(EXIT_FAILURE); }
    close(fd);
    return addr;
}

/*------------------------------------------------------------
 * manager_process:
 *  Creates the pipe, forks extractors, runs sorter, waits for
 *  children and finally launches reporter.
 *-----------------------------------------------------------*/
static void manager_process(program_args_t *args, void *shared, size_t fsize) {
    int pfd[2];
    if (pipe(pfd) == -1) { perror("pipe"); exit(EXIT_FAILURE); }

    /* Stage 2: Extractors */
    for (int i = 0; i < args->num_workers; ++i) {
        pid_t pid = fork();
        if (pid == -1) { perror("fork extractor"); exit(EXIT_FAILURE); }
        if (pid == 0) {
            close(pfd[0]);             // close read end in extractor
            extractor_process(shared, fsize, i, args->num_workers, args->keyword, pfd[1]);
            _exit(EXIT_SUCCESS);
        }
    }

    /* Manager: close write end to signal EOF downstream */
    close(pfd[1]);

    /* Stage 3: Sorter */
    pid_t sorter_pid = fork();
    if (sorter_pid == -1) { perror("fork sorter"); exit(EXIT_FAILURE); }
    if (sorter_pid == 0) {
        sorter_process(args->output_file, pfd[0]);  // sorter reads from pipe (stdin)
        _exit(EXIT_SUCCESS);
    }
    close(pfd[0]);

    /* Wait for all extractors and sorter to finish */
    int status;
    for (int i = 0; i < args->num_workers; ++i) (void)wait(&status);
    (void)waitpid(sorter_pid, &status, 0);

    /* Stage 4: Reporter */
    pid_t reporter_pid = fork();
    if (reporter_pid == -1) { perror("fork reporter"); exit(EXIT_FAILURE); }
    if (reporter_pid == 0) {
        reporter_process(args->output_file);
        _exit(EXIT_SUCCESS);
    }
    (void)waitpid(reporter_pid, &status, 0);
}

/*------------------------------------------------------------
 * extractor_process:
 *  Each worker scans the assigned byte range and processes lines.
 *  If contains_keyword() is true, the line (plus '\n') is written to the pipe
 *  using writev(). Start offset is aligned to the next line boundary;
 *  the last worker consumes until EOF.
 *-----------------------------------------------------------*/
static void extractor_process(void *shared, size_t fsize, int wid, int nworkers,
                              const char *keyword, int pipe_write_fd) {
    const char *p = (const char *)shared;

    /* Compute this worker's chunk. */
    size_t chunk = fsize / (size_t)nworkers;
    size_t beg   = (size_t)wid * chunk;
    size_t end   = (wid == nworkers - 1) ? fsize : (size_t)(wid + 1) * chunk;

    /* Align beginning to the start of the next line (skip partial line). */
    if (beg > 0) while (beg < fsize && p[beg - 1] != '\n') beg++;
    while (beg < fsize && (p[beg] == '\n' || p[beg] == '\r')) beg++;

    /* Scan line by line. */
    size_t i = beg;
    while (i < end && i < fsize) {
        size_t line_start = i;
        while (i < fsize && p[i] != '\n') i++;
        size_t line_end = i;
        if (i < fsize) i++;  // skip '\n'

        /* Trim trailing CR/space/tab. */
        while (line_end > line_start &&
               (p[line_end - 1] == '\r' || p[line_end - 1] == ' ' || p[line_end - 1] == '\t'))
            line_end--;

        size_t linelen = line_end - line_start;

        /* If line matches, write it (plus '\n') to the pipe. */
        if (linelen > 0 && contains_keyword(p + line_start, linelen, keyword)) {
            char nl = '\n';
            struct iovec iov[2];
            iov[0].iov_base = (void *)(p + line_start);
            iov[0].iov_len  = linelen;
            iov[1].iov_base = &nl;
            iov[1].iov_len  = 1;
            if (writev(pipe_write_fd, iov, 2) == -1) {
                perror("writev pipe");
                _exit(EXIT_FAILURE);
            }
        }

        if (i >= fsize) break;
    }

    close(pipe_write_fd);
}

/*------------------------------------------------------------
 * sorter_process:
 *  Redirects stdin to the pipe and stdout to the output file.
 *  Runs "grep -v '^[[:space:]]*$' | sort -k5,5n" in a shell:
 *    - filters out empty/whitespace-only lines
 *    - numeric sort by 5th column (grade)
 *-----------------------------------------------------------*/
static void sorter_process(const char *output_file, int pipe_read_fd) {
    int outfd = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (outfd == -1) { perror("open output"); _exit(EXIT_FAILURE); }

    if (dup2(pipe_read_fd, STDIN_FILENO) == -1)  { perror("dup2 stdin");  _exit(EXIT_FAILURE); }
    if (dup2(outfd,       STDOUT_FILENO) == -1)  { perror("dup2 stdout"); _exit(EXIT_FAILURE); }
    close(pipe_read_fd);
    close(outfd);

    /* grep -v ... | sort -k5,5n pipeline */
    setenv("LC_ALL","C",1);
    char *args[] = {"sh","-c","grep -v '^[[:space:]]*$' | sort -k5,5n", NULL};
    execvp("sh", args);
    perror("exec sort");
    _exit(EXIT_FAILURE);
}

/*------------------------------------------------------------
 * reporter_process:
 *  Uses wc -l on output_file and prints only the line count.
 *-----------------------------------------------------------*/
static void reporter_process(const char *output_file) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "wc -l %s | awk '{print $1}'", output_file);
    char *args[] = {"sh", "-c", cmd, NULL};
    execvp("sh", args);
    perror("exec wc");
    _exit(EXIT_FAILURE);
}

/*------------------------------------------------------------
 * unmap_file:
 *  Unmaps the previously mmapped region.
 *-----------------------------------------------------------*/
static void unmap_file(void *addr, size_t len) {
    if (munmap(addr, len) == -1) { perror("munmap"); exit(EXIT_FAILURE); }
}
