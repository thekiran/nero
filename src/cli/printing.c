/*the functions is a printing*/
/*---------------------------*/
#define ESSID_1 "\x1b[1mESSID\x1b[0m"
#define PURdBm_1 "\x1b[1mPUR (dBm)\x1b[0m"
#define ENCCIPHER_1 "\x1b[1mENC CIPHER\x1b[0m"
#define CH_1 "\x1b[1mCH\x1b[0m"
#define HB_1 "\x1b[1mHB (Mbps)\x1b[0m"
#define Beacons_1 "\x1b[1mBeacons\x1b[0m"
#define Data_1 "\x1b[1m#Data, #/s\x1b[0m"
/*---------------------------*/
#define ClientMAC_2 "\x1b[1mClient MAC\x1b[0m"
#define AssociatedBSSID_2 "\x1b[1mAssociated BSSID\x1b[0m"
#define AssociatedAP_2 "\x1b[1mAssociated AP\x1b[0m"
#define Signal_2 "\x1b[1mSignal\x1b[0m"
#define DataRate_2 "\x1b[1mData Rate\x1b[0m"
#define PacketCount_2 "\x1b[1mPacket Count\x1b[0m"
#define LastSeen_2 "\x1b[1mLast Seen\x1b[0m"
/*---------------------------*/

//monitor_mode_scan.h 
#define RF_KILL_ERROR "RF-kill command failed.\n"
#define ADAPTER_CLOSE_ERROR "Failed to close adapter: %s\n"
#define MONITOR_MODE_ERROR "Failed to enable monitor mode: %s\n"
#define ADAPTER_OPEN_ERROR "Failed to open adapter: %s\n"
#define MEMORY_ALLOCATION_ERROR "Memory allocation failed"
#define CHANNEL_CHANGE_ERROR "Channel change failed: %s\n"
#define CHANNEL_CHANGE_STOPPED "Channel switching stopped.\n"
#define ADAPTER_NAME_ERROR "Failed to retrieve network adapter name"
#define THREAD_CREATION_ERROR "Failed to create channel hopping thread.\n"
#define PACKET_CAPTURE_ERROR "Packet capture failed: %s\n"

//terminal.h +
#define EXIT_MESSAGE "\n\x1B[35mExiting...\x1B[0m\n"
#define UNKNOWN_COMMAND_WARNING "\a\n[\x1B[34mWARNING\x1B[0m] Unknown command! Available commands: 'help'"

//interface.h +
#define MEMORY_ALLOCATION_FAILED "[\x1B[34mERROR\x1B[0m] Memory allocation failed!\n"
#define CURL_INIT_WARNING "[\x1B[33mWARNING\x1B[0m] curl could not be initialized. Continuing without network request!\n"

//network_scan.h +
#define FILE_OPEN_ERROR "Failed to open file"
#define CURL_ERROR "CURL error: %s\n"
#define CURL_INIT_FAILED "Failed to initialize CURL.\n"
#define POPEN_FAILED "popen failed\n"
#define NETWORK_SC_ERROR "\033[1;31m[ERROR]\033[0m : <network IP> -Sc\n"

#ifdef UNICODE_SUPPORT

#define TERMINAL_CURSOR "\xE2\x96\xBA"
#define TERMINAL_CURSOR_1 ---

#define PRINTING_LINE "\033[33m\033[1m─────────────────────────────────────────────────────────────\n\033[0m"
#define PRINTING_LINE_1 "\033[1;34m─────────────────────────────────────────────────────────────\033[0m\n"
#define PRINTING_LINE_2 "\n\033[1m\b\b\b─────────────────────────────────────────────────────────────\033[0m\n"

#define PRINTING_LINE_3_FOR_TABLE_1 "┌───────────────────────┬──────────────┬──────────────┬────────┬──────────────┬──────────────┬──────────────┐\n"
#define PRINTING_LINE_3_FOR_TABLE_2 "│ %-29.29s │ %-20.20s │ %-20.20s │ %-14.14s │ %-20.20s │ %-20.20s │ %-20.20s │\n", \
                                     ESSID_1, PURdBm_1, ENCCIPHER_1, CH_1, HB_1, Beacons_1, Data_1
#define PRINTING_LINE_3_FOR_TABLE_3 "├───────────────────────┼──────────────┼──────────────┼────────┼──────────────┼──────────────┼──────────────┤\n"
#define PRINTING_LINE_3_FOR_TABLE_4 "└───────────────────────┴──────────────┴──────────────┴────────┴──────────────┴──────────────┴──────────────┘\n"

