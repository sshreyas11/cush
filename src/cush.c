/*
 * cush - the customizable shell.
 *
 * Developed by Godmar Back for CS 3214 Summer 2020
 * Virginia Tech.  Augmented to use posix_spawn in Fall 2021.
 */
#define _GNU_SOURCE 1
#include <stdio.h>
#include <readline/readline.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <sys/wait.h>
#include <assert.h>
#include "spawn.h"
#include <fcntl.h>
/* Since the handed out code contains a number of unused functions. */
#pragma GCC diagnostic ignored "-Wunused-function"

#include "termstate_management.h"
#include "signal_support.h"
#include "shell-ast.h"
#include "utils.h"
int num_hist;
#define MAX_HISTORY 100
#define MAX_CMD 256
char history_of_commands[MAX_HISTORY][MAX_CMD];
static void add_history(const char *cmd);
static struct job *get_job(pid_t pid);
static int most_recent_job();

static void handle_child_status(pid_t pid, int status);

static void
usage(char *progname)
{
    printf("Usage: %s -h\n"
           " -h            print this help\n",
           progname);

    exit(EXIT_SUCCESS);
}

/* Build a prompt */
static char *
build_prompt(void)
{
    return strdup("cush> ");
}

enum job_status
{
    FOREGROUND,    /* job is running in foreground.  Only one job can be
                      in the foreground state. */
    BACKGROUND,    /* job is running in background */
    STOPPED,       /* job is stopped via SIGSTOP */
    NEEDSTERMINAL, /* job is stopped because it was a background job
                      and requires exclusive terminal access */
};

struct pid2
{
    pid_t p;
    struct list_elem pid_elem;
};

struct job
{
    struct list_elem elem;          /* Link element for jobs list. */
    struct ast_pipeline *pipe;      /* The pipeline of commands this job represents */
    int jid;                        /* Job id. */
    enum job_status status;         /* Job status. */
    int num_processes_alive;        /* The number of processes that we know to be alive */
    struct termios saved_tty_state; /* The state of the terminal when this job was
                                       stopped after having been in foreground */

    /* Add additional fields here if needed. */

    pid_t pid; /* process id for each command */
    // list or arr
    struct list pids;
    bool saved; /* return true if job goes into the bg and saves terminal state */
    pid_t pgid; /* process group id -> same as the first pid in the command line */
};
// array of pids...

/* Utility functions for job list management.
 * We use 2 data structures:
 * (a) an array jid2job to quickly find a job based on its id
 * (b) a linked list to support iteration
 */
#define MAXJOBS (1 << 16)
static struct list job_list;

static struct job *jid2job[MAXJOBS];

/* Return job corresponding to jid */
static struct job *
get_job_from_jid(int jid)
{
    if (jid > 0 && jid < MAXJOBS && jid2job[jid] != NULL)
        return jid2job[jid];

    return NULL;
}

/* Add a new job to the job list */
static struct job *
add_job(struct ast_pipeline *pipe)
{
    struct job *job = malloc(sizeof *job);
    job->pipe = pipe;
    job->num_processes_alive = 0;
    list_push_back(&job_list, &job->elem);
    for (int i = 1; i < MAXJOBS; i++)
    {
        if (jid2job[i] == NULL)
        {
            jid2job[i] = job;
            job->jid = i;
            list_init(&job->pids);
            return job;
        }
    }

    fprintf(stderr, "Maximum number of jobs exceeded\n");
    abort();
    return NULL;
}

/* Delete a job.
 * This should be called only when all processes that were
 * forked for this job are known to have terminated.
 */
static void
delete_job(struct job *job)
{
    int jid = job->jid;
    assert(jid != -1);
    jid2job[jid]->jid = -1;
    jid2job[jid] = NULL;
    ast_pipeline_free(job->pipe);
    free(job);
}

