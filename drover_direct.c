/*
 * macOS Direct-mode port of Discord Drover's UDP prelude.
 *
 * On the first 74-byte UDP send on a socket:
 *   1. send drover-packet.bin if present (re-read each time)
 *   2. send a 1-byte packet of 0x00
 *   3. send a 1-byte packet of 0x01
 *   4. wait 50 ms
 *   5. send Discord's original packet unchanged
 *
 * Chromium renderer/GPU helpers are left alone. Hooking send() or
 * forcing DYLD_INSERT_LIBRARIES into those processes crashes Discord.
 */
#define _DARWIN_C_SOURCE

#include <crt_externs.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef DYLD_INTERPOSE
/* Non-const pointers keep this in __DATA,__interpose. const fields get
 * moved to __DATA_CONST, which dyld does not treat as interpose. */
#define DYLD_INTERPOSE(_replacement, _replacee)                                 \
    __attribute__((used)) static struct {                                       \
        void *replacement;                                                      \
        void *replacee;                                                         \
    } _interpose_##_replacee __attribute__((section("__DATA,__interpose"))) = { \
        (void *)(unsigned long)(_replacement),                                  \
        (void *)(unsigned long)(_replacee)                                      \
    }
#endif

#define PACKET_FILENAME "drover-packet.bin"
#define FIRST_UDP_LEN 74
#define PRELUDE_SLEEP_US (50 * 1000)
#define MAX_PACKET_BYTES 65507
#define FD_TABLE_SIZE 65536

typedef ssize_t (*sendto_fn)(
    int,
    const void *,
    size_t,
    int,
    const struct sockaddr *,
    socklen_t
);
typedef ssize_t (*sendmsg_fn)(int, const struct msghdr *, int);
typedef ssize_t (*send_fn)(int, const void *, size_t, int);
typedef int (*socket_fn)(int, int, int);

static sendto_fn real_sendto;
static sendmsg_fn real_sendmsg;
static send_fn real_send;
static socket_fn real_socket;
static int udp_log_remaining = 48;

static pthread_once_t resolve_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;

static uint8_t injected_fds[FD_TABLE_SIZE];
static char dylib_path[4096];
static char packet_path[4096];
static bool debug_enabled;
static bool verbose_udp;
static bool hooks_active;
static bool renderer_hooks;
static FILE *log_file;
static volatile int injecting;

static void drover_log(const char *fmt, ...) {
    va_list args;
    va_list args_copy;

    va_start(args, fmt);
    va_copy(args_copy, args);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);

    if (log_file != NULL) {
        vfprintf(log_file, fmt, args_copy);
        fputc('\n', log_file);
        fflush(log_file);
    }
    va_end(args_copy);
}

static bool argv_has_prefix(const char *value, const char *prefix) {
    return value != NULL && strncmp(value, prefix, strlen(prefix)) == 0;
}

