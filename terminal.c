#include <stdio.h>     
#include <string.h>   
#include <termios.h>    
#include <unistd.h>
#include <stdlib.h> 
#include <signal.h>//--

#include "printing.h"
#include "syn_flood_powered.h"
#include "ascii.h"

/*-------------------------------*/    
#define MAX_HISTORY 256
#define MAX_COMMAND_LENGTH 256
/*-------------------------------*/   

//global
char history[MAX_HISTORY][MAX_COMMAND_LENGTH];
int history_index = 0;
int history_count = 0;
int row_location = 14;

// Terminal girişini anlık okumaya ayarlama
void disableBuffering(struct termios *old) {
    struct termios new;
    tcgetattr(STDIN_FILENO, old); // Mevcut ayarları al
    new = *old;
    new.c_lflag &= ~(ICANON | ECHO); // Canonical mode ve echo'yu kapat
    tcsetattr(STDIN_FILENO, TCSANOW, &new); // Yeni ayarları uygula
}
// Terminal ayarlarını geri yükleme
void restoreBuffering(struct termios *old) {
    tcsetattr(STDIN_FILENO, TCSANOW, old);
}

void clear_screen() {
    write(STDOUT_FILENO, "\033[H\033[J", 6);
}

int row_location_function(){
    return row_location; 
}

