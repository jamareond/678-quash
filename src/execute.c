/**
 * @file execute.c
 *
 * @brief Implements interface functions between Quash and the environment and
 * functions that interpret an execute commands.
 *
 * @note As you add things to this file you may want to change the method signature
 */

#include "execute.h"

#include <stdio.h>

#include "quash.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>

// Remove this and all expansion calls to it
/**
 * @brief Note calls to any function that requires implementation
 */
#define IMPLEMENT_ME()                                                  \
  fprintf(stderr, "IMPLEMENT ME: %s(line %d): %s()\n", __FILE__, __LINE__, __FUNCTION__)

/***************************************************************************
 * Minimal jobs support (enough for background, jobs, kill, completion prints)
 ***************************************************************************/

#define MAX_JOB_PROCS 64
#define MAX_JOBS 128
#define MAX_CMD_STR 256

typedef struct Job {
  int job_id;
  pid_t pids[MAX_JOB_PROCS];
  int num_pids;
  pid_t first_pid;
  char cmd[MAX_CMD_STR];
} Job;

static Job g_jobs[MAX_JOBS];
static int g_num_jobs = 0;
static int g_next_job_id = 1;

// Track pids in the current script/job while run_script() runs
static pid_t g_cur_pids[MAX_JOB_PROCS];
static int g_cur_npids = 0;

// Pipe state across create_process calls in one script
static int g_prev_pipe_read = -1;

static void remove_job_at(int idx) {
  if (idx < 0 || idx >= g_num_jobs) return;
  for (int i = idx; i < g_num_jobs - 1; ++i)
    g_jobs[i] = g_jobs[i + 1];
  g_num_jobs--;
}

static void remember_pid(pid_t pid) {
  if (g_cur_npids < MAX_JOB_PROCS) {
    g_cur_pids[g_cur_npids++] = pid;
  }
}

static void build_cmd_string(CommandHolder* holders, char* out, size_t n) {
  if (out == NULL || n == 0) return;
  out[0] = '\0';

  // Keep it simple: use the first command name, and add " | ..." if piped
  CommandType t = get_command_holder_type(holders[0]);
  const char* name = "cmd";

  if (t == GENERIC && holders[0].cmd.generic.args && holders[0].cmd.generic.args[0])
    name = holders[0].cmd.generic.args[0];
  else if (t == ECHO)   name = "echo";
  else if (t == EXPORT) name = "export";
  else if (t == CD)     name = "cd";
  else if (t == KILL)   name = "kill";
  else if (t == PWD)    name = "pwd";
  else if (t == JOBS)   name = "jobs";
  else if (t == EXIT)   name = "exit";

  snprintf(out, n, "%s", name);

  // If there is any pipe flag in the script, show it briefly
  for (int i = 0; get_command_holder_type(holders[i]) != EOC; ++i) {
    if (holders[i].flags & PIPE_OUT) {
      strncat(out, " | ...", n - strlen(out) - 1);
      break;
    }
  }
}

/***************************************************************************
 * Interface Functions
 ***************************************************************************/

// Return a string containing the current working directory
char* get_current_directory(bool* should_free) {
  // getcwd(NULL, 0) moves a buffer large enough for the path
  char* cwd = getcwd(NULL, 0);
  if (cwd == NULL) {
    // If something goes wrong, fall back to a static
    *should_free = false;
    return ".";
  }

  *should_free = true;
  return cwd;
}

// Returns the value of an environment variable env_var
const char* lookup_env(const char* env_var) {
  if (env_var == NULL) return NULL;

  const char* val = getenv(env_var);

  // The parser wants NULL when a variable doesn't exist, but the prompt prints
  // USER/HOSTNAME directly. safe fallbacks for those so the prompt
  // doesn't print a NULL pointer
  if (val == NULL) {
    if (strcmp(env_var, "USER") == 0) return "unknown";

    if (strcmp(env_var, "HOSTNAME") == 0) {
      static char host[256];
      if (gethostname(host, sizeof(host)) == 0) {
        host[sizeof(host) - 1] = '\0';
        return host;
      }
      return "unknown-host";
    }

    return NULL;
  }

  return val;
}

// Set environment variables (declared in execute.h)
void write_env(const char* env_var, const char* val) {
  if (env_var == NULL || env_var[0] == '\0') return;
  if (val == NULL) val = "";

  if (setenv(env_var, val, 1) != 0) {
    perror("ERROR: setenv failed");
  }
}

