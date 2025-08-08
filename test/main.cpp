#include <shpp/shpp.hpp>
#include <sstream>

int main()
{
  using namespace shpp;

  // Console: stdout->cout, stderr->cerr
  std::cout << "\n---------------------\n";
  CC % "ls -ltc";

  // Console with pipe
  std::cout << "\n---------------------\n";
  CC % "ls -ltc" | "grep main";

  {
    // Split to your own streams
    std::cout << "\n---------------------\n";
    std::ostringstream out, err;
    SS{out, err} % "ls -ltc";
    // out.str() and err.str() now contain the captured text

    std::cout << "Out: " << out.str();
    std::cout << "Err: " << err.str();
  }

  {
    std::cout << "\n---------------------\n";
    std::ostringstream out, err;
    SS{out, err} % R"(bash -lc 'echo hello; echo oops 1>&2')";
    std::cout << "Out: " << out.str();
    std::cout << "Err: " << err.str();
  }

  {
    std::cout << "\n---------------------\n";
    std::ostringstream err;
    CS{err} % R"(bash -lc 'echo only-stdout-to-console; echo only-stderr-to-stream 1>&2')";
    std::cout << "Err: " << err.str();
  }

  // Get the exit code explicitly
  std::cout << "\n---------------------\n";
  auto r = (CC % "bash -lc \"echo ok && false\""); // last cmd's status
  int code = r.run().exit_code;                    // 1 (because 'false' exits 1)
  std::cout << "code: " << code << std::endl;
}
