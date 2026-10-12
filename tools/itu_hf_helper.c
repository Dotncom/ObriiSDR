#include <stdio.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>
#endif

int itu_hf_upstream_main(int argc, char **argv);

int main(int argc, char **argv) {
#ifdef _WIN32
    wchar_t executable[32768];
    DWORD length = GetModuleFileNameW(NULL, executable, 32768);
    if (!length || length >= 32768) return 2;
    while (length && executable[length - 1] != L'\\') --length;
    if (!length) return 2;
    executable[length] = L'\0';
    /* Upstream loads its DLLs by basename. Exclude the private job cwd and PATH. */
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS)
        || !AddDllDirectory(executable)) {
        fputs("Cannot restrict ITU DLL search\n", stderr);
        return 2;
    }
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
#else
    char executable[PATH_MAX], library[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (length <= 0 || length >= sizeof(executable) - 1) return 2;
    executable[length] = '\0';
    char *slash = strrchr(executable, '/');
    if (!slash) return 2;
    *slash = '\0';
    /* Preload exact sibling SONAMEs; upstream dlopen(basename) reuses them. */
    const char *names[] = {"libp372.so", "libp533.so"};
    for (int i = 0; i < 2; ++i) {
        if (snprintf(library, sizeof(library), "%s/%s", executable, names[i]) >= sizeof(library)) return 2;
        if (!dlopen(library, RTLD_NOW | RTLD_LOCAL)) {
            fprintf(stderr, "Cannot load sibling ITU library: %s\n", dlerror());
            return 2;
        }
    }
    setpriority(PRIO_PROCESS, 0, 10);
#endif
    return itu_hf_upstream_main(argc, argv);
}
