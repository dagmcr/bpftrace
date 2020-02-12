#include <cerrno>
#include <fcntl.h>
#include <cassert>

#include <stdexcept>
#include <string>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>

#include "child.h"
#include "utils.h"

extern char** environ;

namespace bpftrace {

constexpr unsigned int maxargs = 256;
constexpr char CHILD_GO = 'g';
constexpr unsigned int STACK_SIZE = (64 * 1024UL);

std::system_error SYS_ERROR(std::string msg)
{
  return std::system_error(errno, std::generic_category(), msg);
}

static int childfn(void* arg)
{
  struct child_args* args = static_cast<struct child_args*>(arg);

  // Receive SIGTERM if parent dies
  if (prctl(PR_SET_PDEATHSIG, SIGTERM))
  {
    perror("child: prctl(PR_SET_PDEATHSIG)");
    return 10;
  }

  // Convert vector of strings into raw array of C-strings for execve(2)
  char* argv[maxargs];
  int idx = 0;
  for (const auto& arg : args->cmd)
  {
    argv[idx++] = const_cast<char*>(arg.c_str());
  }
  argv[idx] = nullptr; // must be null terminated

  char bf;
  int ret = read(args->pipe_fd, &bf, 1);
  if (ret != 1)
  {
    perror("child: failed to read 'go' pipe");
    return 11;
  }

  close(args->pipe_fd);

  execve(argv[0], argv, environ);

  auto err = "child: failed to execve: " + std::string(argv[0]);
  perror(err.c_str());
  return 12;
}

static void validate_cmd(std::vector<std::string>& cmd)
{
  auto paths = resolve_binary_path(cmd[0]);
  switch (paths.size())
  {
    case 0:
      throw std::runtime_error("path '" + cmd[0] +
                               "' does not exist or is not executable");
    case 1:
      cmd[0] = paths.front().c_str();
      break;
    default:
      throw std::runtime_error("path '" + cmd[0] +
                               "' must refer to a unique binary but matched " +
                               std::to_string(paths.size()) + " binaries");
      return;
  }

  if (cmd.size() >= (maxargs - 1))
  {
    throw std::runtime_error("Too many arguments for command (" +
                             std::to_string(cmd.size()) + " > " +
                             std::to_string(maxargs - 1) + ")");
  }
}

ChildProc::ChildProc(std::string cmd)
{
  auto child_args = std::make_unique<struct child_args>();
  auto child_stack = std::make_unique<char[]>(STACK_SIZE);

  child_args->cmd = split_string(cmd, ' ');
  validate_cmd(child_args->cmd);

  int pipefd[2];
  int ret = pipe2(pipefd, O_CLOEXEC);
  if (ret < 0)
  {
    SYS_ERROR("Failed to create pipe");
  }

  child_args->pipe_fd = pipefd[0];
  child_pipe_ = pipefd[1];

  pid_t cpid = clone(
      childfn, child_stack.get() + STACK_SIZE, SIGCHLD, child_args.get());

  if (cpid <= 0)
  {
    close(pipefd[0]);
    close(pipefd[1]);
    throw SYS_ERROR("Failed to clone child");
  }

  child_pid_ = cpid;
  close(pipefd[0]);
  state_ = State::FORKED;
}

ChildProc::~ChildProc()
{
  close(child_pipe_);

  if (is_alive())
    terminate(true);
}

bool ChildProc::is_alive()
{
  if (!died())
    check_child();
  return !died();
}

void ChildProc::terminate(bool force)
{
  // Make sure child didn't terminate in mean time
  check_child();
  if (died())
    return;

  if (child_pid_ <= 1)
    throw std::runtime_error("BUG: child_pid <= 1");

  int sig = force ? SIGKILL : SIGTERM;

  kill(child_pid_, sig);
  check_child(force);
}

void ChildProc::run(bool pause __attribute__((unused)))
{
  if (!is_alive())
  {
    throw std::runtime_error("Child died unexpectedly");
  }

  assert(state_ == State::FORKED);

  int ret = write(child_pipe_, &CHILD_GO, 1);
  if (ret < 0)
  {
    terminate(true);
    throw SYS_ERROR("Failed to write 'go' pipe");
  }
  state_ = State::RUNNING;
  close(child_pipe_);
}

// private
void ChildProc::check_wstatus(int wstatus)
{
  if (WIFEXITED(wstatus))
    exit_code_ = WEXITSTATUS(wstatus);
  else if (WIFSIGNALED(wstatus))
    term_signal_ = WTERMSIG(wstatus);
  // Ignore STOP and CONT
  else
    return;

  state_ = State::DIED;
}

void ChildProc::check_child(bool block)
{
  int status = 0;

  int flags = WNOHANG;
  if (block)
    flags &= ~WNOHANG;

  pid_t ret;
  while ((ret = waitpid(child_pid_, &status, flags)) < 0 && errno == EINTR)
    ;

  if (ret < 0)
  {
    if (errno == EINVAL)
      throw std::runtime_error("BUG: waitpid() EINVAL");
    else
    {
      std::cerr << "waitpid(" << child_pid_
                << ") returned unexpected error: " << errno
                << ". Marking the child as dead" << std::endl;
      state_ = State::DIED;
      return;
    }
  }

  if (ret == 0)
    return;

  check_wstatus(status);
}

} // namespace bpftrace