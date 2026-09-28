/****************************************************************************************
 **
 ** L29 Stage-A: bounded exact-key client buffer pool for the libhybris Wayland
 ** window path (WaylandNativeWindow / ServerWaylandBuffer).
 **
 ** Scope: remove/reduce repeated vendor first-use work when an exact compatible
 ** geometry (width, height, format, usage) returns.  It is a *partial*
 ** optimization: fresh-geometry drag is not addressed and continuous new-size
 ** churn keeps its cost.
 **
 ** Accounting contract (enforced by WaylandNativeWindow and by the host model):
 **  - a pooled buffer is NOT part of the active m_bufList and is NOT counted in
 **    m_freeBufs;
 **  - only fully released buffers (not busy, not posted/fronted/queued/
 **    post_registered) may enter the pool;
 **  - eviction/drain uses the resource-only destruction path, never the
 **    active-list accounting path;
 **  - format/usage changes drain the entries currently pooled (best-effort
 **    semantic hygiene).  An old-format/usage buffer that is still active can
 **    still be retired later into its own exact-key entry; the exact key keeps
 **    the cache correct, so no generation system is used.
 **  - exactly one owner: a buffer is either active, or pooled, or destroyed.
 **
 ** Bounds: <= 3 distinct keys, <= 6 buffers, <= 16 MiB accounted payload.
 ** `bytes` is a bounded *logical/accounted* payload (stride x height x bpp for
 ** supported gralloc formats), not exact physical/PVR resident memory: vendor
 ** tiling/alignment/hidden allocations are not measurable here.
 **
 ** Unsupported formats, zero/invalid height or stride and overflowing
 ** arithmetic are never cached (the caller destroys the buffer instead).
 **
 ****************************************************************************************/
#ifndef L29_WAYLAND_WINDOW_POOL_H
#define L29_WAYLAND_WINDOW_POOL_H

#include <stddef.h>
#include <stdint.h>

namespace l29 {

#ifndef L29_POOL_MAX_GROUPS
#define L29_POOL_MAX_GROUPS 3
#endif
#ifndef L29_POOL_MAX_BUFFERS
#define L29_POOL_MAX_BUFFERS 6
#endif
#ifndef L29_POOL_MAX_BYTES
#define L29_POOL_MAX_BYTES (16ull * 1024ull * 1024ull)
#endif

/* Supported gralloc format -> bytes per pixel.  0 means "not safely sized". */
static inline unsigned l29_bytes_per_pixel(int format)
{
    switch (format) {
    case 1: /* HAL_PIXEL_FORMAT_RGBA_8888 */
    case 2: /* HAL_PIXEL_FORMAT_RGBX_8888 */
    case 5: /* HAL_PIXEL_FORMAT_BGRA_8888 */
        return 4;
    case 3: /* HAL_PIXEL_FORMAT_RGB_888 */
        return 3;
    case 4: /* HAL_PIXEL_FORMAT_RGB_565 */
        return 2;
    default:
        return 0;
    }
}

/* Overflow-safe accounted payload for one buffer.  Returns 0 on success,
 * -1 when the format is unsupported or any multiplication would overflow. */
static inline int l29_accounted_bytes(unsigned stride, unsigned height, int format,
                                      unsigned long long *out)
{
    const unsigned bpp = l29_bytes_per_pixel(format);
    unsigned long long s, h;
    if (!out || bpp == 0)
        return -1;
    s = (unsigned long long)stride;
    h = (unsigned long long)height;
    if (s == 0 || h == 0)
        return -1;
    if (s > (~0ull) / h)
        return -1;
    s *= h;
    if (s > (~0ull) / (unsigned long long)bpp)
        return -1;
    *out = s * (unsigned long long)bpp;
    return 0;
}

template <typename T>
class BufferPool {
public:
    BufferPool() { init(); }

    void init()
    {
        m_count = 0;
        m_bytes = 0;
        m_tick = 0;
    }

    int count() const { return m_count; }
    unsigned long long bytes() const { return m_bytes; }

    /* Exact-key inspection helper (diagnostics/tests only; no behaviour). */
    bool contains(unsigned w, unsigned h, int format, unsigned usage) const
    {
        return find_group(w, h, format, usage) >= 0;
    }

    /* Number of distinct exact keys currently pooled (inspection/diagnostics;
     * the hard cap is enforced through needs_room()/put()). */
    int groups() const
    {
        int i, j, n = 0;
        for (i = 0; i < m_count; i++) {
            bool dup = false;
            for (j = 0; j < i; j++)
                if (key_eq(m_entries[j], m_entries[i].width, m_entries[i].height,
                           m_entries[i].format, m_entries[i].usage)) {
                    dup = true;
                    break;
                }
            if (!dup)
                n++;
        }
        return n;
    }