static bool should_activate_hooks(void) {
    int *argc_ptr = _NSGetArgc();
    char ***argv_ptr = _NSGetArgv();
    if (argc_ptr == NULL || argv_ptr == NULL || *argv_ptr == NULL) {
        return true;
    }

    int argc = *argc_ptr;
    char **argv = *argv_ptr;

    /* discord_voice.node is loaded in the Electron renderer, not main. */
    for (int i = 1; i < argc; i++) {
        if (argv[i] == NULL) {
            continue;
        }
        if (argv_has_prefix(argv[i], "--type=renderer")) {
            return true;
        }
        if (argv_has_prefix(argv[i], "--type=")) {
            return false;
        }
    }

    if (argv[0] != NULL) {
        if (strstr(argv[0], "GPU") != NULL || strstr(argv[0], "Plugin") != NULL) {
            return false;
        }
    }

    return true;
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static ssize_t kernel_sendto(
    int fd,
    const void *buffer,
    size_t length,
    int flags,
    const struct sockaddr *destination,
    socklen_t destination_length
) {
    return syscall(
        SYS_sendto_nocancel,
        fd,
        buffer,
        length,
        flags,
        destination,
        destination_length
    );
}

static ssize_t kernel_sendmsg(int fd, const struct msghdr *message, int flags) {
    return syscall(SYS_sendmsg_nocancel, fd, message, flags);
}

static ssize_t kernel_send(int fd, const void *buffer, size_t length, int flags) {
    return kernel_sendto(fd, buffer, length, flags, NULL, 0);
}

static int kernel_socket(int domain, int type, int protocol) {
    return (int)syscall(SYS_socket, domain, type, protocol);
}
#pragma clang diagnostic pop

static void resolve_reals(void) {
    real_sendto = kernel_sendto;
    real_sendmsg = kernel_sendmsg;
    real_send = kernel_send;
    real_socket = kernel_socket;
}

static ssize_t send_as_sendto(int fd, const void *buffer, size_t length, int flags) {
    if (real_sendto == NULL) {
        return -1;
    }
    return real_sendto(fd, buffer, length, flags, NULL, 0);
}

static bool path_is_file(const char *path) {
    struct stat info;

    if (path == NULL || path[0] == '\0') {
        return false;
    }
    return stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

static void join_dir_file(char *out, size_t out_size, const char *dir, const char *file) {
    snprintf(out, out_size, "%s/%s", dir, file);
}

static void directory_of(const char *path, char *out, size_t out_size) {
    const char *slash = strrchr(path, '/');

    if (slash == NULL || slash == path) {
        snprintf(out, out_size, ".");
        return;
    }

    size_t length = (size_t)(slash - path);
    if (length >= out_size) {
        length = out_size - 1;
    }
    memcpy(out, path, length);
    out[length] = '\0';
}

static bool try_packet_candidate(const char *path) {
    if (!path_is_file(path)) {
        return false;
    }
    snprintf(packet_path, sizeof(packet_path), "%s", path);
    return true;
}

static void locate_paths(void) {
    Dl_info info;
    char dir[4096];
    char candidate[4096];
    const char *env_packet = getenv("DROVER_PACKET_PATH");
    const char *env_dylib = getenv("DYLD_INSERT_LIBRARIES");

    memset(&info, 0, sizeof(info));
    if (dladdr((void *)locate_paths, &info) != 0 && info.dli_fname != NULL) {
        snprintf(dylib_path, sizeof(dylib_path), "%s", info.dli_fname);
    } else if (env_dylib != NULL && env_dylib[0] != '\0') {
        const char *colon = strchr(env_dylib, ':');
        size_t length = colon != NULL ? (size_t)(colon - env_dylib) : strlen(env_dylib);
        if (length >= sizeof(dylib_path)) {
            length = sizeof(dylib_path) - 1;
        }
        memcpy(dylib_path, env_dylib, length);
        dylib_path[length] = '\0';
    }

    if (env_packet != NULL && try_packet_candidate(env_packet)) {
        return;
    }

    if (dylib_path[0] != '\0') {
        directory_of(dylib_path, dir, sizeof(dir));
        join_dir_file(candidate, sizeof(candidate), dir, PACKET_FILENAME);
        if (try_packet_candidate(candidate)) {
            return;
        }
        /* Linked dylib lives in Contents/Frameworks; packet is in Contents/Resources. */
        snprintf(candidate, sizeof(candidate), "%s/../Resources/%s", dir, PACKET_FILENAME);
        if (try_packet_candidate(candidate)) {
            return;
        }
    }

    uint32_t executable_size = sizeof(candidate);
    if (_NSGetExecutablePath(candidate, &executable_size) == 0) {
        directory_of(candidate, dir, sizeof(dir));
        join_dir_file(candidate, sizeof(candidate), dir, PACKET_FILENAME);
        try_packet_candidate(candidate);
    }
}

static uint8_t *read_file_bytes(const char *path, size_t *length_out) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }

    long size = ftell(file);
    if (size <= 0 || size > MAX_PACKET_BYTES) {
        fclose(file);
        return NULL;
    }
    rewind(file);

    uint8_t *buffer = malloc((size_t)size);
    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }

    size_t read_bytes = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    if (read_bytes != (size_t)size) {
        free(buffer);
        return NULL;
    }

    *length_out = read_bytes;
    return buffer;
}