static const char *
get_status(enum job_status status)
{
    switch (status)
    {
    case FOREGROUND:
        return "Foreground";
    case BACKGROUND:
        return "Running";
    case STOPPED:
        return "Stopped";
    case NEEDSTERMINAL:
        return "Stopped (tty)";
    default:
        return "Unknown";
    }
}

/* Print the command line that belongs to one job. */
static void
print_cmdline(struct ast_pipeline *pipeline)
{
    struct list_elem *e = list_begin(&pipeline->commands);
    for (; e != list_end(&pipeline->commands); e = list_next(e))
    {
        struct ast_command *cmd = list_entry(e, struct ast_command, elem);
        if (e != list_begin(&pipeline->commands))
            printf("| ");
        char **p = cmd->argv;
        printf("%s", *p++);
        while (*p)
            printf(" %s", *p++);
    }
}

/* Print a job */
static void
print_job(struct job *job)
{
    printf("[%d]\t%s\t\t(", job->jid, get_status(job->status));
    print_cmdline(job->pipe);
    printf(")\n");
}

/*
 * Suggested SIGCHLD handler.
 *
 * Call waitpid() to learn about any child processes that
 * have exited or changed status (been stopped, needed the
 * terminal, etc.)
 * Just record the information by updating the job list
 * data structures.  Since the call may be spurious (e.g.
 * an already pending SIGCHLD is delivered even though
 * a foreground process was already reaped), ignore when
 * waitpid returns -1.
 * Use a loop with WNOHANG since only a single SIGCHLD
 * signal may be delivered for multiple children that have
 * exited. All of them need to be reaped.
 */
static void
sigchld_handler(int sig, siginfo_t *info, void *_ctxt)
{
    pid_t child;
    int status;

    assert(sig == SIGCHLD);

    while ((child = waitpid(-1, &status, WUNTRACED | WNOHANG)) > 0)
    {
        handle_child_status(child, status);
    }
}

/* Wait for all processes in this job to complete, or for
 * the job no longer to be in the foreground.
 * You should call this function from a) where you wait for
 * jobs started without the &; and b) where you implement the
 * 'fg' command.
 *
 * Implement handle_child_status such that it records the
 * information obtained from waitpid() for pid 'child.'
 *
 * If a process exited, it must find the job to which it
 * belongs and decrement num_processes_alive.
 *
 * However, note that it is not safe to call delete_job
 * in handle_child_status because wait_for_job assumes that
 * even jobs with no more num_processes_alive haven't been
 * deallocated.  You should postpone deleting completed
 * jobs from the job list until when your code will no
 * longer touch them.
 *
 * The code below relies on `job->status` having been set to FOREGROUND
 * and `job->num_processes_alive` having been set to the number of
 * processes successfully forked for this job.
 */
static void
wait_for_job(struct job *job)
{
    assert(signal_is_blocked(SIGCHLD));
    // termstate_give_terminal_back_to_shell();
    while (job->status == FOREGROUND && job->num_processes_alive > 0)
    {
        // printf("Job num of processes %d Job pid %d\n", job->num_processes_alive, job->pgid);
        int status;

        pid_t child = waitpid(-1, &status, WUNTRACED);

        // When called here, any error returned by waitpid indicates a logic
        // bug in the shell.
        // In particular, ECHILD "No child process" means that there has
        // already been a successful waitpid() call that reaped the child, so
        // there's likely a bug in handle_child_status where it failed to update
        // the "job" status and/or num_processes_alive fields in the required
        // fashion.
        // Since SIGCHLD is blocked, there cannot be races where a child's exit
        // was handled via the SIGCHLD signal handler.
        if (child != -1)
            handle_child_status(child, status);
        else
            utils_fatal_error("waitpid failed, see code for explanation");
    }
}