    /* Insert a fully released, not-in-flight buffer.  Returns true when the
     * pool took ownership (the caller must not destroy it); returns false when
     * the caller must destroy it (unsupported/invalid/overflowing geometry,
     * single entry above the byte cap, or caps still exceeded after eviction).
     * Every evicted entry is destroyed through `destroy` exactly once. */
    template <typename Destroy>
    bool put(T *buf, unsigned w, unsigned h, int format, unsigned usage,
             unsigned stride, Destroy destroy)
    {
        unsigned long long bytes;
        int g;

        if (!buf)
            return false;
        if (l29_accounted_bytes(stride, h, format, &bytes) != 0)
            return false; /* unknown/overflow: never cache */
        if (bytes > (unsigned long long)L29_POOL_MAX_BYTES)
            return false; /* a single entry can never fit */

        g = find_group(w, h, format, usage);
        if (g >= 0 && m_entries[g].stride != stride) {
            /* Exact key but a different stride: the accounted payload is not
             * interchangeable; drop the old group and re-cache. */
            drop_group(g, destroy);
        }

        while (needs_room(w, h, format, usage, bytes)) {
            if (!evict_lru(destroy))
                return false;
        }
        if (m_count >= L29_POOL_MAX_BUFFERS)
            return false;
        if (m_bytes + bytes > (unsigned long long)L29_POOL_MAX_BYTES)
            return false;

        Entry &e = m_entries[m_count++];
        e.buf = buf;
        e.width = w;
        e.height = h;
        e.format = format;
        e.usage = usage;
        e.stride = stride;
        e.bytes = bytes;
        e.last_use = ++m_tick;
        m_bytes += bytes;
        return true;
    }

    /* Remove and return the most recently used exact-key entry, or NULL. */
    T *take(unsigned w, unsigned h, int format, unsigned usage)
    {
        int best = -1;
        int i;

        for (i = 0; i < m_count; i++) {
            if (!key_eq(m_entries[i], w, h, format, usage))
                continue;
            if (best < 0 || m_entries[i].last_use > m_entries[best].last_use)
                best = i;
        }
        if (best < 0)
            return NULL;
        return remove_at(best);
    }

    /* Destroy and remove the least recently used entry; false when empty. */
    template <typename Destroy>
    bool evict_lru(Destroy destroy)
    {
        int best = -1;
        int i;

        for (i = 0; i < m_count; i++)
            if (best < 0 || m_entries[i].last_use < m_entries[best].last_use)
                best = i;
        if (best < 0)
            return false;
        destroy(remove_at(best));
        return true;
    }

    /* Destroy and remove every entry (teardown / format-usage change). */
    template <typename Destroy>
    void drain(Destroy destroy)
    {
        while (m_count > 0)
            destroy(remove_at(m_count - 1));
        m_bytes = 0;
    }

private:
    struct Entry {
        T *buf;
        unsigned width;
        unsigned height;
        int format;
        unsigned usage;
        unsigned stride;
        unsigned long long bytes;
        unsigned long long last_use;
    };

    static bool key_eq(const Entry &e, unsigned w, unsigned h, int f, unsigned u)
    {
        return e.width == w && e.height == h && e.format == f && e.usage == u;
    }

    int find_group(unsigned w, unsigned h, int f, unsigned u) const
    {
        int i;
        for (i = 0; i < m_count; i++)
            if (key_eq(m_entries[i], w, h, f, u))
                return i;
        return -1;
    }

    bool needs_room(unsigned w, unsigned h, int f, unsigned u,
                    unsigned long long bytes) const
    {
        if (find_group(w, h, f, u) < 0 &&
            groups() >= L29_POOL_MAX_GROUPS)
            return true;
        if (m_count >= L29_POOL_MAX_BUFFERS)
            return true;
        if (m_bytes + bytes > (unsigned long long)L29_POOL_MAX_BYTES)
            return true;
        return false;
    }

    T *remove_at(int i)
    {
        T *buf = m_entries[i].buf;
        m_bytes -= m_entries[i].bytes;
        m_entries[i] = m_entries[--m_count];
        return buf;
    }

    template <typename Destroy>
    void drop_group(int g, Destroy destroy)
    {
        const unsigned w = m_entries[g].width;
        const unsigned h = m_entries[g].height;
        const int f = m_entries[g].format;
        const unsigned u = m_entries[g].usage;
        int i;

        for (i = m_count - 1; i >= 0; i--) {
            if (key_eq(m_entries[i], w, h, f, u))
                destroy(remove_at(i));
        }
    }

    Entry m_entries[L29_POOL_MAX_BUFFERS];
    int m_count;
    unsigned long long m_bytes;
    unsigned long long m_tick;
};

} /* namespace l29 */

#endif /* L29_WAYLAND_WINDOW_POOL_H */
