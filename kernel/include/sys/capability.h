#ifndef BOLDON_CAPABILITY_H
#define BOLDON_CAPABILITY_H
#include <stdint.h>
#include <stdbool.h>

/* Only privileges enforced in BoldonOS are advertised to Linux programs. */
#define BOLDON_CAP_DAC_OVERRIDE (1ULL << 1)
#define BOLDON_CAP_FOWNER       (1ULL << 3)
#define BOLDON_CAP_KILL         (1ULL << 5)
#define BOLDON_CAP_SETGID       (1ULL << 6)
#define BOLDON_CAP_SETUID       (1ULL << 7)
#define BOLDON_CAP_SETPCAP      (1ULL << 8)
#define BOLDON_CAP_SYS_ADMIN    (1ULL << 21)
#define BOLDON_CAP_SYS_BOOT     (1ULL << 22)
#define BOLDON_CAP_SETFCAP      (1ULL << 31)
#define BOLDON_CAP_SUPPORTED (BOLDON_CAP_DAC_OVERRIDE | BOLDON_CAP_FOWNER | \
                              BOLDON_CAP_KILL | BOLDON_CAP_SETGID | \
                              BOLDON_CAP_SETUID | BOLDON_CAP_SETPCAP | \
                              BOLDON_CAP_SYS_ADMIN | BOLDON_CAP_SYS_BOOT | \
                              BOLDON_CAP_SETFCAP)

struct vfs_node;
typedef struct {
    bool present;
    bool effective;
    uint64_t permitted, inheritable;
} boldon_file_caps_t;
/* No xattr -> present=false. Unsupported/invalid file caps -> negative errno. */
int capability_parse_file(const void *raw, int len, boldon_file_caps_t *caps);
int capability_read_file(struct vfs_node *node, boldon_file_caps_t *caps);
struct process;
void capability_apply_exec(struct process *process, const boldon_file_caps_t *caps,
                           bool setid);
#endif
