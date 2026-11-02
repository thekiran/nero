#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>

#define MAX_INTERFACES 128
#define MAX_NAME_LEN 64

// ANSI renk kodları
enum {
    COLOR_RESET = 0,
    COLOR_BLUE = 34,    // Ethernet
    COLOR_GREEN = 32,   // Wireless
    COLOR_YELLOW = 33,  // Loopback
    COLOR_RED = 31      // Diğer
};

// Arayüz tipini sysfs'ten oku
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

// Tipin okunabilir adı
static const char* type_str(int type) {
    switch (type) {
        case 1:   return "Ethernet";
        case 801: return "Wireless";
        case 772: return "Loopback";
        default:  return "Unknown";
    }
}

// Renkli satır yazdırma fonksiyonu
static void print_colored(int idx, const char *name, const char *desc, int type) {
    int color;
    if (type == 1)        color = COLOR_BLUE;   // Ethernet
    else if (type == 801) color = COLOR_GREEN;  // Wireless
    else if (type == 772) color = COLOR_YELLOW; // Loopback
    else                  color = COLOR_RED;    // Diğer

    printf("%2d) \033[1;%dm%-15s\033[0m (%s)\n", idx, color, name, desc);
}

// Arayüzleri listele ve kullanıcıdan seçim al
const char *list_and_select_interface() {
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
        fprintf(stderr, "Ağ arayüzü bulunamadı.\n");
        return NULL;
    }

    // Ekranı temizle
    printf("\033[H\033[J");
    printf("\033[1;31mMevcut Ağ Arayüzleri:\033[0m\n\n");

    for (int i = 0; i < count; ++i) {
        print_colored(i + 1, names[i], type_str(types[i]), types[i]);
    }

    int choice = 0;
    printf("\nArayüz seçin [1-%d]: ", count);
    if (scanf("%d", &choice) != 1 || choice < 1 || choice > count) {
        fprintf(stderr, "Geçersiz seçim.\n");
        return NULL;
    }

    return names[choice - 1];
}