static void reset_injected_fd(int fd) {
    if (fd < 0 || fd >= FD_TABLE_SIZE) {
        return;
    }
    pthread_mutex_lock(&state_lock);
    injected_fds[fd] = 0;
    pthread_mutex_unlock(&state_lock);
}

static bool is_udp_socket(int fd) {
    int type = 0;
    socklen_t length = sizeof(type);

    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length) != 0) {
        return false;
    }
    return type == SOCK_DGRAM;
}

static bool is_inet_udp_socket(int fd) {
    struct sockaddr_storage local;
    socklen_t local_length = sizeof(local);

    if (!is_udp_socket(fd)) {
        return false;
    }
    memset(&local, 0, sizeof(local));
    if (getsockname(fd, (struct sockaddr *)&local, &local_length) != 0) {
        return false;
    }
    return local.ss_family == AF_INET || local.ss_family == AF_INET6;
}

static bool consume_voice_handshake(int fd, size_t length) {
    if (!hooks_active || fd < 0 || fd >= FD_TABLE_SIZE || length == 0) {
        return false;
    }
    if (!is_inet_udp_socket(fd)) {
        return false;
    }
    /* Main process also sees DNS/QUIC; only the classic 74-byte discovery
     * packet is safe there. The renderer owns discord_voice.node, so the
     * first IP UDP datagram on a socket is the voice handshake. */
    if (!renderer_hooks && length != FIRST_UDP_LEN) {
        return false;
    }

    bool first = false;
    pthread_mutex_lock(&state_lock);
    if (!injected_fds[fd]) {
        injected_fds[fd] = 1;
        first = true;
    }
    pthread_mutex_unlock(&state_lock);
    return first;
}

static ssize_t call_out(
    int fd,
    const void *buffer,
    size_t length,
    int flags,
    const struct sockaddr *destination,
    socklen_t destination_length
) {
    if (destination != NULL && destination_length > 0 && real_sendto != NULL) {
        return real_sendto(fd, buffer, length, flags, destination, destination_length);
    }
    if (real_send != NULL) {
        return real_send(fd, buffer, length, flags);
    }
    if (real_sendto != NULL) {
        return real_sendto(fd, buffer, length, flags, NULL, 0);
    }
    return -1;
}

static void send_prelude(
    int fd,
    const struct sockaddr *destination,
    socklen_t destination_length
) {
    locate_paths();
    injecting = 1;

    if (packet_path[0] != '\0' && path_is_file(packet_path)) {
        size_t packet_length = 0;
        uint8_t *packet_data = read_file_bytes(packet_path, &packet_length);
        if (packet_data != NULL && packet_length > 0) {
            drover_log("[drover] sending %zu-byte %s on fd %d", packet_length, PACKET_FILENAME, fd);
            call_out(fd, packet_data, packet_length, 0, destination, destination_length);
        }
        free(packet_data);
    } else {
        drover_log("[drover] %s not found; built-in prelude only", PACKET_FILENAME);
    }

    unsigned char zero = 0;
    unsigned char one = 1;
    call_out(fd, &zero, sizeof(zero), 0, destination, destination_length);
    call_out(fd, &one, sizeof(one), 0, destination, destination_length);
    injecting = 0;
    usleep(PRELUDE_SLEEP_US);
}

static void maybe_inject(
    int fd,
    size_t length,
    const struct sockaddr *destination,
    socklen_t destination_length
) {
    if (!hooks_active || injecting) {
        return;
    }

    bool udp = is_udp_socket(fd);
    if (udp) {
        drover_log("[drover] udp send fd=%d len=%zu", fd, length);
    } else if (udp_log_remaining > 0) {
        udp_log_remaining--;
        drover_log("[drover] send fd=%d len=%zu udp=0", fd, length);
    }

    if (!consume_voice_handshake(fd, length)) {
        return;
    }

    drover_log("[drover] first UDP send on fd %d len=%zu", fd, length);
    send_prelude(fd, destination, destination_length);
}

static size_t msg_payload_length(const struct msghdr *message) {
    size_t total = 0;

    if (message == NULL || message->msg_iov == NULL) {
        return 0;
    }

    for (int i = 0; i < (int)message->msg_iovlen; i++) {
        total += message->msg_iov[i].iov_len;
    }
    return total;
}

