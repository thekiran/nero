/* UI-only fixture: exercises long lists without creating network traffic. */
#include "nero/operation_ui.h"
#include <termios.h>
#include <unistd.h>

int main(void)
{
    struct termios saved, raw;
    if (tcgetattr(STDIN_FILENO, &saved) != 0) return 1;
    raw = saved;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return 1;
    nero_operation_ui ui = {0};
    int ports[100];
    for (int i = 0; i < 100; ++i) ports[i] = 1000 + i;
    nero_operation_begin(&ui, "Port scan", "127.0.0.1", "Esc · Back");
    nero_operation_discovery(ports, 7, 7, 3, 8197);
    int selected = 0;
    for (;;) {
        nero_operation_ports(&ui, ports, 100, selected);
        nero_ui_key key = nero_operation_read_key(50);
        if (key == NERO_UI_KEY_CANCEL || key == NERO_UI_KEY_EOF) break;
        if (key == NERO_UI_KEY_DOWN && selected < 99) ++selected;
        if (key == NERO_UI_KEY_UP && selected > 0) --selected;
    }
    nero_operation_end(&ui);
    tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    return 0;
}
