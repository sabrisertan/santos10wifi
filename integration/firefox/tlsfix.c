/* santos-tlsfix: preserve the bionic GLES dispatch pointer kept in the
 * native TLS slot at gs:0xc across pthread_create.
 *
 * Under glibc, gs:0 points at the TCB and gs:0xc is `multiple_threads`,
 * which glibc sets to 1 on the first thread creation, clobbering the
 * table pointer the vendor /system/lib/libGLESv2.so dispatcher stored
 * there.  The dispatcher trampolines then jump through 0x1.
 *
 * Capture the slot on entry and restore it after the real call.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <dlfcn.h>

typedef int (*pc_t)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);

static unsigned long getslot(void)
{
    unsigned long v;
    __asm__ __volatile__("movl %%gs:0xc, %0" : "=r"(v));
    return v;
}
static void setslot(unsigned long v)
{
    __asm__ __volatile__("movl %0, %%gs:0xc" :: "r"(v) : "memory");
}

int pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg)
{
    static pc_t real;
    unsigned long save = getslot();
    int r;

    if (!real) real = (pc_t)dlsym(RTLD_NEXT, "pthread_create");

    r = real(t, a, fn, arg);

    /* Restore only a plausible vendor-GLES dispatch pointer.  Restoring a
     * 0/1 (glibc single/multi-threaded flag) would regress
     * multiple_threads back to 0 after glibc created a thread, which
     * corrupts malloc (sysmalloc assertion) and crashes mpv. */
    if (save > 0x1000 && (save & 3) == 0)
        setslot(save);

    return r;
}
