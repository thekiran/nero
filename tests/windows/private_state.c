/* Native private-path contract and failure diagnostics. This creates only a
 * temporary directory under the runner's own profile and never sends traffic. */
#include "nero/private_file.h"
#include <sddl.h>
static void diagnose(char *path) {
    printf("state path: %s\n", path);
    size_t length = strlen(path);
    for (size_t i = 3; i <= length; ++i) {
        if (path[i] && path[i] != '\\' && path[i] != '/') continue;
        char saved = path[i]; path[i] = 0;
        HANDLE h = CreateFileA(path, FILE_READ_ATTRIBUTES | READ_CONTROL,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
        printf("component %s handle=%s error=%lu\n", path,
               h == INVALID_HANDLE_VALUE ? "failed" : "opened", (unsigned long)GetLastError());
        if (h != INVALID_HANDLE_VALUE) {
            BY_HANDLE_FILE_INFORMATION info;
            if (GetFileInformationByHandle(h, &info)) printf("attributes=%lu\n", (unsigned long)info.dwFileAttributes);
            PSECURITY_DESCRIPTOR descriptor = NULL;
            if (GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                NULL, NULL, NULL, NULL, &descriptor) == ERROR_SUCCESS) {
                LPSTR sddl = NULL;
                if (ConvertSecurityDescriptorToStringSecurityDescriptorA(descriptor, SDDL_REVISION_1,
                    OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &sddl, NULL)) {
                    printf("descriptor=%s\n", sddl); LocalFree(sddl);
                }
                LocalFree(descriptor);
            }
            int acl = nero_state_acl(h, i == length, 0);
            printf("ACL check=%d errno=%d windows_error=%lu\n", acl, errno, (unsigned long)GetLastError());
            CloseHandle(h);
        }
        path[i] = saved;
    }
}
int main(void) {
    const char *root = getenv("LOCALAPPDATA");
    if (!root) return 2;
    char path[NERO_STATE_PATH], parent[NERO_STATE_PATH];
    int n = snprintf(parent, sizeof(parent), "%s\\nero-state-probe-%lu", root, (unsigned long)GetCurrentProcessId());
    if (n < 0 || (size_t)n >= sizeof(parent)) return 2;
    n = snprintf(path, sizeof(path), "%s\\history", parent);
    if (n < 0 || (size_t)n >= sizeof(path)) return 2;
    PSECURITY_DESCRIPTOR descriptor = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(
        "D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;OW)",
        SDDL_REVISION_1, &descriptor, NULL)) return 2;
    SECURITY_ATTRIBUTES attributes = {sizeof(attributes), descriptor, FALSE};
    BOOL created = CreateDirectoryA(parent, &attributes);
    LocalFree(descriptor);
    if (!created) return 2;
    nero_state_dir directory = NERO_STATE_DIR_INIT;
    int result = nero_state_parent(&directory, path, 1);
    printf("parent result=%d errno=%d windows_error=%lu\n", result, errno, (unsigned long)GetLastError());
    if (result == 0) {
        int fd = nero_state_open(&directory, "history", O_RDWR | O_CREAT, 1);
        printf("file result=%d errno=%d windows_error=%lu\n", fd, errno, (unsigned long)GetLastError());
        if (fd >= 0) { _close(fd); result = 0; } else result = -1;
    }
    nero_state_close(&directory);
    if (result != 0) diagnose(parent);
    DeleteFileA(path); RemoveDirectoryA(parent);
    return result == 0 ? 0 : 1;
}