static void
handle_child_status(pid_t pid, int status)
{

    /**
     */
    assert(signal_is_blocked(SIGCHLD));

    /* To be implemented.
     * Step 1. Given the pid, determine which job this pid is a part of
     *         (how to do this is not part of the provided code.)
     * Step 2. Determine what status change occurred using the
     *         WIF*() macros.
     * Step 3. Update the job status accordingly, and adjust
     *         num_processes_alive if appropriate.
     *         If a process was stopped, save the terminal state.
     */

    struct job *j = get_job(pid);
    if (j != NULL)
    {
        if (WIFSIGNALED(status))
        {
            fprintf(stderr, "\n%s\n", strsignal(WTERMSIG(status)));
            j->num_processes_alive--;
        }
        else if (WIFEXITED(status))
        {
            j->num_processes_alive--;
            if (j->status == FOREGROUND)
            {
                termstate_sample();
            }
        }
        else if (WIFSTOPPED(status))
        {

            if (WSTOPSIG(status) == SIGTSTP)
            {
                termstate_save(&j->saved_tty_state);
                j->status = STOPPED;
                print_job(j);
                termstate_give_terminal_back_to_shell();
            }
            if (WSTOPSIG(status) == SIGSTOP)
            {
                termstate_save(&j->saved_tty_state);
                j->status = STOPPED;
            }
            if (WSTOPSIG(status) == SIGTTOU || WSTOPSIG(status) == SIGTTIN)
            {
                j->status = NEEDSTERMINAL;
            }
            else if (WTERMSIG(status) == SIGTERM)
            {
                j->num_processes_alive--;
            }
        }
        if (j->num_processes_alive == 0)
        {
            list_remove(&j->elem);
            delete_job(j);
        }
    }
}

/**
 * start by iterating through the job list

struct list_elem *e;
for (e = list_begin(&job_list); e != list_end(&job_list); e = list_next(e)) {
// struct ast_pipeline and create a temp job
struct ast_pipeline *temp = list_entry(e, struct ast_pipeline, elem);
if ()
}*/
// struct job * curr = get_job(pid);

//    assert(curr != NULL);

/*
 * dont call delete job here
 */

/**
 * iterate through joblist and get job nusing list entry
 * sizeof pid list and match pid to the job
 */

