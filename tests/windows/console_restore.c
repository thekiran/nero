/* Use a real console, owned by this test process; no network operations. */
#include "../../src/platform/windows/nero_win_compat.h"
#include <fcntl.h>
#define CHECK(expression) do { if (!(expression)) { \
    fprintf(stderr, "FAIL line %d: %s (Win32=%lu)\n", __LINE__, #expression, GetLastError()); return 1; \
} } while (0)
int main(void)
{
    if (!AllocConsole()) CHECK(GetLastError() == ERROR_ACCESS_DENIED);
    HANDLE input = CreateFileA("CONIN$", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE output = CreateFileA("CONOUT$", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(input != INVALID_HANDLE_VALUE && output != INVALID_HANDLE_VALUE);
    int fd = _open_osfhandle((intptr_t)input, _O_RDONLY);
    CHECK(fd >= 0);
    DWORD original;
    CHECK(GetConsoleMode(input, &original));
    for (unsigned bits = 0; bits < 4; ++bits) {
        DWORD mode = original & ~(DWORD)(ENABLE_PROCESSED_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT);
        if (bits & 1) mode |= ENABLE_PROCESSED_INPUT;
        if (bits & 2) mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
        CHECK(SetConsoleMode(input, mode));
        struct termios saved, raw;
        CHECK(tcgetattr(fd, &saved) == 0);
        raw = saved; raw.c_lflag &= ~(ICANON | ECHO);
        CHECK(tcsetattr(fd, TCSANOW, &raw) == 0);
        CHECK(tcsetattr(fd, TCSANOW, &saved) == 0);
        DWORD restored;
        CHECK(GetConsoleMode(input, &restored) && restored == mode);
    }
    CHECK(SetConsoleMode(input, original));
    HANDLE old_output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD output_mode;
    CHECK(GetConsoleMode(output, &output_mode));
    UINT codepage = GetConsoleOutputCP();
    CHECK(SetStdHandle(STD_OUTPUT_HANDLE, output));
    nero_windows_init();
    nero_windows_restore();
    DWORD restored;
    CHECK(GetConsoleMode(output, &restored) && restored == output_mode);
    CHECK(GetConsoleOutputCP() == codepage);
    nero_windows_restore(); /* Idempotent cleanup. */
    CHECK(SetStdHandle(STD_OUTPUT_HANDLE, old_output));
    _close(fd); CloseHandle(output);
    puts("PASS: real Win32 input/output console modes and code page restored");
    return 0;
}