#define PR_1 "│ %-21.21s │ %-12s │ %-12s │ %-6s │ %-12s │ %-12d │ %-12d │\n", \
             display_ssid, networks[i].signal, networks[i].enc_cipher, networks[i].channel, networks[i].hb, networks[i].beacon_count, networks[i].data_count

#define PRINTING_LINE_3_FOR_TABLE_1_FOUND_DIVICE "┌───────────────────┬───────────────────┬──────────────────────┬────────┬──────────────┬──────────────┬───────────┐\n"
#define PRINTING_LINE_3_FOR_TABLE_2_FOUND_DIVICE "│ %-25.25s │ %-25.25s │ %-28.28s │ %-14.14s │ %-20.20s │ %-20.20s │ %-17.17s │\n", \
                                                 ClientMAC_2, AssociatedBSSID_2, AssociatedAP_2, Signal_2, DataRate_2, PacketCount_2, LastSeen_2
#define PRINTING_LINE_3_FOR_TABLE_3_FOUND_DIVICE "├───────────────────┼───────────────────┼──────────────────────┼────────┼──────────────┼──────────────┼───────────┤\n"
#define PRINTING_LINE_3_FOR_TABLE_4_FOUND_DIVICE "└───────────────────┴───────────────────┴──────────────────────┴────────┴──────────────┴──────────────┴───────────┘\n"

#define PR_2 "│ %-16s │ %-16s │ %-20.20s │ %-6s │ %-12s │ %-12d │ %-9s │\n", \
             clients[i].client_mac, clients[i].associated_bssid, display_ap_ssid, clients[i].signal, clients[i].hb, clients[i].packet_count, clients[i].last_seen

#else

#define TERMINAL_CURSOR ">"
#define TERMINAL_CURSOR_1 ---

#define PRINTING_LINE "\033[33m\033[1m+--------------------------------------------------------+\n\033[0m"
#define PRINTING_LINE_1 "\033[1;34m------------------------------------------------------------------\033[0m\n"
#define PRINTING_LINE_2 "\033[1m\b\b\b+--------------------------------------------------------+\033[0m\n"

#define PRINTING_LINE_3_FOR_TABLE_1 "+----------------------+--------------+--------------+--------+--------------+--------------+--------------+\n"
#define PRINTING_LINE_3_FOR_TABLE_2 "| %-29.29s | %-20.20s | %-20.20s | %-14.14s | %-20.20s | %-20.20s | %-20.20s |\n", \
                                     ESSID_1, PURdBm_1, ENCCIPHER_1, CH_1, HB_1, Beacons_1, Data_1
#define PRINTING_LINE_3_FOR_TABLE_3 "+----------------------+--------------+--------------+--------+--------------+--------------+--------------+\n"
#define PRINTING_LINE_3_FOR_TABLE_4 "+----------------------+--------------+--------------+--------+--------------+--------------+--------------+\n"

#define PR_1 "| %-21.21s | %-12s | %-12s | %-6s | %-12s | %-12d | %-12d |\n", \
             display_ssid, networks[i].signal, networks[i].enc_cipher, networks[i].channel, networks[i].hb, networks[i].beacon_count, networks[i].data_count

#define PRINTING_LINE_3_FOR_TABLE_1_FOUND_DIVICE "+------------------+------------------+----------------------+--------+--------------+--------------+-----------+\n"
#define PRINTING_LINE_3_FOR_TABLE_2_FOUND_DIVICE "| %-25.25s | %-25.25s | %-28.28s | %-14.14s | %-20.20s | %-20.20s | %-17.17s |\n", \
                                                 ClientMAC_2, AssociatedBSSID_2, AssociatedAP_2, Signal_2, DataRate_2, PacketCount_2, LastSeen_2
#define PRINTING_LINE_3_FOR_TABLE_3_FOUND_DIVICE "+------------------+------------------+----------------------+--------+--------------+--------------+-----------+\n"
#define PRINTING_LINE_3_FOR_TABLE_4_FOUND_DIVICE "+------------------+------------------+----------------------+--------+--------------+--------------+-----------+\n"

#define PR_2 "| %-16s | %-16s | %-20.20s | %-6s | %-12s | %-12d | %-9s |\n", \
             clients[i].client_mac, clients[i].associated_bssid, display_ap_ssid, clients[i].signal, clients[i].hb, clients[i].packet_count, clients[i].last_seen