// Check the status of background jobs
void check_jobs_bg_status() {
  // Check on the statuses of all processes belonging to all background
  // jobs. This function should remove jobs from the jobs queue once all
  // processes belonging to a job have completed.

  for (int j = 0; j < g_num_jobs; /* increment inside */) {
    Job* job = &g_jobs[j];
    int all_done = 1;

    for (int i = 0; i < job->num_pids; ++i) {
      pid_t pid = job->pids[i];
      if (pid <= 0) continue;

      int status = 0;
      pid_t r = waitpid(pid, &status, WNOHANG);

      if (r == 0) {
        all_done = 0; // still running
      } else if (r == -1) {
        // If already reaped, treat as done
        if (errno != ECHILD) all_done = 0;
        job->pids[i] = -1;
      } else {
        // finished
        job->pids[i] = -1;
      }
    }

    if (all_done) {
      // print completion, then remove from list
      print_job_bg_complete(job->job_id, job->first_pid, job->cmd);
      remove_job_at(j);
      continue; // array shifted, re-check same index
    }

    j++;
  }
}

// Prints the job id number, the process id of the first process belonging to
// the Job, and the command string associated with this job
void print_job(int job_id, pid_t pid, const char* cmd) {
  printf("[%d]\t%8d\t%s\n", job_id, pid, cmd);
  fflush(stdout);
}

// Prints a start up message for background processes
void print_job_bg_start(int job_id, pid_t pid, const char* cmd) {
  printf("Background job started: ");
  print_job(job_id, pid, cmd);
}

// Prints a completion message followed by the print job
void print_job_bg_complete(int job_id, pid_t pid, const char* cmd) {
  printf("Completed: \t");
  print_job(job_id, pid, cmd);
}

/***************************************************************************
 * Functions to process commands
 ***************************************************************************/
// Run a program reachable by the path environment variable, relative path, or
// absolute path
void run_generic(GenericCommand cmd) {
  // Execute a program with a list of arguments. The `args` array is a NULL
  // terminated (last string is always NULL) list of strings. The first element
  // in the array is the executable
  char* exec = cmd.args[0];
  char** args = cmd.args;

  if (exec == NULL) {
    fprintf(stderr, "ERROR: empty command\n");
    return;
  }

  execvp(exec, args);
  perror("ERROR: Failed to execute program");
}

// Print strings
void run_echo(EchoCommand cmd) {
  // Print an array of strings. The args array is a NULL terminated (last
  // string is always NULL) list of strings.
  char** str = cmd.args;

  if (str == NULL) {
    putchar('\n');
    fflush(stdout);
    return;
  }

  for (int i = 0; str[i] != NULL; ++i) {
    if (i > 0) putchar(' ');
    fputs(str[i], stdout);
  }
  putchar('\n');

  // Flush the buffer before returning
  fflush(stdout);
}

// Sets an environment variable
void run_export(ExportCommand cmd) {
  // Write an environment variable
  const char* env_var = cmd.env_var;
  const char* val = cmd.val;

  if (env_var == NULL || env_var[0] == '\0') {
    fprintf(stderr, "export: missing variable name\n");
    return;
  }

  if (val == NULL) val = "";
  write_env(env_var, val);
}

// Changes the current working directory
void run_cd(CDCommand cmd) {
  // Get the directory name
  const char* dir = cmd.dir;

  // Check if the directory is valid
  if (dir == NULL) {
    perror("ERROR: Failed to resolve path");
    return;
  }

  // Save old working directory for OLD_PWD
  bool free_old = false;
  char* old_dir = get_current_directory(&free_old);

  if (chdir(dir) != 0) {
    perror("cd");
    if (free_old) free(old_dir);
    return;
  }

  // Update the PWD environment variable to be the new current working
  // directory and optionally update OLD_PWD environment variable to be the old
  // working directory.
  bool free_new = false;
  char* new_dir = get_current_directory(&free_new);

  write_env("OLD_PWD", old_dir);
  write_env("PWD", new_dir);

  if (free_old) free(old_dir);
  if (free_new) free(new_dir);
}

