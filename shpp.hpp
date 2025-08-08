#pragma once
#include <iostream>
#include <vector>

namespace shpp
{
  // ——— Sinks (where output goes) ———
  struct CC_t
  {
  };
  inline constexpr CC_t CC{}; // console stdout+stderr
  struct SC_t
  {
    std::ostream &out;
  }; // stdout -> out, stderr -> std::cerr
  struct CS_t
  {
    std::ostream &err;
  }; // stdout -> std::cout, stderr -> err

  struct SS_t
  {
    std::ostream &out;
    std::ostream &err;
  }; // split streams

  struct Cmd
  {
    std::string prog;
    std::vector<std::string> args; // args[0] should be prog for execvp
    static Cmd parse(std::string_view s);
  };

  // ——— Pipeline ———
  struct Pipeline
  {
    std::vector<Cmd> stages;
  };

  // ——— Result ———
  struct Result
  {
    int exit_code = 0;               // of the *last* stage
    std::vector<int> stage_statuses; // wait status for each stage
  };

  // ——— Core runner ———
  namespace detail
  {

    struct Fd
    {
      int fd = -1;
      Fd() = default;
      explicit Fd(int f);
      Fd(const Fd &) = delete;
      Fd &operator=(const Fd &) = delete;
      Fd(Fd &&o) noexcept;
      Fd &operator=(Fd &&o) noexcept;
      ~Fd();
      void close();
      int release();
    };
  } // namespace detail

  // ——— Pipeline builder that runs on destruction unless .run() was called ———
  class Pending
  {
    Pipeline pl_;
    std::ostream *out_;
    std::ostream *err_;
    bool armed_ = true; // was executed_; true means "auto-run in dtor"

  public:
    Pending(Pipeline pl, std::ostream &out, std::ostream &err);

    // Disarm moved-from, so its dtor won't auto-run
    Pending(Pending &&other) noexcept;
    Pending &operator=(Pending &&other) noexcept;
    ~Pending() noexcept;
    Result run();
    friend Pending operator|(Pending &&p, std::string_view rhs);
  };

  // ——— Top-level operators ———
  inline Pending operator%(CC_t, std::string_view cmd)
  {
    Pipeline pl;
    pl.stages.push_back(Cmd::parse(cmd));
    return Pending(std::move(pl), std::cout, std::cerr);
  }
  inline Pending operator%(SC_t sc, std::string_view cmd)
  {
    Pipeline pl;
    pl.stages.push_back(Cmd::parse(cmd));
    return Pending(std::move(pl), sc.out, std::cerr);
  }
  inline Pending operator%(CS_t cs, std::string_view cmd)
  {
    Pipeline pl;
    pl.stages.push_back(Cmd::parse(cmd));
    return Pending(std::move(pl), std::cout, cs.err);
  }
  inline Pending operator%(SS_t ss, std::string_view cmd)
  {
    Pipeline pl;
    pl.stages.push_back(Cmd::parse(cmd));
    return Pending(std::move(pl), ss.out, ss.err);
  }

  using SC = SC_t;
  using SS = SS_t;
  using CS = CS_t;
} // namespace shpp
