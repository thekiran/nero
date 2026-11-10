#ifndef NERO_PRIVATE_FILE_H
#define NERO_PRIVATE_FILE_H
/* Local per-user state. Custom paths are intentional authority supplied by the
 * current user, never by a remote target. Reject elevation, traversal, shared
 * parents, and links before reading or modifying state. Keep parents pinned
 * through rotation/clear, so resolving a pathname is not a separate check/use.
 * Processes already running as this same user remain outside this boundary. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#define NERO_STATE_PATH 4096
static inline int nero_state_leaf(const char *name);
static inline int nero_state_validate_path(const char *path) {
    if (!path || !*path || strlen(path) >= NERO_STATE_PATH) { errno = EINVAL; return -1; }
    const char *p = path;
#ifdef _WIN32
    if (strlen(path) >= 3 && path[1] == ':') p += 2;
#endif
    while (*p) {
        if (*p == '/') { ++p; continue; }
#ifdef _WIN32
        if (*p == '\\') { ++p; continue; }
#endif
        const char *start = p;
        while (*p && *p != '/'
#ifdef _WIN32
               && *p != '\\'
#endif
        ) ++p;
        size_t n = (size_t)(p - start);
        char component[256];
        if (n >= sizeof(component)) { errno = ENAMETOOLONG; return -1; }
        memcpy(component, start, n); component[n] = '\0';
        if (nero_state_leaf(component) != 0) return -1;
    }
    size_t length = strlen(path);
    if (path[length - 1] == '/'
#ifdef _WIN32
        || path[length - 1] == '\\'
#endif
    ) { errno = EINVAL; return -1; }
    return 0;
}
#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#include <io.h>
typedef struct {
    char parent[NERO_STATE_PATH];
    HANDLE directories[128];
    unsigned count;
} nero_state_dir;
#define NERO_STATE_DIR_INIT { .count = 0 }
static inline void nero_state_close(nero_state_dir *dir) {
    while (dir->count) CloseHandle(dir->directories[--dir->count]);
}
static inline int nero_state_elevated(void) { return 0; }
/* Only the user, SYSTEM and Administrators may mutate a private state parent.
 * Inherit-only creator ACEs do not grant access to the directory itself. */