// Sends a signal to all processes contained in a job
void run_kill(KillCommand cmd) {
  int signal = cmd.sig;
  int job_id = cmd.job;

  if (signal == 0) signal = SIGTERM;

  for (int j = 0; j < g_num_jobs; ++j) {
    if (g_jobs[j].job_id != job_id) continue;

    for (int i = 0; i < g_jobs[j].num_pids; ++i) {
      pid_t pid = g_jobs[j].pids[i];
      if (pid > 0) {
        if (kill(pid, signal) != 0) {
          perror("kill");
        }
      }
    }
    return;
  }

  fprintf(stderr, "kill: job %d not found\n", job_id);
}


// Prints the current working directory to stdout
void run_pwd() {
  bool should_free = false;
  char* cwd = get_current_directory(&should_free);

  printf("%s\n", cwd);

  if (should_free) free(cwd);

  // Flush the buffer before returning
  fflush(stdout);
}

// Prints all background jobs currently in the job list to stdout
void run_jobs() {
  // Print background jobs
  for (int j = 0; j < g_num_jobs; ++j) {
    print_job(g_jobs[j].job_id, g_jobs[j].first_pid, g_jobs[j].cmd);
  }

  // Flush the buffer before returning
  fflush(stdout);
}

/***************************************************************************
 * Functions for command resolution and process setup
 ***************************************************************************/

/**
 * @brief A dispatch function to resolve the correct @a Command variant
 * function for child processes.
 *
 * This version of the function is tailored to commands that should be run in
 * the child process of a fork.
 *
 * @param cmd The Command to try to run
 *
 * @sa Command
 */
void child_run_command(Command cmd) {
  CommandType type = get_command_type(cmd);

  switch (type) {
  case GENERIC:
    run_generic(cmd.generic);
    break;

  case ECHO:
    run_echo(cmd.echo);
    break;

  case PWD:
    run_pwd();
    break;

  case JOBS:
    run_jobs();
    break;

  case EXPORT:
  case CD:
  case KILL:
  case EXIT:
  case EOC:
    break;

  default:
    fprintf(stderr, "Unknown command type: %d\n", type);
  }
}

/**
 * @brief A dispatch function to resolve the correct @a Command variant
 * function for the quash process.
 *
 * This version of the function is tailored to commands that should be run in
 * the parent process (quash).
 *
 * @param cmd The Command to try to run
 *
 * @sa Command
 */
void parent_run_command(Command cmd) {
  CommandType type = get_command_type(cmd);

  switch (type) {
  case EXPORT:
    run_export(cmd.export);
    break;

  case CD:
    run_cd(cmd.cd);
    break;

  case KILL:
    run_kill(cmd.kill);
    break;

  case GENERIC:
  case ECHO:
  case PWD:
  case JOBS:
  case EXIT:
  case EOC:
    break;

  default:
    fprintf(stderr, "Unknown command type: %d\n", type);
  }
}

/**
 * @brief Creates one new process centered around the @a Command in the @a
 * CommandHolder setting up redirects and pipes where needed
 *
 * @note Processes are not the same as jobs. A single job can have multiple
 * processes running under it. This function creates a process that is part of a
 * larger job.
 *
 * @note Not all commands should be run in the child process. A few need to
 * change the quash process in some way
 *
 * @param holder The CommandHolder to try to run
 *
 * @sa Command CommandHolder
 */
