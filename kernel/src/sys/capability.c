#include <sys/capability.h>
#include <fs/vfs.h>
#include <scheduler/scheduler.h>
#include <errno.h>
#include <string.h>
#include <memory.h>

/* Linux security.capability v1/v2/v3 disk format, little-endian on x86-64.
   Never silently discard unsupported privileges or a noninitial userns. */
int capability_read_file(vfs_node_t *node, boldon_file_caps_t *caps) {
    if (!caps) return -EINVAL;
    memset(caps, 0, sizeof(*caps));
    if (!node || !node->ops || !node->ops->getxattr) return 0;

    uint8_t raw[24] = {0};
    int len = node->ops->getxattr(node, "security.capability", raw, sizeof(raw));
    if (len == -ENODATA) return 0;
    if (len < 0) return len == -ERANGE ? -EOPNOTSUPP : len;
    return capability_parse_file(raw, len, caps);
}

int capability_parse_file(const void *raw, int len, boldon_file_caps_t *caps) {
    if (!raw || !caps) return -EINVAL;
    memset(caps, 0, sizeof(*caps));
    const uint8_t *bytes = raw;
    if (len < 4) return -EINVAL;

    uint32_t magic;
    memcpy(&magic, bytes, sizeof(magic));
    uint32_t revision = magic & 0xff000000U;
    int expected = revision == 0x01000000U ? 12 :
                   revision == 0x02000000U ? 20 :
                   revision == 0x03000000U ? 24 : -1;
    if (len != expected || (magic & 0x00fffffeU)) return -EINVAL;
    if (revision == 0x03000000U) {
        uint32_t rootid;
        memcpy(&rootid, bytes + 20, sizeof(rootid));
        if (rootid) return -EOPNOTSUPP; /* no user namespaces */
    }
    uint32_t p0, i0, p1 = 0, i1 = 0;
    memcpy(&p0, bytes + 4, 4);
    memcpy(&i0, bytes + 8, 4);
    if (len >= 20) {
        memcpy(&p1, bytes + 12, 4);
        memcpy(&i1, bytes + 16, 4);
    }
    caps->permitted = (uint64_t)p0 | ((uint64_t)p1 << 32);
    caps->inheritable = (uint64_t)i0 | ((uint64_t)i1 << 32);
    if ((caps->permitted | caps->inheritable) & ~BOLDON_CAP_SUPPORTED)
        return -EOPNOTSUPP;
    caps->present = true;
    caps->effective = (magic & 1U) != 0;
    return 0;
}

void capability_apply_exec(process_t *p, const boldon_file_caps_t *caps,
                           bool setid) {
    if (!p || !caps) return;
    bool privileged = caps->present || setid;
    if (privileged) p->cap_ambient = 0;
    uint64_t old_permitted = p->cap_permitted;
    if (caps->present) {
        p->cap_permitted = (caps->permitted & p->cap_bounding) |
                           (caps->inheritable & p->cap_inheritable);
    } else {
        p->cap_permitted = p->euid == 0 ?
            (p->cap_bounding & BOLDON_CAP_SUPPORTED) : p->cap_ambient;
    }
    if (p->no_new_privs) p->cap_permitted &= old_permitted;
    p->cap_effective = caps->present ?
        (caps->effective ? p->cap_permitted : 0) : p->cap_permitted;
}
