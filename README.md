# shpp — a tiny shell-style pipeline DSL for C++

Run external commands from C++ with a Bash-y feel—without writing Bash *scripts*.
Think:

```cpp
using namespace shpp;

CC % "ls -ltc";
CC % R"(bash -lc 'printf "one\ntwo\n"')" | "grep two";

std::ostringstream out, err;
SS{out, err} % R"(bash -lc 'echo hello; echo oops 1>&2')";

CS{err} % R"(bash -lc 'echo only-stdout-to-console; echo only-stderr-to-stream 1>&2')";

auto res = (CC % R"(bash -lc 'false')").run(); // explicit
```

It’s header-only (POSIX) and intentionally small.

---

# Features

* **Sinks** (stdout/stderr routing):

  * `CC` → stdout→`std::cout`, stderr→`std::cerr`
  * `SC_t{out}` / `SC` → stdout→`out`, stderr→`std::cerr`
  * `CS_t{err}` / `CS` → stdout→`std::cout`, stderr→`err`
  * `SS_t{out, err}` / `SS` → split to your streams
* **Pipes**: `CC % "cmd1" | "cmd2" | "cmd3";` (pipes **stdout** only, Bash semantics)
* **RAII run**: Pipelines auto-run at end of the full expression; or call `.run()` to get a `Result`.
* **Solid parsing** for direct exec: spaces, `'single'` and `"double"` quotes, backslash escapes, `$VAR` / `${VAR}` env expansion (not in single quotes), and `~` at word start.

> ❗ **Shell syntax (`;`, `&&`, redirections, globs, etc.) is not interpreted** unless you explicitly run a shell (e.g., `bash -lc '...'`). See examples below.

---

# Requirements

* POSIX (Linux/macOS): uses `fork/execvp/pipe/waitpid/dup2`
* C++20

Windows backend not implemented yet (see **Portability**).

---

# Quick start

```cpp
#include "shpp.hpp"
#include <sstream>
using namespace shpp;

int main() {
  // 1) Print to console
  CC % "ls -ltc";

  // 2) Pipe like Bash (stdout only is piped)
  CC % R"(bash -lc 'printf "one\ntwo\n"')" | "grep two";

  // 3) Capture to your own streams
  std::ostringstream out, err;
  SS{out, err} % R"(bash -lc 'echo hello; echo oops 1>&2')";
  // out == "hello\n", err == "oops\n"

  // 4) Split sinks
  SC_t sc{out};  sc % R"(bash -lc 'echo to-out; echo to-stderr 1>&2')"; // stderr -> std::cerr
  CS_t cs{err};  cs % R"(bash -lc 'echo visible; echo hidden 1>&2')";   // stderr -> err

  // 5) Exit status from the *last* stage
  auto res = (CC % R"(bash -lc 'true && false')").run();
  return res.exit_code; // 1
}
```

---

# API

## Sinks

```cpp
namespace shpp {
  struct CC_t { };                 // console/console
  inline constexpr CC_t CC{};

  struct SC_t { std::ostream& out; };               // stdout -> out,   stderr -> std::cerr
  struct CS_t { std::ostream& err; };               // stdout -> cout,  stderr -> err
  struct SS_t { std::ostream& out; std::ostream& err; }; // split streams

  using SC = SC_t;
  using SS = SS_t;
  using CS = CS_t;
}
```

## Building a pipeline

* Start with a **sink** and `% "command ..."`.
* Extend with `| "next ..."`.
* End the statement with `;` and it will run automatically (RAII), **or** call `.run()`.

Operator precedence works in our favor: `%` binds tighter than `|`, so `CC % "a" | "b"` is parsed as `(CC % "a") | "b"`.

## Running & results

```cpp
struct Result {
  int exit_code;                     // exit status of the *last* stage
  std::vector<int> stage_statuses;   // raw wait() statuses for each stage
};
```

`.run()` returns `Result`. If you don’t call `.run()`, the pipeline runs in the destructor (noexcept).

---

# Shell vs. direct exec

By default, shpp runs programs **directly** with `execvp`. That means:

* ✅ `CC % "grep magic"` — runs `/usr/bin/grep` with args.
* ❌ `CC % "echo hi && echo bye"` — `&&` is *not* interpreted (no shell).

If you want shell operators (`;`, `&&`, `||`, redirections, globs, subshells, etc.), **wrap that stage in Bash**:

```cpp
SS{out, err} % R"(bash -lc 'echo hello; echo oops 1>&2')";
CC % R"(bash -lc 'printf "one\ntwo\n"')" | "grep two";
```

> ⚠️ Don’t feed untrusted strings to `bash -lc '...'` (shell injection risk). Prefer plain argv when you can.

---

# Behavior & caveats

* **Pipes:** Only **stdout** is piped between stages. `stderr` of non-final stages goes to the parent’s `std::cerr`. The final stage’s `stderr` goes wherever your sink routes it.
* **CLOEXEC:** All pipes are created `O_CLOEXEC` (or marked `FD_CLOEXEC`) to avoid fd leaks across `exec`.
* **Threading:** Two threads pump the final stage’s stdout/stderr into your selected `std::ostream`s.
* **Errors:**

  * `execvp` failure in a child prints to that child’s `stderr` and exits `127`; you’ll see it via the sink.
  * Parent syscall failures throw `std::system_error` from `.run()`; destructors never throw.
* **RAII safety:** Moved-from pipeline objects are disarmed so their destructor won’t auto-run an empty pipeline.

---

# Portability

* **Linux/macOS**: supported today.
* **Windows**: not yet. Easiest path is a Windows runner using `CreateProcess`/pipes while keeping this same DSL; or build the DSL on top of `Boost.Process`.

---

# Security notes

This library executes external programs. If arguments come from users, *don’t* concatenate them into `bash -lc '...'`. Either:

* run programs directly with argv (no shell), or
* strictly validate/escape, understanding the risks.