void create_process(CommandHolder holder) {
  // Read the flags field from the parser
  bool p_in  = holder.flags & PIPE_IN;
  bool p_out = holder.flags & PIPE_OUT;
  bool r_in  = holder.flags & REDIRECT_IN;
  bool r_out = holder.flags & REDIRECT_OUT;
  bool r_app = holder.flags & REDIRECT_APPEND; // This can only be true if r_out
                                               // is true

  // If a builtin needs to affect the parent, run it in the parent when it is
  // not part of a pipe.
  CommandType type = get_command_type(holder.cmd);
  if ((type == EXPORT || type == CD || type == KILL) && !p_in && !p_out) {
    parent_run_command(holder.cmd);
    return;
  }

  int pipefd[2] = {-1, -1};
  if (p_out) {
    if (pipe(pipefd) != 0) {
      perror("pipe");
      return;
    }
  }

  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    if (pipefd[0] != -1) close(pipefd[0]);
    if (pipefd[1] != -1) close(pipefd[1]);
    return;
  }

  if (pid == 0) {
    // Child: setup pipe and redirects

    // If reading from previous pipe, connect it to stdin
    if (p_in && g_prev_pipe_read != -1) {
      if (dup2(g_prev_pipe_read, STDIN_FILENO) < 0) {
        perror("dup2");
        _exit(1);
      }
    }

    // If writing to a new pipe, connect it to stdout
    if (p_out) {
      if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
        perror("dup2");
        _exit(1);
      }
    }

    // Redirect input from file
    if (r_in && holder.redirect_in != NULL) {
      int fd = open(holder.redirect_in, O_RDONLY);
      if (fd < 0) {
        perror("open");
        _exit(1);
      }
      if (dup2(fd, STDIN_FILENO) < 0) {
        perror("dup2");
        _exit(1);
      }
      close(fd);
    }

    // Redirect output to file
    if (r_out && holder.redirect_out != NULL) {
      int flags = O_CREAT | O_WRONLY;
      if (r_app) flags |= O_APPEND;
      else flags |= O_TRUNC;

      int fd = open(holder.redirect_out, flags, 0644);
      if (fd < 0) {
        perror("open");
        _exit(1);
      }
      if (dup2(fd, STDOUT_FILENO) < 0) {
        perror("dup2");
        _exit(1);
      }
      close(fd);
    }

    // Close fds not needed by child
    if (g_prev_pipe_read != -1) close(g_prev_pipe_read);
    if (pipefd[0] != -1) close(pipefd[0]);
    if (pipefd[1] != -1) close(pipefd[1]);

    // Run in child
    child_run_command(holder.cmd);

    // If it was a builtin, it returns; generic should not return on success
    _exit(0);
  }

  // Parent

  remember_pid(pid);

  // Parent closes ends it doesn't need
  if (g_prev_pipe_read != -1) {
    close(g_prev_pipe_read);
    g_prev_pipe_read = -1;
  }

  if (p_out) {
    // Keep read end for next command's PIPE_IN
    close(pipefd[1]);
    g_prev_pipe_read = pipefd[0];
  } else {
    if (pipefd[0] != -1) close(pipefd[0]);
    if (pipefd[1] != -1) close(pipefd[1]);
  }
}

// Run a list of commands
void run_script(CommandHolder* holders) {
  if (holders == NULL)
    return;

  check_jobs_bg_status();

  if (get_command_holder_type(holders[0]) == EXIT &&
      get_command_holder_type(holders[1]) == EOC) {
   end_main_loop(EXIT_SUCCESS);
    return;
  }

  // reset current job tracking and pipe state
  g_cur_npids = 0;
  if (g_prev_pipe_read != -1) {
    close(g_prev_pipe_read);
    g_prev_pipe_read = -1;
  }

  CommandType type;

  // Run all commands in the `holder` array
  for (int i = 0; (type = get_command_holder_type(holders[i])) != EOC; ++i)
    create_process(holders[i]);

  // Close any leftover pipe read end
  if (g_prev_pipe_read != -1) {
    close(g_prev_pipe_read);
    g_prev_pipe_read = -1;
  }

  if (!(holders[0].flags & BACKGROUND)) {
    // Not a background Job
    // Wait for all processes under the job to complete
    for (int i = 0; i < g_cur_npids; ++i) {
      int status = 0;
      if (g_cur_pids[i] > 0) {
        if (waitpid(g_cur_pids[i], &status, 0) < 0) {
          if (errno != ECHILD) perror("waitpid");
        }
      }
    }
  }
  else {
    // A background job.
    // Push the new job to the job queue

    if (g_cur_npids == 0) return;

    if (g_num_jobs >= MAX_JOBS) {
      fprintf(stderr, "ERROR: job list full\n");
      return;
    }

    Job* job = &g_jobs[g_num_jobs++];
    job->job_id = g_next_job_id++;
    job->num_pids = (g_cur_npids > MAX_JOB_PROCS) ? MAX_JOB_PROCS : g_cur_npids;
    job->first_pid = g_cur_pids[0];

    for (int i = 0; i < job->num_pids; ++i)
      job->pids[i] = g_cur_pids[i];

    build_cmd_string(holders, job->cmd, sizeof(job->cmd));

    // Once jobs are implemented, print the start
    print_job_bg_start(job->job_id, job->first_pid, job->cmd);
  }
}