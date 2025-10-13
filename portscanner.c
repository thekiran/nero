#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <errno.h>
#include <termios.h> // Terminal kontrolü için eklendi

/* YÖN TUŞLARI İLE SEÇİM İÇİN GEREKLİ YARDIMCI FONKSİYONLAR */

// Orijinal terminal ayarlarını saklamak için bir yapı
static struct termios orig_termios;

// Orijinal terminal ayarlarını geri yükleyen fonksiyon
void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

// Terminali tuş vuruşlarını anında yakalamak için "raw" moda alan fonksiyon
void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    // Program sonlandığında terminal ayarlarının geri yüklenmesini garantile
    atexit(disable_raw_mode);

    struct termios raw = orig_termios;
    // ECHO (yazılanı gösterme) ve ICANON (satır tabanlı girişi) kapat
    raw.c_lflag &= ~(ECHO | ICANON);
    
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

// --- BU FONKSİYON TİTREMEYİ ÖNLEMEK İÇİN GÜNCELLENDİ ---
int select_port_menu(int *open_ports, int count) {
    if (count == 0) {
        printf("[!] Seçilecek açık port bulunamadı.\n");
        return 0; // Seçilecek port yoksa 0 döndür
    }

    enable_raw_mode();

    int current_selection = 0;
    char c;
    
    // Menüyü ilk kez ekrana çiz
    for (int i = 0; i < count; i++) {
        if (i == 0) { // İlk eleman seçili başlasın
            printf(" > \033[7m%d\033[0m\n", open_ports[i]);
        } else {
            printf("   %d\n", open_ports[i]);
        }
    }

    while (1) {
        // İmleci listenin en başına geri taşı
        // \x1b[<N>A komutu imleci N satır yukarı taşır.
        printf("\x1b[%dA", count);

        // Menüyü, mevcut satırların üzerine yazarak yeniden çiz
        for (int i = 0; i < count; i++) {
            // Satırın geri kalanını temizle (\x1b[K), bu kalıntıları önler
            printf("\x1b[K"); 
            if (i == current_selection) {
                printf(" > \033[7m%d\033[0m\n", open_ports[i]); // Seçili olanı ters renkle göster
            } else {
                printf("   %d\n", open_ports[i]);
            }
        }

        read(STDIN_FILENO, &c, 1);

        if (c == '\x1b') { // Escape karakteri, yön tuşu olabilir
            char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) != 1) continue;
            if (read(STDIN_FILENO, &seq[1], 1) != 1) continue;

            if (seq[0] == '[') {
                if (seq[1] == 'A') { // Yukarı ok
                    if (current_selection > 0) current_selection--;
                } else if (seq[1] == 'B') { // Aşağı ok
                    if (current_selection < count - 1) current_selection++;
                }
            }
        } else if (c == '\n' || c == '\r') { // Enter tuşu
            break; // Seçim yapıldı, döngüden çık
        }
    }
    
    // Menü seçimi bittikten sonra menünün kapladığı alanı temizle
    // Önce imleci menünün başına taşı
    printf("\x1b[%dA", count);
    // Sonra imleçten ekranın sonuna kadar olan bölümü temizle
    printf("\x1b[0J");

    disable_raw_mode(); // Normal terminal moduna geri dön
    
    // En son seçilen portu temiz ekrana yazdır
    printf("Seçilen Port: %d\n", open_ports[current_selection]);

    return open_ports[current_selection];
}


/* KODUNUZUN ORİJİNAL HALİ (DEĞİŞTİRİLMEDİ) */

int portscanner(const char *target_ip) {
    struct sockaddr_in addr;
    int sockfd;
    int port;
    
    // Açık portları saklamak için bir dizi ve sayaç
    int *open_ports_list = NULL;
    int port_counter = 0;

    // IP adresinin geçerli olup olmadığını kontrol et
    if (inet_pton(AF_INET, target_ip, &addr.sin_addr) <= 0) {
        fprintf(stderr, "[-] Geçersiz IP adresi: %s\n", target_ip);
        return 1;
    }

    addr.sin_family = AF_INET;

    printf("\n[*] Taranıyor: %s\n", target_ip);

    for (port = 1; port <= 1000; port++) { //65535
        sockfd = socket(AF_INET, SOCK_STREAM, 0);

        if (sockfd < 0) {
            perror("Socket hatası");
            free(open_ports_list); // Hata durumunda belleği boşalt
            return 1;
        }

        addr.sin_port = htons(port);

        // Timeout ayarı (0.5 saniye)
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 500000;

        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            
            // Açık portu listeye ekle
            port_counter++;
            open_ports_list = realloc(open_ports_list, port_counter * sizeof(int));
            if (open_ports_list == NULL) {
                perror("Bellek hatası");
                exit(1);
            }
            open_ports_list[port_counter - 1] = port;
        }

        close(sockfd);
    }

    // --- ESKİ scanf DÖNGÜSÜNÜN YERİNE YENİ MENÜ FONKSİYONU GELDİ ---
    int selected_port = select_port_menu(open_ports_list, port_counter);

    // Kullanılan belleği serbest bırak
    free(open_ports_list);

    return selected_port; // Kullanıcının seçtiği portu döndür
}