static ssize_t drover_sendto(
    int,
    const void *,
    size_t,
    int,
    const struct sockaddr *,
    socklen_t
);
static ssize_t drover_sendmsg(int, const struct msghdr *, int);
static ssize_t drover_send(int, const void *, size_t, int);

#ifdef __LP64__
typedef struct mach_header_64 macho_header;
typedef struct segment_command_64 macho_segment;
typedef struct nlist_64 macho_nlist;
#define LC_SEGMENT_KIND LC_SEGMENT_64
#else
typedef struct mach_header macho_header;
typedef struct segment_command macho_segment;
typedef struct nlist macho_nlist;
#define LC_SEGMENT_KIND LC_SEGMENT
#endif

static bool symbol_is_send_family(const char *name) {
    return strcmp(name, "_sendto") == 0 ||
           strcmp(name, "_sendmsg") == 0 ||
           strcmp(name, "_send") == 0 ||
           strcmp(name, "_sendto$NOCANCEL") == 0 ||
           strcmp(name, "_sendmsg$NOCANCEL") == 0 ||
           strcmp(name, "_send$NOCANCEL") == 0;
}

static void *replacement_for_symbol(const char *name) {
    if (strstr(name, "sendmsg") != NULL) {
        return (void *)(uintptr_t)drover_sendmsg;
    }
    if (strstr(name, "sendto") != NULL) {
        return (void *)(uintptr_t)drover_sendto;
    }
    return (void *)(uintptr_t)drover_send;
}

static void rebind_image(const struct mach_header *header, intptr_t slide) {
    Dl_info image_info;
    memset(&image_info, 0, sizeof(image_info));
    if (dladdr(header, &image_info) != 0 && image_info.dli_fname != NULL) {
        if (strstr(image_info.dli_fname, "drover_direct") != NULL ||
            strstr(image_info.dli_fname, "/libsystem_") != NULL ||
            strstr(image_info.dli_fname, "/libSystem") != NULL) {
            return;
        }
    }

    const macho_header *mh = (const macho_header *)header;
    const uint8_t *cursor = (const uint8_t *)header + sizeof(*mh);
    macho_segment *linkedit = NULL;
    struct symtab_command *symtab_cmd = NULL;
    struct dysymtab_command *dysymtab_cmd = NULL;
    bool chained_fixups = false;

    for (uint32_t i = 0; i < mh->ncmds; i++) {
        const struct load_command *command = (const struct load_command *)cursor;
        if (command->cmd == LC_SEGMENT_KIND) {
            macho_segment *segment = (macho_segment *)cursor;
            if (strcmp(segment->segname, SEG_LINKEDIT) == 0) {
                linkedit = segment;
            }
        } else if (command->cmd == LC_SYMTAB) {
            symtab_cmd = (struct symtab_command *)cursor;
        } else if (command->cmd == LC_DYSYMTAB) {
            dysymtab_cmd = (struct dysymtab_command *)cursor;
        } else if (command->cmd == LC_DYLD_CHAINED_FIXUPS) {
            chained_fixups = true;
        }
        cursor += command->cmdsize;
    }
    if (chained_fixups || linkedit == NULL || symtab_cmd == NULL || dysymtab_cmd == NULL) {
        return;
    }

    uintptr_t linkedit_base = (uintptr_t)slide + linkedit->vmaddr - linkedit->fileoff;
    macho_nlist *symtab = (macho_nlist *)(linkedit_base + symtab_cmd->symoff);
    char *strtab = (char *)(linkedit_base + symtab_cmd->stroff);
    uint32_t *indirect = (uint32_t *)(linkedit_base + dysymtab_cmd->indirectsymoff);

    cursor = (const uint8_t *)header + sizeof(*mh);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        const struct load_command *command = (const struct load_command *)cursor;
        if (command->cmd == LC_SEGMENT_KIND) {
            macho_segment *segment = (macho_segment *)cursor;
            struct section_64 *sections = (struct section_64 *)(cursor + sizeof(*segment));
#ifndef __LP64__
            struct section *sections32 = (struct section *)(cursor + sizeof(*segment));
#endif
            uint32_t section_count = segment->nsects;
            for (uint32_t s = 0; s < section_count; s++) {
#ifdef __LP64__
                struct section_64 *section = &sections[s];
                uint32_t type = section->flags & SECTION_TYPE;
                if (type != S_LAZY_SYMBOL_POINTERS && type != S_NON_LAZY_SYMBOL_POINTERS) {
                    continue;
                }
                uint32_t pointer_count = (uint32_t)(section->size / sizeof(void *));
                void **bindings = (void **)((uintptr_t)slide + section->addr);
                uint32_t indirect_offset = section->reserved1;
                vm_protect(
                    mach_task_self(),
                    (vm_address_t)bindings,
                    (vm_size_t)section->size,
                    false,
                    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY
                );
#else
                struct section *section = &sections32[s];
                uint32_t type = section->flags & SECTION_TYPE;
                if (type != S_LAZY_SYMBOL_POINTERS && type != S_NON_LAZY_SYMBOL_POINTERS) {
                    continue;
                }
                uint32_t pointer_count = (uint32_t)(section->size / sizeof(void *));
                void **bindings = (void **)((uintptr_t)slide + section->addr);
                uint32_t indirect_offset = section->reserved1;
                vm_protect(
                    mach_task_self(),
                    (vm_address_t)bindings,
                    (vm_size_t)section->size,
                    false,
                    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY
                );
#endif
                for (uint32_t p = 0; p < pointer_count; p++) {
                    uint32_t sym_index = indirect[indirect_offset + p];
                    if ((sym_index & INDIRECT_SYMBOL_ABS) != 0 ||
                        (sym_index & INDIRECT_SYMBOL_LOCAL) != 0) {
                        continue;
                    }
                    const char *name = strtab + symtab[sym_index].n_un.n_strx;
                    if (!symbol_is_send_family(name)) {
                        continue;
                    }
                    bindings[p] = replacement_for_symbol(name);
                    if (image_info.dli_fname != NULL) {
                        drover_log("[drover] rebound %s in %s", name, image_info.dli_fname);
                    }
                }
            }
        }
        cursor += command->cmdsize;
    }
}

