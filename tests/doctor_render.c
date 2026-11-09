/* Deterministic platform/error matrix, no platform spoofing in production. */
#include "../src/core/doctor_internal.h"
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) abort(); } while (0)
int main(void)
{
    for (int os = 0; os < 5; ++os) {
        for (int state = -1; state < 3; ++state) {
            doctor_snapshot s = {0};
            s.os = (doctor_os)os; s.architecture = state;
            s.executable = state; s.path = state; s.terminal = 0;
            s.config = state; s.privilege = state;
            s.scanner = os > 0 && os < 4; s.raw_engine = os == DOCTOR_LINUX;
            FILE *file = tmpfile(); CHECK(file);
            CHECK(doctor_render(file, &s) == 0);
            CHECK(fseek(file, 0, SEEK_SET) == 0);
            char output[8192] = {0};
            CHECK(fread(output, 1, sizeof(output) - 1, file) > 0);
            CHECK(strstr(output, "NERO - Offline Doctor"));
            CHECK(strstr(output, "No network tests, file changes"));
            CHECK(!strchr(output, '\033'));
            CHECK(strstr(output, os == 0 || os == 4 ? "Unsupported/unknown" :
                         os == DOCTOR_MACOS ? "macOS" : os == DOCTOR_LINUX ? "Linux" : "Windows"));
            fclose(file);
        }
    }
    return 0;
}