int terminal_code()
{
    // Set up signal handler
    struct termios old;
    char command[MAX_COMMAND_LENGTH];
    int history_pos = history_count; // Geçmişte gezinmek için

    disableBuffering(&old); // Terminal girişini anlık hale getir
    ascii_function();  
    fflush(stdout); 

    while (1) 
    {
        printf("\033[0;35m\n%s \033[0m",TERMINAL_CURSOR);
        fflush(stdout);

        int i = 0;
        memset(command, 0, sizeof(command));

        while (1) 
        {
            char ch;
            read(STDIN_FILENO, &ch, 1); // Tek karakter oku

            // Enter'a basıldıysa komutu çalıştır
            if (ch == '\n') { // Enter tuşu
                row_location += 2;
                if (i > 0) {
                    command[i] = '\0';

                    if (history_count < MAX_HISTORY) {
                        strcpy(history[history_count], command);
                        history_count++;
                    } else {
                        // History dolduğunda en eskiyi sil ve kaydır
                        for (int j = 1; j < MAX_HISTORY; j++) {
                            strcpy(history[j - 1], history[j]);
                        }
                        strcpy(history[MAX_HISTORY - 1], command);
                    }

                    history_pos = history_count; // En son girilene set et
                }
                break;
            }

            // Yön tuşlarını kontrol et
            if (ch == 27) { // Escape karakteri (yön tuşları için)
                read(STDIN_FILENO, &ch, 1); // '[' karakterini oku
                if (ch == '[') {
                    read(STDIN_FILENO, &ch, 1); // Asıl yön karakterini oku

                    if (ch == 'A') { // Yukarı tuşu

                        if (history_pos > 0) {
                            history_pos--;
                            printf("\r\033[K\033[0;35m%s \033[0m %s",TERMINAL_CURSOR,history[history_pos]);
                            fflush(stdout);
                            strcpy(command, history[history_pos]);
                            i = strlen(command);
                            continue;
                        }
                        else
                        { 
                            printf("\a"); // Uyarı sesi (Beep)
                            fflush(stdout);
                            continue;
                        }   
                    }
                    else if (ch == 'B') { // Aşağı tuşu

                        if (history_pos < history_count - 1) {
                            history_pos++;
                            printf("\r\033[K\033[0;35m%s \033[0m %s",TERMINAL_CURSOR,history[history_pos]);
                            fflush(stdout);
                            strcpy(command, history[history_pos]);
                            i = strlen(command);
                            continue;
                        }
                        else
                        { 
                            printf("\a"); // Uyarı sesi (Beep)
                            fflush(stdout);
                            continue;
                        }             
                    }
                    else if (ch == 'C') { 
                        i++;
                        //CD_COUNTER++;
                        printf("\033[C");
                        fflush(stdout);
                        continue;
                    }
                    else if (ch == 'D') { //bozuk
                        if (i > 0) 
                        { // Eğer kullanıcı en az bir karakter girdiyse
                            i--;
                            //CD_COUNTER--;
                            printf("\033[D");
                            fflush(stdout);
                            continue;
                        }
                        else
                        { 
                            printf("\a"); // Uyarı sesi (Beep)
                            fflush(stdout);
                            continue;
                        }            
                    }
                    else if(history_pos == history_count)
                    {                        
                        printf("\r\033[K\033[0;35m%s \033[0m",TERMINAL_CURSOR);
                        fflush(stdout);
                        command[0] = '\0';
                        i = 0;
                    }       
                }  
            }
            else if (ch == 127) 
            {  // Backspace
                if (i > 0)      
                { // Eğer kullanıcı en az bir karakter girdiyse
                    i--;
                    command[i] = '\0';  // Son karakteri temizle
                    printf("\b \b"); // Son karakteri sil
                    fflush(stdout);
                    continue;
                } 
                else
                { 
                    printf("\a"); // Uyarı sesi (Beep)
                    fflush(stdout);
                    continue;
                }
            }


            // Normal karakterleri ekle
            command[i++] = ch;
            printf("%c", ch);
            fflush(stdout);
        }

        //add_to_history(command); // Komutu geçmişe ekle

        // Komutları işle
        if (strstr(command, " -Sc\0") != NULL) {
            char command_for_mac[MAX_COMMAND_LENGTH];
            strncpy(command_for_mac, command, MAX_COMMAND_LENGTH - 1);
            command_for_mac[MAX_COMMAND_LENGTH - 1] = '\0';
        
            char *value = strstr(command_for_mac, "-Sc");
            if (value && strlen(value) >= 3) {
                memmove(value, value + 3, strlen(value + 3) + 1);
            }
        
            char *point = strrchr(command_for_mac, '.');
            if (point) *point = '\0';
        
            if (*command_for_mac != '\0') {  // Boş string olmasını engelle
                //network_scan(command_for_mac);
            }
        }
        else if (strstr(command, "-mode\0") != NULL) {
            char command_for_monitor_mode[MAX_COMMAND_LENGTH];
            strncpy(command_for_monitor_mode, command, MAX_COMMAND_LENGTH - 1);
            command_for_monitor_mode[MAX_COMMAND_LENGTH - 1] = '\0'; // Buffer overflow önleme
        
            char *value = strstr(command_for_monitor_mode, "-mode");
            if (value) {
                memmove(value, value + 5, strlen(value + 5) + 1);  // "-mode" çıkarıldı
        
                char *point = strrchr(command_for_monitor_mode, '.');
                if (point) *point = '\0';
        
                //packet_shot();  // Eğer parametre alıyorsa, command_for_monitor_mode geçilmeli
            }
        }
        else if (strstr(command, " -Syn") != NULL) {
            char command_for_ip[MAX_COMMAND_LENGTH];
            strncpy(command_for_ip, command, MAX_COMMAND_LENGTH);

            char *value = strstr(command_for_ip, " -Syn");
            if (value) *value = '\0';  // "-Syn" ifadesini ve sonrasını kes

            // Gereksiz nokta silme işlemi yapılmıyor

            if (*command_for_ip != '\0') {
                
                SYN_Flood(command_for_ip);
            }
        }
      
        else if (strstr(command, "history") != NULL) 
        {
            
            if (strstr(command, " -clear\0") != NULL)//düzeltilecek ---------------------
            {
                printf("done");
            }
            else
            {
                //printf("\n %d",history_count);
                for (int counter = 0; counter < history_count; counter++)
                {
                    printf("\n%s",history[counter]);
                }
            }
        } 
        else if (strstr(command, "monitor") != NULL) 
        {
            
            
        } 
        else if (strstr(command,"help") != NULL) 
        {
                row_location+=11;
                const char *commands[][2] = {
                {"<network IP> -Syn", ": Scan Port"},
                {"monitor", ": monitor-mode"},
                {"help", ": Show this help message"},
                {"help -All", ": Show this help message"},
                {"exit", ": Quit the shell"},
                {"clear", ": Clear the screen"},
                {"history", ": Show command history"},
                {"version", ": Show program version"} 
             };

            int num_commands = sizeof(commands) / sizeof(commands[0]);

            printf("\n\033[1;32mAvailable Commands:\033[0m\n");
            printf(PRINTING_LINE_1);

            for (int counter_help = 0; counter_help < num_commands; counter_help++) {
                printf("%d.\033[1;33m%-20s\033[0m %s\n",counter_help+1,commands[counter_help][0], commands[counter_help][1]);
                fflush(stdout);
            }

            printf(PRINTING_LINE_1);
        }

        else if (strstr(command, "exit") != NULL) 
        {
            restoreBuffering(&old); // Terminal ayarlarını geri yükle

            printf(EXIT_MESSAGE);

            if (strstr(command, " -All ") != NULL)
            {
                printf("\033[H\033[J");   // Ekranı temizlemek için escape kodu
                system("clear");          // Alternatif olarak clear komutu
                fflush(stdout); 

                break;
                return 0;
            }
            else
            {
                break;
                return 0;
            }
        }
        else if (strstr(command, "clear") != NULL) 
        {   
            row_location = 16;            
            terminal_code();

        } 
        else if (strstr(command, "version") != NULL) 
        {
            printf(VERSION_MESSAGE);
        }
        else if (i > 0) 
        {
            printf(UNKNOWN_COMMAND_WARNING);
        }
    }

    return 0;
}

