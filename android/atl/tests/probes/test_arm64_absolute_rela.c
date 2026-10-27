/* Build on ARM64:
 * cc -shared -nostdlib -Wl,--hash-style=both arm64-absolute-rela.S -o rela-fixture.so
 * cc test_arm64_absolute_rela.c -Wl,--export-dynamic -ldl_bio -o test-arm64-rela
 * ./test-arm64-rela ./rela-fixture.so
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <link.h>

extern void *bionic_dlopen(const char *, int);
extern void *bionic_dlsym(void *, const char *);
extern char *bionic_dlerror(void);
extern void dl_parse_library_path(const char *, char *);
extern struct r_debug *_r_debug_ptr;

int bionic_fixture_android_marker = 123;

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    /* Standalone probes must provide the executable's debugger link map. */
    _r_debug_ptr = &_r_debug;
    char *path = realpath(argv[1], NULL);
    if (!path) return 2;
    char *slash = strrchr(path, '/');
    *slash = '\0';
    dl_parse_library_path(path, ":");
    *slash = '/';
    void *library = bionic_dlopen(path, 2);
    free(path);
    if (!library) {
        fprintf(stderr, "load failed: %s\n", bionic_dlerror());
        return 1;
    }
    void **slot = bionic_dlsym(library, "fixture_slot");
    void *target = bionic_dlsym(library, "fixture_target");
    unsigned char **data_slot = bionic_dlsym(library, "fixture_data_slot");
    unsigned char *data = bionic_dlsym(library, "fixture_data");
    if (!slot || !target || !data_slot || !data) return 1;
    if (*slot != target || *data_slot != data + 7) {
        fprintf(stderr, "ABS64 mismatch: function %p expected %p; data %p expected %p\n",
                *slot, target, (void *)*data_slot, (void *)(data + 7));
        return 1;
    }
    if (((int (*)(void))*slot)() != 42 || **data_slot != 7) return 1;
    puts("ARM64_ABS64_RELA_OK function=42 explicit_addend=7");
    return 0;
}