static void on_image_added(const struct mach_header *header, intptr_t slide) {
    Dl_info image_info;
    memset(&image_info, 0, sizeof(image_info));
    if (dladdr(header, &image_info) != 0 && image_info.dli_fname != NULL &&
        strstr(image_info.dli_fname, "discord_voice") != NULL) {
        drover_log("[drover] voice module loaded: %s", image_info.dli_fname);
    }
    rebind_image(header, slide);
}

extern ssize_t sendto_nocancel(
    int,
    const void *,
    size_t,
    int,
    const struct sockaddr *,
    socklen_t
) asm("_sendto$NOCANCEL") __attribute__((weak_import));
extern ssize_t sendmsg_nocancel(int, const struct msghdr *, int)
    asm("_sendmsg$NOCANCEL") __attribute__((weak_import));
extern ssize_t send_nocancel(int, const void *, size_t, int)
    asm("_send$NOCANCEL") __attribute__((weak_import));

static ssize_t drover_sendto(
    int fd,
    const void *buffer,
    size_t length,
    int flags,
    const struct sockaddr *destination,
    socklen_t destination_length
) {
    pthread_once(&resolve_once, resolve_reals);
    if (real_sendto == NULL) {
        return -1;
    }

    maybe_inject(fd, length, destination, destination_length);
    return real_sendto(fd, buffer, length, flags, destination, destination_length);
}

static ssize_t drover_sendmsg(int fd, const struct msghdr *message, int flags) {
    pthread_once(&resolve_once, resolve_reals);
    if (real_sendmsg == NULL) {
        return -1;
    }

    const struct sockaddr *destination = NULL;
    socklen_t destination_length = 0;
    if (message != NULL && message->msg_name != NULL) {
        destination = (const struct sockaddr *)message->msg_name;
        destination_length = (socklen_t)message->msg_namelen;
    }

    maybe_inject(fd, msg_payload_length(message), destination, destination_length);
    return real_sendmsg(fd, message, flags);
}