int main(int ac, char *av[])
{
    int opt;

    /* Process command-line arguments. See getopt(3) */
    while ((opt = getopt(ac, av, "h")) > 0)
    {
        switch (opt)
        {
        case 'h':
            usage(av[0]);
            break;
        }
    }

    list_init(&job_list);
    signal_set_handler(SIGCHLD, sigchld_handler);
    termstate_init();

    /* Read/eval loop. */
    for (;;)
    {

        /* If you fail this assertion, you were about to enter readline()
         * while SIGCHLD is blocked.  This means that your shell would be
         * unable to receive SIGCHLD signals, and thus would be unable to
         * wait for background jobs that may finish while the
         * shell is sitting at the prompt waiting for user input.
         */
        assert(!signal_is_blocked(SIGCHLD));

        /* If you fail this assertion, you were about to call readline()
         * without having terminal ownership.
         * This would lead to the suspension of your shell with SIGTTOU.
         * Make sure that you call termstate_give_terminal_back_to_shell()
         * before returning here on all paths.
         */
        termstate_give_terminal_back_to_shell();
        assert(termstate_get_current_terminal_owner() == getpgrp());

        /* Do not output a prompt unless shell's stdin is a terminal */
        char *prompt = isatty(0) ? build_prompt() : NULL;
        char *cmdline = readline(prompt);
        // delete job do anywhere between here and where we spawn the processes (after ast_commandlineprint(cline))
        free(prompt);

        if (cmdline == NULL) /* User typed EOF */
            break;

        struct ast_command_line *cline = ast_parse_command_line(cmdline);
        free(cmdline);
        if (cline == NULL) /* Error in command line */
            continue;

        if (list_empty(&cline->pipes))
        { /* User hit enter */
            ast_command_line_free(cline);
            continue;
        }

        // ast_command_line_print(cline);      /* Output a representation of
        // the entered command line */

        struct list_elem *j = list_begin(&job_list);
        while (j != list_end(&job_list))
        {
            struct job *job = list_entry(list_begin(&job_list), struct job, elem);
            if (job->num_processes_alive == 0)
            {
                j = list_remove(j);
                delete_job(job);
            }
            else
            {
                j = list_next(j);
            }
        }

        struct list_elem *e;
        signal_block(SIGCHLD);
        for (e = list_begin(&cline->pipes); e != list_end(&cline->pipes); e = list_next(e))
        {
            struct ast_pipeline *pipeline = list_entry(e, struct ast_pipeline, elem);
            // save the size--> make a 2d array of pipe--> set a counter-->loop thru the size--> pipe2(pipearr[i],CLOEXEC)
            int size = list_size(&pipeline->commands);
            int count_pipes = 0;
            int pipe[size][2];
            for (int i = 0; i < size; i++)
            {
                pipe2(pipe[i], O_CLOEXEC);
            }
            struct job *job_to_add = add_job(pipeline);
            int count = 0;
            // struct job *curr_job = add_job(pipeline);
            for (struct list_elem *elem = list_begin(&pipeline->commands); elem != list_end(&pipeline->commands); elem = list_next(elem))
            {

                struct ast_command *commands = list_entry(elem, struct ast_command, elem);
                // int command_size = list_size(&pipeline->commands);
                //  create variable
                char *command = commands->argv[0];
                add_history(command);
                // struct ast_command *last = list_entry(list_tail(&pipeline->commands),struct ast_commnand, elem);
                pid_t pid = 0; // initial pid
                /*compare command to each
                 * of the six built-in commands
                 * and if the command is one of
                 * them, then perform the action
                 * as it relates to that command
                 */

                // check to see of your command is a built in or non builtin command

                if (strcmp(command, "exit") == 0)
                {
                    exit(0);
                }
                else if (strcmp(command, "jobs") == 0)
                {
                    /*
                     * when the jobs command is found:
                     * we must print all jobs that are running
                     * and jobs that are stopped
                     */
                    for (struct list_elem *e = list_begin(&job_list); e != list_end(&job_list); e = list_next(e))
                    {
                        // struct job_list * list_jobs = list_entry(e, struct job, elem);
                        struct job *curr_job = list_entry(e, struct job, elem);
                        if (curr_job != NULL && curr_job->status != FOREGROUND) {
                                print_job(curr_job);
                        }
                        
                    }
                }
                else if (strcmp(command, "stop") == 0)
                {
                    int n = atoi(commands->argv[1]);
                    struct job *j = get_job_from_jid(n);
                    if (j != NULL)
                    {
                        killpg(list_entry(list_begin(&j->pids), struct pid2, pid_elem)->p, SIGSTOP);
                    }
                    else
                    {
                        printf("Job %d not found\n", n);
                    }
                }
                /*
                 * first additional built-in: cd
                 * if command is cd,
                 * check for value of second argument
                 */
                else if (strcmp(command, "cd") == 0)
                {
                    char *arg2 = commands->argv[1];
                    if (arg2 == NULL)
                    {
                        chdir(getenv("HOME"));
                    }
                    else
                    {
                        chdir(arg2);
                    }
                }
                /*
                 * second additional built-in: history
                 * if command is history
                 * print all previous commands
                 */
                else if (strcmp(command, "history") == 0)
                {
                    for (int i = 0; i < num_hist; i++)
                    {
                        printf("%d %s\n", i + 1, history_of_commands[i]);
                    }
                }
                else if (strcmp(command, "stop") == 0)
                {
                    int n = atoi(commands->argv[1]);
                    struct job *j = get_job_from_jid(n);
                    termstate_save(&j->saved_tty_state);
                    if (j != NULL)
                    {
                        killpg(list_entry(list_begin(&j->pids), struct pid2, pid_elem)->p, SIGSTOP);
                    }
                    else
                    {
                        printf("Job %d not found\n", n);
                    }
                }

                else if (strcmp(command, "bg") == 0)
                {
                    int n = atoi(commands->argv[1]);
                    struct job *j = get_job_from_jid(n);
                    j->status = BACKGROUND;
                    int p = j->pid;
                    printf("[%d] %d", j->jid, p);
                    printf("\n");
                    // termstate_give_terminal_to(&j->saved_tty_state, list_entry(list_begin(&j->pids), struct pid2, pid_elem)->p);
                    killpg(list_entry(list_begin(&j->pids), struct pid2, pid_elem)->p, SIGCONT);
                    termstate_save(&j->saved_tty_state);
                }
                else if (strcmp(command, "fg") == 0)
                {
                    int n = atoi(commands->argv[1]);
                    struct job *j = get_job_from_jid(n);
                    j->status = FOREGROUND;
                    print_cmdline(j->pipe);
                    printf("\n");
                    pid_t pgid = list_entry(list_begin(&j->pids), struct pid2, pid_elem)->p;
                    termstate_give_terminal_to(&j->saved_tty_state, pgid);
                    killpg(pgid, SIGCONT);
                    wait_for_job(j);
                    termstate_give_terminal_back_to_shell();
                }
                else if (strcmp(command, "kill") == 0)
                {
                    int n = atoi(commands->argv[1]);
                    struct job *j = get_job_from_jid(n);
                    if (j != NULL)
                    {
                        killpg(list_entry(list_begin(&j->pids), struct pid2, pid_elem)->p, SIGTERM);
                    }
                    else
                    {
                        printf("Job %d not found\n", n);
                    }
                }

                // non built ins
                else
                {

                    posix_spawn_file_actions_t child_file_attr;
                    posix_spawnattr_t child_spawn_attr;

                    posix_spawnattr_init(&child_spawn_attr);
                    posix_spawn_file_actions_init(&child_file_attr);

                    // Input and output handeling
                    if (pipeline->iored_input != NULL && count == 0)
                    {

                        posix_spawn_file_actions_addopen(&child_file_attr, 0, pipeline->iored_input, O_RDONLY, 0);
                    }
                    if (pipeline->iored_output != NULL && count == size - 1)
                    {
                        if (pipeline->append_to_output)
                        {
                            posix_spawn_file_actions_addopen(&child_file_attr, 1, pipeline->iored_output, O_RDWR | O_CREAT | O_APPEND, 0666);
                        }
                        else
                        {
                            posix_spawn_file_actions_addopen(&child_file_attr, 1, pipeline->iored_output, O_RDWR | O_CREAT | O_TRUNC, 0666);
                        }
                    }
                    // if (commands->dup_stderr_to_stdout)
                    // {
                    //     posix_spawn_file_actions_adddup2(&child_file_attr, 1, 2);
                    // }
                    if (size > 1)
                    {
                        if (count_pipes == 0)
                        {

                            posix_spawn_file_actions_adddup2(&child_file_attr, pipe[0][1], 1);
                        }
                        else if (count_pipes == size - 1)
                        {

                            posix_spawn_file_actions_adddup2(&child_file_attr, pipe[size - 2][0], 0);
                        }
                        else
                        {
                            posix_spawn_file_actions_adddup2(&child_file_attr, pipe[count_pipes][1], 1);
                            posix_spawn_file_actions_adddup2(&child_file_attr, pipe[count_pipes - 1][0], 0);
                        }
                        count_pipes++;
                    }
                    if (commands->dup_stderr_to_stdout)
                    {
                        posix_spawn_file_actions_adddup2(&child_file_attr, 1, 2);
                    }
                    

                    if (count == 0)
                    {
                        // printf("COUNT %d\n", job_to_add->pgid);
                        posix_spawnattr_setpgroup(&child_spawn_attr, 0);
                    }
                    else if (count > 0)
                    {
                        //printf("Pgid %d\n", job_to_add->pgid);
                        posix_spawnattr_setpgroup(&child_spawn_attr, job_to_add->pgid);
                    }

                    if (!pipeline->bg_job)
                    {
                        job_to_add->status = FOREGROUND;
                        posix_spawnattr_setflags(&child_spawn_attr, POSIX_SPAWN_TCSETPGROUP | POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_USEVFORK);
                        posix_spawnattr_tcsetpgrp_np(&child_spawn_attr, termstate_get_tty_fd());
                    }
                    else
                    {
                        job_to_add->status = BACKGROUND;
                        posix_spawnattr_setflags(&child_spawn_attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_USEVFORK);
                    }

                    extern char **environ;

                    int spawn_error = posix_spawnp(&pid, commands->argv[0], &child_file_attr, &child_spawn_attr, commands->argv, environ);
                    count++;
                    //printf("count is %d", count);
                    if (spawn_error != 0)
                    {
                        perror(commands->argv[0]);
                        break;
                    }
                    struct pid2 *p = malloc(sizeof(struct pid2));
                    p->p = pid;
                    list_push_back(&job_to_add->pids, &p->pid_elem);
                    job_to_add->pgid = list_entry(list_begin(&job_to_add->pids), struct pid2, pid_elem)->p;
                    job_to_add->pid = pid;
                    if (job_to_add->status == BACKGROUND)
                    {
                        printf("[%d] %d", job_to_add->jid, list_entry(list_begin(&job_to_add->pids), struct pid2, pid_elem)->p);
                        printf("\n");
                        termstate_save(&job_to_add->saved_tty_state);
                    }

                    job_to_add->num_processes_alive++;
                }
            }
            for (int i = 0; i < size; i++)
            {
                close(pipe[i][0]);
                close(pipe[i][1]);
            }
            wait_for_job(job_to_add);

            termstate_give_terminal_back_to_shell();

            // signal_block(SIGCHLD);
            // waitpid();
        }
        signal_unblock(SIGCHLD);

        /* Free the command line.
         * This will free the ast_pipeline objects still contained
         * in the ast_command_line.  Once you implement a job list
         * that may take ownership of ast_pipeline objects that are
         * associated with jobs you will need to reconsider how you
         * manage the lifetime of the associated ast_pipelines.
         * Otherwise, freeing here will cause use-after-free errors.
         */
        free(cline);
    }
    return 0;
}

