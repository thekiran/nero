#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#define MAX_INTERFACES 128
#define MAX_NAME_LEN 64

// ANSI color codes
enum {
    COLOR_RESET = 0,
    COLOR_BLUE = 34,    // Ethernet
    COLOR_GREEN = 32,   // Wireless
    COLOR_YELLOW = 33,  // Loopback
    COLOR_RED = 31      // Other
};

// Read the interface type from sysfs
static int get_interface_type(const char *ifname) {
    char path[128];
    FILE *fp;
    int type = -1;

    snprintf(path, sizeof(path), "/sys/class/net/%s/type", ifname);
    fp = fopen(path, "r");
    if (!fp) return -1;

    if (fscanf(fp, "%d", &type) != 1)
        type = -1;
    fclose(fp);
    return type;
}

// Human-readable interface type
static const char* type_str(int type) {
    switch (type) {
        case 1:   return "Ethernet";
        case 801: return "Wireless";
        case 772: return "Loopback";
        default:  return "Unknown";
    }
}

// Print a colored interface row
static void print_colored(int idx, const char *name, const char *desc, int type) {
    int color;
    if (type == 1)        color = COLOR_BLUE;   // Ethernet
    else if (type == 801) color = COLOR_GREEN;  // Wireless
    else if (type == 772) color = COLOR_YELLOW; // Loopback
    else                  color = COLOR_RED;    // Other

    printf("%2d) \033[1;%dm%-15s\033[0m (%s)\n", idx, color, name, desc);
}

// List interfaces and read the selection
const char *list_and_select_interface(void) {
    static char names[MAX_INTERFACES][MAX_NAME_LEN];
    static int types[MAX_INTERFACES];
    int count = 0;

    DIR *d = opendir("/sys/class/net");
    if (!d) {
        perror("opendir /sys/class/net");
        return NULL;
    }

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && count < MAX_INTERFACES) {
        if (entry->d_name[0] == '.')
            continue;

        int t = get_interface_type(entry->d_name);
        if (t == -1)
            continue;

        strncpy(names[count], entry->d_name, MAX_NAME_LEN - 1);
        names[count][MAX_NAME_LEN - 1] = '\0';
        types[count] = t;
        count++;
    }
    closedir(d);

    if (count == 0) {
        fprintf(stderr, "No network interfaces found.\n");
        return NULL;
    }

    // Clear the screen
    printf("\033[H\033[J");
    printf("\033[1;31mAvailable Network Interfaces:\033[0m\n\n");

    for (int i = 0; i < count; ++i) {
        print_colored(i + 1, names[i], type_str(types[i]), types[i]);
    }

    char input[128];
    printf("\nSelect an interface [1-%d]: ", count);
    if (!fgets(input, sizeof(input), stdin) ||
        (!strchr(input, '\n') && !feof(stdin))) {
        fprintf(stderr, "Invalid selection.\n");
        return NULL;
    }
    char *end;
    errno = 0;
    long choice = strtol(input, &end, 10);
    int invalid = errno == ERANGE || end == input;
    while (isspace((unsigned char)*end))
        ++end;
    if (invalid || *end != '\0' || choice < 1 || choice > count) {
        fprintf(stderr, "Invalid selection.\n");
        return NULL;
    }

    return names[choice - 1];
}
