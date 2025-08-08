#include "shpp.hpp"
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

template <class... Ts>
struct overloaded : Ts...
{
  using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

// Extended splitter: quotes, escapes, $VAR/${VAR}, ~ at word start.
// Throws std::runtime_error on unmatched quotes.
static inline std::vector<std::string> split_cmd(std::string_view s)
{
  enum class State { Unquoted, InSingle, InDouble };
  State st = State::Unquoted;

  std::vector<std::string> parts;
  std::string cur;

  const auto push_token = [&]() {
    parts.push_back(cur);
    cur.clear();
  };

  auto get_env = [](std::string_view name) -> std::string {
    if (name.empty())
      return {};
    if (const char *v = std::getenv(std::string(name).c_str()))
      return std::string(v);
    return {};
  };

  auto is_name_start = [](unsigned char c) { return std::isalpha(c) || c == '_'; };
  auto is_name_char = [](unsigned char c) { return std::isalnum(c) || c == '_'; };

  bool in_token = false;     // whether we're currently building a token (even if cur is empty)
  bool at_word_start = true; // for ~ expansion in unquoted context

  for (size_t i = 0; i < s.size(); ++i)
  {
    char c = s[i];

    if (st == State::Unquoted)
    {
      if (std::isspace(static_cast<unsigned char>(c)))
      {
        if (in_token)
        {
          push_token();
          in_token = false;
        }
        at_word_start = true;
        continue;
      }

      if (c == '\'')
      {
        st = State::InSingle;
        in_token = true;
        at_word_start = false;
        continue;
      }
      if (c == '"')
      {
        st = State::InDouble;
        in_token = true;
        at_word_start = false;
        continue;
      }

      if (c == '\\')
      {
        in_token = true;
        at_word_start = false;
        if (i + 1 < s.size())
        {
          cur.push_back(s[++i]);
        }
        else
        {
          cur.push_back('\\');
        }
        continue;
      }

      if (c == '~' && at_word_start)
      {
        in_token = true;
        at_word_start = false;
        if (const char *home = std::getenv("HOME"))
          cur.append(home);
        else
          cur.push_back('~');
        continue;
      }

      if (c == '$')
      {
        in_token = true;
        at_word_start = false;
        // ${VAR}
        if (i + 1 < s.size() && s[i + 1] == '{')
        {
          size_t j = i + 2, end = j;
          while (end < s.size() && s[end] != '}')
            ++end;
          if (end >= s.size())
            throw std::runtime_error("Unclosed ${...} in command");
          cur += get_env(s.substr(j, end - j));
          i = end; // skip '}'
          continue;
        }
        // $VAR
        size_t j = i + 1;
        if (j < s.size() && is_name_start(static_cast<unsigned char>(s[j])))
        {
          ++j;
          while (j < s.size() && is_name_char(static_cast<unsigned char>(s[j])))
            ++j;
          cur += get_env(s.substr(i + 1, j - (i + 1)));
          i = j - 1;
        }
        else
        {
          // Not a valid name start; treat '$' literally.
          cur.push_back('$');
        }
        continue;
      }

      // normal char
      cur.push_back(c);
      in_token = true;
      at_word_start = false;
      continue;
    }

    if (st == State::InSingle)
    {
      if (c == '\'')
      {
        st = State::Unquoted;
        continue;
      }
      cur.push_back(c); // no escapes, no env expansion
      in_token = true;
      continue;
    }

    // State::InDouble
    if (c == '"')
    {
      st = State::Unquoted;
      at_word_start = false;
      continue;
    }
    if (c == '\\')
    {
      if (i + 1 < s.size())
      {
        cur.push_back(s[++i]);
      }
      else
      {
        cur.push_back('\\');
      }
      in_token = true;
      continue;
    }
    if (c == '$')
    {
      // env expansion allowed in double quotes
      if (i + 1 < s.size() && s[i + 1] == '{')
      {
        size_t j = i + 2, end = j;
        while (end < s.size() && s[end] != '}')
          ++end;
        if (end >= s.size())
          throw std::runtime_error("Unclosed ${...} in command");
        cur += get_env(s.substr(j, end - j));
        i = end;
      }
      else
      {
        size_t j = i + 1;
        if (j < s.size() && is_name_start(static_cast<unsigned char>(s[j])))
        {
          ++j;
          while (j < s.size() && is_name_char(static_cast<unsigned char>(s[j])))
            ++j;
          cur += get_env(s.substr(i + 1, j - (i + 1)));
          i = j - 1;
        }
        else
        {
          cur.push_back('$');
        }
      }
      in_token = true;
      continue;
    }
    // normal char inside double quotes
    cur.push_back(c);
    in_token = true;
  }

  if (st == State::InSingle)
    throw std::runtime_error("Unclosed single quote in command");
  if (st == State::InDouble)
    throw std::runtime_error("Unclosed double quote in command");

  if (in_token)
    push_token();
  return parts;
}

static inline void set_cloexec(int fd)
{
  int flags = fcntl(fd, F_GETFD);
  if (flags >= 0)
    fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static inline void make_pipe_cloexec(int fds[2])
{
#if defined(__linux__) && defined(O_CLOEXEC)
  if (::pipe2(fds, O_CLOEXEC) == 0)
    return;
    // if pipe2 isn’t supported at runtime, fall through
#endif
  if (::pipe(fds) != 0)
    throw std::system_error(errno, std::generic_category(), "pipe");
  set_cloexec(fds[0]);
  set_cloexec(fds[1]);
}

static inline shpp::Result exec_pipeline(const shpp::Pipeline &pl, std::ostream &out, std::ostream &err)
{
  if (pl.stages.empty())
    throw std::runtime_error("empty pipeline");

  const size_t N = pl.stages.size();
  std::vector<pid_t> pids(N, -1);

  // Detect real console sinks (avoid capture -> no tOut/tErr threads)
  const bool to_console_out = (&out == &std::cout);
  const bool to_console_err = (&err == &std::cerr);

  // ===== stdin source? (std::variant<std::monostate, in::String, in::Stream>)
  const bool have_in = !std::holds_alternative<std::monostate>(pl.stdin_src);
  shpp::detail::Fd inR, inW;
  if (have_in)
  {
    int fds[2];
    make_pipe_cloexec(fds);
    inR = shpp::detail::Fd(fds[0]);
    inW = shpp::detail::Fd(fds[1]);
  }

  // ===== pipes between stages (stdout chaining)
  std::vector<std::pair<shpp::detail::Fd, shpp::detail::Fd>> pipes; // (read, write)
  pipes.reserve(N ? N - 1 : 0);
  for (size_t i = 0; i + 1 < N; ++i)
  {
    int fds[2];
    make_pipe_cloexec(fds);
    pipes.emplace_back(shpp::detail::Fd(fds[0]), shpp::detail::Fd(fds[1]));
  }

  // ===== capture pipes ONLY if not going to console
  shpp::detail::Fd capOutR, capOutW, capErrR, capErrW;
  if (!to_console_out)
  {
    int fds[2];
    make_pipe_cloexec(fds);
    capOutR = shpp::detail::Fd(fds[0]);
    capOutW = shpp::detail::Fd(fds[1]);
  }
  if (!to_console_err)
  {
    int fds[2];
    make_pipe_cloexec(fds);
    capErrR = shpp::detail::Fd(fds[0]);
    capErrW = shpp::detail::Fd(fds[1]);
  }

  // ===== fork/exec stages
  for (size_t i = 0; i < N; ++i)
  {
    pid_t pid = ::fork();
    if (pid < 0)
      throw std::system_error(errno, std::generic_category(), "fork");

    if (pid == 0)
    {
      // ---- Child ----

      // stdin
      if (i == 0)
      {
        if (have_in)
          ::dup2(inR.fd, STDIN_FILENO);
        // else: inherit parent's stdin
      }
      else
      {
        ::dup2(pipes[i - 1].first.fd, STDIN_FILENO);
      }

      // stdout
      if (i + 1 < N)
      {
        ::dup2(pipes[i].second.fd, STDOUT_FILENO);
      }
      else
      {
        if (!to_console_out)
        {
          ::dup2(capOutW.fd, STDOUT_FILENO); // capture
        }                                    // else: inherit parent's stdout directly (console)
      }

      // stderr
      if (i + 1 == N)
      {
        if (!to_console_err)
        {
          ::dup2(capErrW.fd, STDERR_FILENO); // capture
        }                                    // else: inherit parent's stderr (console)
      }
      // Non-final stages keep default stderr -> parent's stderr

      // Close fds we don't need in the child
      for (auto &p : pipes)
      {
        p.first.close();
        p.second.close();
      }
      if (have_in)
      {
        inR.close();
        inW.close();
      }
      if (!to_console_out)
      {
        capOutR.close();
        capOutW.close();
      }
      if (!to_console_err)
      {
        capErrR.close();
        capErrW.close();
      }

      // Build argv and exec
      const shpp::Cmd &c = pl.stages[i];
      std::vector<char *> argv;
      argv.reserve(c.args.size() + 1);
      for (auto &s : c.args)
        argv.push_back(const_cast<char *>(s.c_str()));
      argv.push_back(nullptr);

      ::execvp(c.prog.c_str(), argv.data());
      std::fprintf(stderr, "execvp(%s) failed: %s\n", c.prog.c_str(), std::strerror(errno));
      _exit(127);
    }

    // ---- Parent ----
    pids[i] = pid;
    if (i > 0)
      pipes[i - 1].first.close(); // parent doesn't read from previous
    if (i + 1 < N)
      pipes[i].second.close(); // parent doesn't write to next
  }

  // Parent: close capture write ends (children inherited dup'd ones)
  if (!to_console_out)
    capOutW.close();
  if (!to_console_err)
    capErrW.close();

  // ===== stdin pump (parent writes into inW, then closes to send EOF)
  std::thread tIn;
  if (have_in)
  {
    inR.close(); // parent never reads from inR
    auto write_all = [](int fd, const char *p, size_t n) {
      while (n)
      {
        ssize_t m = ::write(fd, p, n);
        if (m > 0)
        {
          p += m;
          n -= size_t(m);
        }
        else if (m < 0 && errno == EINTR)
          continue;
        else
          break; // error/EPIPE
      }
    };
    int wfd = inW.release(); // hand ownership to thread
    tIn = std::thread([wfd, src = pl.stdin_src, write_all]() {
      std::visit(
        [&](auto &&s) {
          using T = std::decay_t<decltype(s)>;
          if constexpr (std::is_same_v<T, std::monostate>) {}
          else if constexpr (std::is_same_v<T, shpp::InString>)
          {
            write_all(wfd, s.data.data(), s.data.size());
          }
          else if constexpr (std::is_same_v<T, shpp::InStream>)
          {
            if (!s.is)
              return;
            char buf[4096];
            while (s.is->good())
            {
              s.is->read(buf, sizeof(buf));
              std::streamsize got = s.is->gcount();
              if (got > 0)
                write_all(wfd, buf, size_t(got));
            }
          }
        },
        src);
      ::close(wfd); // EOF to child
    });
  }

  // ===== capture pumps ONLY if we’re not writing to console
  std::thread tOut, tErr;
  if (!to_console_out)
  {
    int out_fd = capOutR.release();
    tOut = std::thread([out_fd, &out] {
      char buf[4096];
      for (;;)
      {
        ssize_t n = ::read(out_fd, buf, sizeof(buf));
        if (n > 0)
        {
          out.write(buf, n);
          out.flush();
        }
        else if (n == 0)
          break; // EOF
        else if (errno == EINTR)
          continue;
        else
          break; // read error
      }
      ::close(out_fd);
    });
  }
  if (!to_console_err)
  {
    int err_fd = capErrR.release();
    tErr = std::thread([err_fd, &err] {
      char buf[4096];
      for (;;)
      {
        ssize_t n = ::read(err_fd, buf, sizeof(buf));
        if (n > 0)
        {
          err.write(buf, n);
          err.flush();
        }
        else if (n == 0)
          break;
        else if (errno == EINTR)
          continue;
        else
          break;
      }
      ::close(err_fd);
    });
  }

  // ===== wait for children
  shpp::Result res;
  res.stage_statuses.resize(N);
  int last_status = 0;
  for (size_t i = 0; i < N; ++i)
  {
    int st = 0;
    if (::waitpid(pids[i], &st, 0) < 0)
      throw std::system_error(errno, std::generic_category(), "waitpid");
    res.stage_statuses[i] = st;
    if (i + 1 == N)
      last_status = st;
  }

  // ===== joins
  if (tIn.joinable())
    tIn.join();
  if (tOut.joinable())
    tOut.join();
  if (tErr.joinable())
    tErr.join();

  if (WIFEXITED(last_status))
    res.exit_code = WEXITSTATUS(last_status);
  else if (WIFSIGNALED(last_status))
    res.exit_code = 128 + WTERMSIG(last_status);
  else
    res.exit_code = -1;

  return res;
}

namespace shpp
{

  Cmd Cmd::parse(std::string_view s)
  {
    auto p = split_cmd(s);
    if (p.empty())
      throw std::runtime_error("empty command");
    Cmd c;
    c.prog = p[0];
    c.args = p;
    return c;
  }

  // ——— Core runner ———
  namespace detail
  {
    Fd::Fd(int f) : fd(f) {}
    Fd::Fd(Fd &&o) noexcept : fd(o.fd)
    {
      o.fd = -1;
    }
    Fd &Fd::operator=(Fd &&o) noexcept
    {
      if (this != &o)
      {
        close();
        fd = o.fd;
        o.fd = -1;
      }
      return *this;
    }
    Fd::~Fd()
    {
      close();
    }
    void Fd::close()
    {
      if (fd >= 0)
      {
        ::close(fd);
        fd = -1;
      }
    }
    int Fd::release()
    {
      int t = fd;
      fd = -1;
      return t;
    }

  } // namespace detail

  // ——— Pipeline builder that runs on destruction unless .run() was called ———
  Pending::Pending(Pipeline pl, std::ostream &out, std::ostream &err)
    : pl_(std::move(pl)), out_(&out), err_(&err)
  {
  }

  // Disarm moved-from, so its dtor won't auto-run
  Pending::Pending(Pending &&other) noexcept
    : pl_(std::move(other.pl_)), out_(other.out_), err_(other.err_), armed_(other.armed_)
  {
    other.armed_ = false;
    other.out_ = nullptr;
    other.err_ = nullptr;
  }

  Pending &Pending::operator=(Pending &&other) noexcept
  {
    if (this != &other)
    {
      pl_ = std::move(other.pl_);
      out_ = other.out_;
      err_ = other.err_;
      armed_ = other.armed_;
      other.armed_ = false;
      other.out_ = nullptr;
      other.err_ = nullptr;
    }
    return *this;
  }

  Pending::~Pending() noexcept
  {
    if (armed_)
    {
      try
      {
        (void)exec_pipeline(pl_, *out_, *err_);
      }
      catch (...)
      { /* never throw from a destructor */
      }
    }
  }

  shpp::Result Pending::run()
  {
    armed_ = false;
    return exec_pipeline(pl_, *out_, *err_);
  }

  shpp::Pending operator|(shpp::Pending &&p, std::string_view rhs)
  {
    p.pl_.stages.push_back(shpp::Cmd::parse(rhs));
    return std::move(p);
  }
} // namespace shpp
