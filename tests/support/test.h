// A tiny test harness for next's ctest executables. Catch2 would dwarf the
// code under test and pull <iostream>/exceptions into every test binary; this
// is all the tests need: named cases, CHECK (continue) and REQUIRE (abort the
// case), and a non-zero exit when anything failed.
//
//   TEST("json: round trip") { CHECK(a == b); REQUIRE(p != nullptr); }
//   int main() { return base::test::runAll(); }   // or BASE_TEST_MAIN()
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace base::test {

using Fn = void (*)();
struct Case {
    const char *name;
    Fn          fn;
    Case       *next;
};

// Registers a case at static-init time (called by the TEST macro).
int  add(Case *c);
// Records a failed check; returns false so REQUIRE can bail out.
bool fail(const char *file, int line, const char *expr);
// Prints both sides of a failed string comparison.
bool failEq(const char *file, int line, const char *expr, std::string_view a, std::string_view b);
// Runs every case whose name contains argv[1] (all when absent).
int  runAll(int argc = 0, char **argv = nullptr);

inline bool eq(std::string_view a, std::string_view b) {
    return a == b;
}

// ── Portable helpers for what tests need from POSIX (mkdtemp, setenv, …) ───
// A new empty directory "<parent>/<prefix>XXXXXX", the parent made first;
// parent "" = /tmp, or %TEMP% on Windows. '/' separators; "" when it could
// not be made. Deleted with all it holds when the test process exits.
std::string makeTempDir(const char *prefix, const std::string &parent = {});
// Deletes a file or a directory tree; symlinks are removed, never followed.
void        removeTree(const std::string &path);
// setenv / unsetenv (Windows: _wputenv_s, so child processes see it too).
void        setEnv(const char *name, const std::string &value);
void        unsetEnv(const char *name);
// Sets an environment variable back to `old` ("" = unset), as a test found it.
void        restoreEnv(const char *name, const std::string &old);
// Sets a file's modification time (utimes).
bool        setModifiedTime(const std::string &path, int64_t unixSeconds);
// realpath: absolute, symlinks resolved ('/' separators); "" when missing.
std::string realPath(const std::string &path);
// Kills a process outright (SIGKILL, TerminateProcess).
void        killProcess(int64_t pid);
// "file://" + an absolute path, as a file manager writes it ("file:///C:/x"
// on Windows); the path is taken as already percent-encoded.
std::string fileUri(const std::string &path);

} // namespace base::test

#define BASE_TEST_CAT2(a, b) a##b
#define BASE_TEST_CAT(a, b) BASE_TEST_CAT2(a, b)
#define TEST(name)                                                                                 \
    static void             BASE_TEST_CAT(testFn_, __LINE__)();                                    \
    static base::test::Case BASE_TEST_CAT(testCase_, __LINE__){                                    \
        name, &BASE_TEST_CAT(testFn_, __LINE__), nullptr                                           \
    };                                                                                             \
    static int BASE_TEST_CAT(testReg_, __LINE__) =                                                 \
        base::test::add(&BASE_TEST_CAT(testCase_, __LINE__));                                      \
    static void BASE_TEST_CAT(testFn_, __LINE__)()

#define CHECK(expr) ((expr) ? true : base::test::fail(__FILE__, __LINE__, #expr))
#define CHECK_FALSE(expr) CHECK(!(expr))
#define REQUIRE(expr)                                                                              \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            base::test::fail(__FILE__, __LINE__, #expr);                                           \
            return;                                                                                \
        }                                                                                          \
    } while (0)
// String equality that prints both sides on failure.
#define CHECK_STR(a, b)                                                                            \
    (base::test::eq((a), (b)) ? true                                                               \
                              : base::test::failEq(__FILE__, __LINE__, #a " == " #b, (a), (b)))

#define BASE_TEST_MAIN()                                                                           \
    int main(int argc, char **argv) {                                                              \
        return base::test::runAll(argc, argv);                                                     \
    }