static ssize_t drover_send(int fd, const void *buffer, size_t length, int flags) {
    pthread_once(&resolve_once, resolve_reals);
    send_fn impl = real_send != NULL ? real_send : send_as_sendto;
    maybe_inject(fd, length, NULL, 0);
    return impl(fd, buffer, length, flags);
}

DYLD_INTERPOSE(drover_sendto, sendto);
DYLD_INTERPOSE(drover_sendmsg, sendmsg);
DYLD_INTERPOSE(drover_send, send);

__attribute__((used)) static struct { void *replacement; void *replacee; }
    _interpose_sendto_nocancel __attribute__((section("__DATA,__interpose"))) = {
        (void *)(uintptr_t)drover_sendto,
        (void *)(uintptr_t)sendto_nocancel
    };
__attribute__((used)) static struct { void *replacement; void *replacee; }
    _interpose_sendmsg_nocancel __attribute__((section("__DATA,__interpose"))) = {
        (void *)(uintptr_t)drover_sendmsg,
        (void *)(uintptr_t)sendmsg_nocancel
    };
__attribute__((used)) static struct { void *replacement; void *replacee; }
    _interpose_send_nocancel __attribute__((section("__DATA,__interpose"))) = {
        (void *)(uintptr_t)drover_send,
        (void *)(uintptr_t)send_nocancel
    };

static int drover_socket(int domain, int type, int protocol) {
    pthread_once(&resolve_once, resolve_reals);
    if (real_socket == NULL) {
        return -1;
    }
    int fd = real_socket(domain, type, protocol);
    if (fd >= 0) {
        reset_injected_fd(fd);
    }
    if (hooks_active && fd >= 0 && (type & 0xff) == SOCK_DGRAM) {
        drover_log("[drover] socket dgram fd=%d domain=%d proto=%d", fd, domain, protocol);
    }
    return fd;
}

DYLD_INTERPOSE(drover_socket, socket);

__attribute__((constructor))
static void drover_init(void) {
    pthread_once(&resolve_once, resolve_reals);

    const char *debug = getenv("DROVER_DEBUG");
    debug_enabled = debug != NULL && debug[0] != '\0' && strcmp(debug, "0") != 0;
    verbose_udp = debug != NULL && strcmp(debug, "2") == 0;
    hooks_active = should_activate_hooks();
    renderer_hooks = false;
    if (hooks_active) {
        int *argc_ptr = _NSGetArgc();
        char ***argv_ptr = _NSGetArgv();
        if (argc_ptr != NULL && argv_ptr != NULL && *argv_ptr != NULL) {
            char **argv = *argv_ptr;
            for (int i = 1; i < *argc_ptr; i++) {
                if (argv[i] != NULL && argv_has_prefix(argv[i], "--type=renderer")) {
                    renderer_hooks = true;
                    break;
                }
            }
        }
    }

    if (!hooks_active) {
        return;
    }

    log_file = fopen("/tmp/discord-drover.log", "a");
    locate_paths();

    {
        const char *role = "main";
        int *argc_ptr = _NSGetArgc();
        char ***argv_ptr = _NSGetArgv();
        if (argc_ptr != NULL && argv_ptr != NULL && *argv_ptr != NULL) {
            char **argv = *argv_ptr;
            for (int i = 1; i < *argc_ptr; i++) {
                if (argv[i] != NULL && argv_has_prefix(argv[i], "--type=renderer")) {
                    role = "renderer";
                    break;
                }
            }
        }
        drover_log(
            "[drover] loaded pid=%d role=%s dylib=%s packet=%s",
            getpid(),
            role,
            dylib_path[0] != '\0' ? dylib_path : "(unknown)",
            packet_path[0] != '\0' ? packet_path : "(none)"
        );
    }
    if (real_sendto == NULL || real_sendmsg == NULL) {
        drover_log("[drover] failed to resolve original send functions");
    }
    _dyld_register_func_for_add_image(on_image_added);
    drover_log("[drover] send hooks armed (interpose+rebind)");
}
