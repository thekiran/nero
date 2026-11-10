/* Offline unit test for the extracted -Syn settings prompt.
 *
 * No network, socket or terminal device is touched: stdin is replaced by a
 * pipe and stdout by /dev/null, so the prompt's raw-mode byte reader is
 * exercised deterministically. Hardware is supplied through the existing
 * test override instead of probing the build machine. */
#include "../src/modules/syn/syn_internal.h"

#include "nero/hardware.h"
#include "nero/syn_flood_powered.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

static void expect(int condition, const char *label)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", label);
        failures++;
    }
}

static nero_hardware_info fixture(long cores)
{
    nero_hardware_info info;
    memset(&info, 0, sizeof(info));
    info.os = "Fixture";
    info.architecture = "test";
    info.cpu_logical = cores;
    info.cpu_physical = -1;
    info.ram_total_bytes = 16ULL * 1024 * 1024 * 1024;
    return info;
}

/* Feeds `input` to the prompt and discards its screen output. Returns the
 * prompt result (-100 is a harness failure, never a prompt result). */
static int run_prompt(const char *input, long *threads, long *packet)
{
    int input_pipe[2];
    if (pipe(input_pipe) != 0)
        return -100;

    size_t length = strlen(input);
    if (length != 0) {
        ssize_t written = write(input_pipe[1], input, length);
        if (written != (ssize_t)length) {
            close(input_pipe[0]);
            close(input_pipe[1]);
            return -100;
        }
    }
    close(input_pipe[1]);

    int output = open("/dev/null", O_WRONLY);
    if (output < 0) {
        close(input_pipe[0]);
        return -100;
    }

    int saved_stdin = dup(STDIN_FILENO);
    int saved_stdout = dup(STDOUT_FILENO);
    if (saved_stdin < 0 || saved_stdout < 0) {
        close(input_pipe[0]);
        close(output);
        if (saved_stdin >= 0)
            close(saved_stdin);
        if (saved_stdout >= 0)
            close(saved_stdout);
        return -100;
    }

    if (dup2(input_pipe[0], STDIN_FILENO) < 0 ||
        dup2(output, STDOUT_FILENO) < 0) {
        close(input_pipe[0]);
        close(output);
        close(saved_stdin);
        close(saved_stdout);
        return -100;
    }
    close(input_pipe[0]);
    close(output);

    int result = nero_syn_prompt_settings(threads, packet);

    fflush(stdout);
    dup2(saved_stdin, STDIN_FILENO);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdin);
    close(saved_stdout);
    return result;
}

static void test_accepts_entered_values(void)
{
    long threads = 0, packet = 0;
    int rc = run_prompt("5\n128\n", &threads, &packet);
    expect(rc == 0, "entered worker count and packet size accepted");
    expect(threads == 5, "entered worker count returned unchanged");
    expect(packet == 128, "entered packet size returned unchanged");
}

static void test_enter_keeps_defaults(void)
{
    long threads = 0, packet = 0;
    int rc = run_prompt("\n\n", &threads, &packet);
    expect(rc == 0, "empty entry keeps defaults");
    expect(threads == NERO_WORKLOAD_BALANCED_WORKERS,
           "empty entry keeps suggested worker count");
    expect(packet == NERO_SYN_PACKET_SIZE_MAX,
           "empty entry keeps maximum packet size");
}

static void test_boundaries_are_inclusive(void)
{
    long threads = 0, packet = 0;
    int rc = run_prompt("1\n60\n", &threads, &packet);
    expect(rc == 0 && threads == 1 && packet == NERO_SYN_PACKET_SIZE_MIN,
           "minimum worker count and packet size accepted");

    rc = run_prompt("65535\n4096\n", &threads, &packet);
    expect(rc == 0 && threads == NERO_SYN_THREAD_HARD_MAX &&
               packet == NERO_SYN_PACKET_SIZE_MAX,
           "maximum worker count and packet size accepted");
}

static void test_out_of_range_reprompts(void)
{
    long threads = 0, packet = 0;
    int rc = run_prompt("999999\n64\n100\n", &threads, &packet);
    expect(rc == 0, "out-of-range worker count is rejected and re-prompted");
    expect(threads == 64, "value after rejection is accepted");
    expect(packet == 100, "packet prompt continues after a valid retry");
}

static void test_cancel_returns_error(void)
{
    long threads = 0, packet = 0;
    expect(run_prompt("q", &threads, &packet) == -1,
           "q cancels the prompt");
    expect(run_prompt("", &threads, &packet) == -1,
           "closed stdin cancels the prompt");
}

int main(void)
{
    setenv("NERO_LOG_FILE", ":memory:", 1);

    nero_hardware_info info = fixture(8); /* -> BALANCED suggestion */
    nero_hardware_test_set(&info);

    test_accepts_entered_values();
    test_enter_keeps_defaults();
    test_boundaries_are_inclusive();
    test_out_of_range_reprompts();
    test_cancel_returns_error();

    nero_hardware_test_clear();

    if (failures != 0) {
        fprintf(stderr, "%d syn-settings check(s) failed\n", failures);
        return 1;
    }
    printf("PASS syn settings prompt (offline, piped stdin)\n");
    return 0;
}