static int most_recent_job()
{
    if (!list_empty(&job_list))
    {
        struct list_elem *e = list_back(&job_list);
        struct job *curr_job = list_entry(e, struct job, elem);
        return curr_job->jid;
    }
    return 0;
}

static struct job *get_job(pid_t pid)
{
    for (struct list_elem *e = list_begin(&job_list); e != list_end(&job_list); e = list_next(e))
    {
        struct job *curr_job = list_entry(e, struct job, elem);
        /*
         * another for loop to loop through the commands in each pipe
         */
        for (struct list_elem *f = list_begin(&curr_job->pids); f != list_end(&curr_job->pids); f = list_next(f))
        {
            struct pid2 *curr_pid = list_entry(f, struct pid2, pid_elem);

            // printf("Curr job %d with specified pid %d\n", curr_pid->p, pid);
            /*
             * compare job pid to parameter pid
             */
            // struct job * curr_job = list_entry(f, struct job, elem);
            if (curr_pid->p == pid)
            {
                return curr_job;
            }
        }
    }
    return NULL;
}
static void add_history(const char *cmd)
{
    if (num_hist < MAX_HISTORY)
    {
        strcpy(history_of_commands[num_hist], cmd);
        num_hist++;
    }
    else
    {
        for (int i = 0; i < MAX_HISTORY - 1; i++)
        {
            strcpy(history_of_commands[i], history_of_commands[i + 1]);
        }
        strcpy(history_of_commands[MAX_HISTORY - 1], cmd);
    }
}