static inline int nero_state_acl(HANDLE h, int private_parent, int file) {
    HANDLE token;
    union { TOKEN_USER alignment; unsigned char bytes[1024]; } user_buffer;
    DWORD needed;
    PSECURITY_DESCRIPTOR descriptor = NULL;
    PSID owner = NULL;
    PACL acl = NULL;
    union { SID alignment; BYTE bytes[SECURITY_MAX_SID_SIZE]; } system_buffer, admin_buffer;
    DWORD system_size = sizeof(system_buffer.bytes), admin_size = sizeof(admin_buffer.bytes);
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return -1;
    int ok = GetTokenInformation(token, TokenUser, user_buffer.bytes, sizeof(user_buffer.bytes), &needed) != 0;
    CloseHandle(token);
    if (!ok || !CreateWellKnownSid(WinLocalSystemSid, NULL, system_buffer.bytes, &system_size) ||
        !CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, admin_buffer.bytes, &admin_size)) return -1;
    PSID user = ((TOKEN_USER *)(void *)user_buffer.bytes)->User.Sid;
    BOOL administrator = FALSE;
    (void)CheckTokenMembership(NULL, admin_buffer.bytes, &administrator);
    if (GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                        &owner, NULL, &acl, NULL, &descriptor) != ERROR_SUCCESS) return -1;
    ok = owner && (EqualSid(owner, user) ||
         (EqualSid(owner, admin_buffer.bytes) && (!file || administrator)) ||
         (!file && EqualSid(owner, system_buffer.bytes))) && acl;
    if (ok) {
        for (DWORD i = 0; i < acl->AceCount; ++i) {
            void *entry;
            if (!GetAce(acl, i, &entry)) { ok = 0; break; }
            ACE_HEADER *header = entry;
            if (header->AceFlags & INHERIT_ONLY_ACE) continue;
            if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
                /* Fail closed for unfamiliar granting ACE forms. */
                if (header->AceType != ACCESS_DENIED_ACE_TYPE) { ok = 0; break; }
                continue;
            }
            ACCESS_ALLOWED_ACE *ace = entry;
            PSID sid = &ace->SidStart;
            DWORD mutation = DELETE | WRITE_DAC | WRITE_OWNER | FILE_DELETE_CHILD |
                             GENERIC_ALL | GENERIC_WRITE;
            if (private_parent || file) mutation |= FILE_WRITE_DATA | FILE_APPEND_DATA;
            if ((ace->Mask & mutation) && !EqualSid(sid, user) &&
                !EqualSid(sid, system_buffer.bytes) && !EqualSid(sid, admin_buffer.bytes)) { ok = 0; break; }
        }
    }
    LocalFree(descriptor);
    if (!ok) errno = EACCES;
    return ok ? 0 : -1;
}
static inline int nero_state_parent(nero_state_dir *dir, const char *path, int create) {
    nero_state_close(dir);
    if (nero_state_validate_path(path) != 0 || strlen(path) < 4 ||
        !((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) ||
        path[1] != ':' || (path[2] != '/' && path[2] != '\\')) { errno = EINVAL; return -1; }
    char full[NERO_STATE_PATH];
    memcpy(full, path, strlen(path) + 1);
    for (size_t i = 2; full[i]; ++i) if (full[i] == '/') full[i] = '\\';
    char *leaf = strrchr(full, '\\');
    if (!leaf || !leaf[1]) { errno = EINVAL; return -1; }
    *leaf = '\0';
    size_t length = strlen(full);
    for (size_t i = 3; i <= length; ++i) {
        if (full[i] && full[i] != '\\') continue;
        size_t start = i;
        while (start > 3 && full[start - 1] != '\\') --start;
        size_t n = i - start;
        if (!n || (n == 1 && full[start] == '.') ||
            (n == 2 && full[start] == '.' && full[start + 1] == '.') ||
            full[i - 1] == '.' || full[i - 1] == ' ') { errno = EINVAL; goto fail; }
        char saved = full[i]; full[i] = '\0';
        if (nero_state_leaf(full + start) != 0) goto fail;
        if (strchr(full + 2, ':')) { errno = EINVAL; goto fail; }
        if (create && !CreateDirectoryA(full, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
            errno = EACCES; goto fail;
        }
        HANDLE h = CreateFileA(full, FILE_READ_ATTRIBUTES | READ_CONTROL,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (h == INVALID_HANDLE_VALUE) { errno = EACCES; goto fail; }
        BY_HANDLE_FILE_INFORMATION info;
        if (!GetFileInformationByHandle(h, &info) ||
            !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            nero_state_acl(h, i == length, 0) != 0 || dir->count == 128) {
            CloseHandle(h); errno = EACCES; goto fail;
        }
        dir->directories[dir->count++] = h;
        full[i] = saved;
    }
    memcpy(dir->parent, full, length + 1);
    return 0;
fail:
    nero_state_close(dir);
    return -1;
}
#else
#include <unistd.h>
typedef struct { int fd; } nero_state_dir;
#define NERO_STATE_DIR_INIT { .fd = -1 }
static inline void nero_state_close(nero_state_dir *dir) {
    if (dir->fd >= 0) close(dir->fd);
    dir->fd = -1;
}
static inline int nero_state_elevated(void) {
    return geteuid() != getuid() || getegid() != getgid() ||
           (geteuid() == 0 && getenv("SUDO_UID") != NULL);
}
static inline int nero_state_directory(int fd, int final) {
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode) || (st.st_uid != geteuid() && st.st_uid != 0) ||
        ((st.st_mode & 022) && !(st.st_uid == 0 && (st.st_mode & 01000))) ||
        (final && st.st_uid != geteuid() && !(st.st_uid == 0 && (st.st_mode & 01000)))) {
        errno = EACCES; return -1;
    }
    return 0;
}
static inline int nero_state_parent(nero_state_dir *dir, const char *path, int create) {
    nero_state_close(dir);
    if (nero_state_validate_path(path) != 0 || nero_state_elevated()) {
        errno = EINVAL; return -1;
    }
    char copy[NERO_STATE_PATH];
    memcpy(copy, path, strlen(path) + 1);
    char *leaf = strrchr(copy, '/');
    if (leaf) *leaf = '\0';
    else copy[0] = '\0';
    int fd = open(path[0] == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    if (nero_state_directory(fd, 0) != 0) { int error = errno; close(fd); errno = error; return -1; }
    char *part = copy;
    unsigned links = 0;
    while (*part) {
        if (*part == '/') { ++part; continue; }
        char *end = strchr(part, '/');
        if (end) *end = '\0';
        if (nero_state_leaf(part) != 0) {
            errno = EINVAL; goto fail;
        }
        /* macOS /tmp and /var are root-owned aliases. Only resolve an alias
         * owned by root in a root-owned, non-writable directory; never follow
         * a user-controlled symlink, even if its destination looks private. */
        struct stat entry;
        if (fstatat(fd, part, &entry, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(entry.st_mode)) {
            struct stat parent;
            char target[NERO_STATE_PATH], remaining[NERO_STATE_PATH];
            if (++links > 8 || entry.st_uid != 0 || fstat(fd, &parent) != 0 ||
                parent.st_uid != 0 || (parent.st_mode & 022)) { errno = EACCES; goto fail; }
            ssize_t n = readlinkat(fd, part, target, sizeof(target) - 1);
            if (n < 0) goto fail;
            if (n == 0 || (size_t)n >= sizeof(target) - 1) { errno = ENAMETOOLONG; goto fail; }
            target[n] = '\0';
            int length = snprintf(remaining, sizeof(remaining), "%s%s%s", target,
                                  end && end[1] ? "/" : "", end ? end + 1 : "");
            if (length < 0 || (size_t)length >= sizeof(remaining)) { errno = ENAMETOOLONG; goto fail; }
            if (target[0] == '/') {
                close(fd);
                fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
                if (fd < 0) return -1;
            }
            memcpy(copy, remaining, (size_t)length + 1); part = copy; continue;
        }
        if (create && mkdirat(fd, part, 0700) != 0 && errno != EEXIST) goto fail;
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) goto fail;
        if (nero_state_directory(next, 0) != 0) { int error = errno; close(next); errno = error; goto fail; }
        close(fd); fd = next;
        if (!end) break;
        part = end + 1;
    }
    if (nero_state_directory(fd, 1) != 0) goto fail;
    dir->fd = fd;
    return 0;
fail: {
    int error = errno; close(fd); errno = error; return -1;
    }
}
#endif
/* Every descriptor-relative operation takes exactly one component. Reject
 * traversal rather than normalizing it away. Absolute custom parents remain
 * supported, but cannot be reached through attacker-controlled links. */
static inline int nero_state_leaf(const char *name) {
    if (!name || !*name || strcmp(name, ".") == 0 || strcmp(name, "..") == 0 ||
        strchr(name, '/') || strchr(name, '\\') || strchr(name, ':') || strlen(name) > 255) {
        errno = EINVAL; return -1;
    }
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
        if (*p < 32 || *p == 127) { errno = EINVAL; return -1; }
#ifdef _WIN32
    size_t n = strlen(name);
    if (name[n - 1] == ' ' || name[n - 1] == '.' || strpbrk(name, "<>\"|?*")) { errno = EINVAL; return -1; }
    char base[5] = {0}; size_t i = 0;
    while (i < 4 && name[i] && name[i] != '.') { base[i] = name[i]; ++i; }
    if (_stricmp(base, "CON") == 0 || _stricmp(base, "PRN") == 0 ||
        _stricmp(base, "AUX") == 0 || _stricmp(base, "NUL") == 0 ||
        ((_strnicmp(base, "COM", 3) == 0 || _strnicmp(base, "LPT", 3) == 0) &&
         base[3] >= '1' && base[3] <= '9')) { errno = EINVAL; return -1; }
#endif
    return 0;
}
static inline const char *nero_state_basename(const char *path) {
    const char *leaf = path;
    for (const char *p = path; *p; ++p)
        if (*p == '/' || *p == '\\') leaf = p + 1;
    return leaf;
}
#ifdef _WIN32
static inline int nero_state_name(nero_state_dir *dir, const char *leaf, char *out, size_t cap) {
    if (!dir->count || nero_state_leaf(leaf) != 0) { errno = EINVAL; return -1; }
    int n = snprintf(out, cap, "%s\\%s", dir->parent, leaf);
    if (n < 0 || (size_t)n >= cap) { errno = ENAMETOOLONG; return -1; }
    return 0;
}
#endif
static inline int nero_state_open(nero_state_dir *dir, const char *leaf, int flags, int private_mode) {
    if (nero_state_leaf(leaf) != 0) return -1;
#ifdef _WIN32
    char name[NERO_STATE_PATH];
    if (nero_state_name(dir, leaf, name, sizeof(name)) != 0) return -1;
    int writing = (flags & (_O_RDWR | _O_WRONLY)) != 0;
    DWORD disposition = (flags & _O_EXCL) ? CREATE_NEW : (flags & _O_CREAT) ? OPEN_ALWAYS : OPEN_EXISTING;
    HANDLE h = CreateFileA(name, (writing ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ) | READ_CONTROL,
                          0, NULL, disposition, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        errno = error == ERROR_FILE_NOT_FOUND ? ENOENT : error == ERROR_FILE_EXISTS ? EEXIST : EACCES;
        return -1;
    }
    BY_HANDLE_FILE_INFORMATION info;
    if (GetFileType(h) != FILE_TYPE_DISK || !GetFileInformationByHandle(h, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
        info.nNumberOfLinks != 1 || nero_state_acl(h, 1, 1) != 0) {
        CloseHandle(h); errno = EACCES; return -1;
    }
    (void)private_mode;
    int fd = _open_osfhandle((intptr_t)h, (writing ? _O_RDWR : _O_RDONLY) |
                           (flags & _O_APPEND) | _O_BINARY | _O_NOINHERIT);
    if (fd < 0) CloseHandle(h);
    return fd;
#else
    if (dir->fd < 0) { errno = EACCES; return -1; }
    /* Never pass O_TRUNC: ownership/type checks must precede any mutation. */
    int fd = openat(dir->fd, leaf, (flags & ~O_TRUNC) | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        st.st_nlink != 1 || (st.st_mode & (private_mode ? 077 : 022))) {
        close(fd); errno = EACCES; return -1;
    }
    return fd;
#endif
}
static inline int nero_state_remove(nero_state_dir *dir, const char *leaf) {
    if (nero_state_leaf(leaf) != 0) return -1;
#ifdef _WIN32
    char name[NERO_STATE_PATH];
    if (nero_state_name(dir, leaf, name, sizeof(name)) != 0) return -1;
    if (DeleteFileA(name)) return 0;
    errno = GetLastError() == ERROR_FILE_NOT_FOUND ? ENOENT : EACCES; return -1;
#else
    if (dir->fd < 0) { errno = EACCES; return -1; }
    return unlinkat(dir->fd, leaf, 0);
#endif
}
static inline int nero_state_rename(nero_state_dir *dir, const char *from, const char *to) {
    if (nero_state_leaf(from) != 0 || nero_state_leaf(to) != 0) return -1;
#ifdef _WIN32
    char oldname[NERO_STATE_PATH], newname[NERO_STATE_PATH];
    if (nero_state_name(dir, from, oldname, sizeof(oldname)) != 0 ||
        nero_state_name(dir, to, newname, sizeof(newname)) != 0) return -1;
    if (MoveFileExA(oldname, newname, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 0;
    errno = EACCES; return -1;
#else
    if (dir->fd < 0) { errno = EACCES; return -1; }
    return renameat(dir->fd, from, dir->fd, to);
#endif
}
#endif
