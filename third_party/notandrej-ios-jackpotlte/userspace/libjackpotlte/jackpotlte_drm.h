/* SPDX-License-Identifier: MIT */
/*
 * jackpotlte_drm: the libdrm-shaped face of the M4 shim.
 *
 * Mesa's panfrost is built against libdrm.  It opens a device
 * (drmOpenWithType), hands it Linux's DRM requests (drmIoctl), and maps a
 * buffer with mmap() on the device.  iOS has no libdrm and no mmap() on a
 * driver's memory, so this header is what Mesa is built against instead: it is
 * installed into Mesa's tree as its xf86drm.h, and jackpotlte_drm.c carries
 * each request to our kernel's user client through jackpotlte_gpu (0154).
 *
 * The names libdrm owns keep libdrm's names and semantics, because Mesa calls
 * them unmodified.  What libdrm has no equivalent for is ours, and says so with
 * a jackpotlte_ prefix:
 *
 *   jackpotlte_drm_map / _unmap   a buffer's memory in this process, in place
 *                                 of mmap() on the device (pan_kmod.c's mmap)
 *   jackpotlte_drm_surface        a SweetSurface's pixels as a buffer - the
 *                                 display's own memory (0154's 0x7e)
 *   jackpotlte_drm_last_seq       the number of the last job submitted
 *   jackpotlte_drm_wait_seq       and waiting for one by its number (0x7d)
 *
 * The request numbers are Linux's, and our kernel's selectors are those
 * numbers' low bytes (0154:66-79), so a request is not renumbered.  Its
 * structure is translated field by field instead: the kernel reads exactly its
 * own structures and refuses a submit larger than its 40-byte one, which is
 * where this layer differs from the reference's (theirs passes the bytes
 * through untouched).
 */
#ifndef JACKPOTLTE_DRM_H
#define JACKPOTLTE_DRM_H

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * What Mesa's panfrost takes from libdrm's xf86drm.h.
 *
 * The request codes and their structures are Linux's, and Mesa carries them
 * (include/drm-uapi/drm.h, which itself includes drm_mode.h).  Real libdrm's
 * xf86drm.h includes drm.h, and so does this one: Mesa's renderonly.c includes
 * only <xf86drm.h> and then uses DRM_IOCTL_MODE_CREATE_DUMB and
 * struct drm_mode_create_dumb, so a stand-in that left them out would not be a
 * stand-in at all.  Nothing here repeats what that header says.
 *
 * The include is conditional because this header is also built outside Mesa
 * (userspace/build.sh, for the phone), where there is no drm-uapi copy and
 * nothing asks for those codes.
 */
#if defined(__has_include)
#if __has_include("drm-uapi/drm.h")
#include "drm-uapi/drm.h"
#endif
#endif
#ifndef DRM_NODE_PRIMARY
#define DRM_NODE_PRIMARY        0
#endif
#ifndef DRM_NODE_RENDER
#define DRM_NODE_RENDER         2
#endif
#ifndef DRM_CLOEXEC
#define DRM_CLOEXEC             O_CLOEXEC
#endif
#ifndef DRM_RDWR
#define DRM_RDWR                O_RDWR
#endif

typedef struct _drmVersion {
	int version_major, version_minor, version_patchlevel;
	int name_len;
	char *name;
	int date_len;
	char *date;
	int desc_len;
	char *desc;
} drmVersion, *drmVersionPtr;

int drmOpenWithType(const char *name, const char *busid, int type);
int drmIoctl(int fd, unsigned long request, void *arg);
int drmCommandWrite(int fd, unsigned long index, const void *data, unsigned long size);
int drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size);
drmVersionPtr drmGetVersion(int fd);
void drmFreeVersion(drmVersionPtr version);
int drmGetCap(int fd, uint64_t capability, uint64_t *value);

/*
 * Buffers and fences as file descriptors: there are none here.  But memory
 * somebody else has made does come in the way Linux's does, by a descriptor:
 * a SweetSurface, by its ID, where panfrost expects one.  The buffer itself is
 * made by drmPrimeFDToHandle().
 */
int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
int drmPrimeFDToHandle(int fd, int surface, uint32_t *handle);
int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle);
int drmSyncobjDestroy(int fd, uint32_t handle);
int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count);
int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t count);
int drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points, uint32_t count);
int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle);
int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd);
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd);
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd);
int drmSyncobjWait(int fd, uint32_t *handles, unsigned count, int64_t timeout_nsec,
    unsigned flags, uint32_t *first_signaled);
int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned count,
    int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled);
int drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t count);
int drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points, uint32_t count, uint32_t flags);
int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle,
    uint64_t src_point, uint32_t flags);
int drmGetNodeTypeFromFd(int fd);
int drmCloseBufferHandle(int fd, uint32_t handle);

/* Linux's list of DRM devices, which the Vulkan runtime walks: none here. */
#ifndef DRM_NODE_MAX
#define DRM_NODE_MAX            3
#endif
#ifndef DRM_BUS_PCI
#define DRM_BUS_PCI             0
#endif
#ifndef DRM_BUS_USB
#define DRM_BUS_USB             1
#endif
#ifndef DRM_BUS_PLATFORM
#define DRM_BUS_PLATFORM        2
#endif
#ifndef DRM_BUS_HOST1X
#define DRM_BUS_HOST1X          3
#endif
typedef struct _drmDevice {
	char **nodes;
	int available_nodes;
	int bustype;
	union { void *pci, *usb, *platform, *host1x; } businfo;
	union { void *pci, *usb, *platform, *host1x; } deviceinfo;
} drmDevice, *drmDevicePtr;
int drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices);
void drmFreeDevices(drmDevicePtr devices[], int count);
int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device);
void drmFreeDevice(drmDevicePtr *device);

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Ours.
 */
struct jt_gpu;

/*
 * A buffer's memory in this process, in place of mmap() on the device:
 * MAP_FAILED with errno, or the address.  A mapping is kept until the buffer
 * is given back (drmCloseBufferHandle), because panfrost unmaps a buffer every
 * time its cache takes it back and maps it again when it hands it out, and each
 * of those is a call into the kernel.
 */
void *jackpotlte_drm_map(int fd, uint32_t handle, uint64_t size);
void jackpotlte_drm_unmap(int fd, uint32_t handle, void *address);

/*
 * What a SweetSurface's pixels would be like as a buffer (0154:82, "struct
 * jackpotlte_surface").  The fields are named as the reference names its own
 * mirror of that structure; the layout is the kernel's, field for field.
 * `handle` = JACKPOTLTE_DRM_DESCRIBE asks only what the buffer would be like,
 * 0 asks for one.  0, or -1 with errno.
 */
struct jackpotlte_drm_surface {
	uint32_t surface;                       /* the SweetSurface's ID */
	uint32_t handle;                        /* JACKPOTLTE_DRM_DESCRIBE: describe only */
	uint64_t size;                          /* back: how big the buffer is, */
	uint32_t offset, row_bytes;             /* where the pixels start and how long a row is, */
	uint32_t width, height, pixel_format, planes;   /* and what they are */
};
#define JACKPOTLTE_DRM_DESCRIBE 0xffffffffu
int jackpotlte_drm_surface(int fd, struct jackpotlte_drm_surface *surface);

/* The number of the last job submitted, and a wait for one by its number for
 * `relative_ns` at most: 0 when it is done, -ETIMEDOUT when it is not. */
uint64_t jackpotlte_drm_last_seq(void);
int jackpotlte_drm_wait_seq(uint64_t seq, int64_t relative_ns);

/*
 * One DRM request, as drmIoctl() would carry it, without the errno dance: 0 or
 * -errno.  `request` is a DRM_IOCTL_* code - its low byte is the kernel's
 * selector (0154:66-79), the size field the structure's size, and `size` is
 * that size given explicitly.  This is the part the host test drives against a
 * fake GPU; drmIoctl() is the thin wrapper Mesa sees.
 */
int jackpotlte_drm_ioctl(struct jt_gpu *gpu, unsigned long request, void *arg, size_t size);

/*
 * The one connection this process has to the GPU.  Every drm* call above goes
 * through it: there is one GPU, and buffers belong to the connection that made
 * them, so a second connection would see none of the first one's.
 *
 * jackpotlte_drm_gpu() opens it on first use (on the phone; on a build machine
 * it returns what was attached).  jackpotlte_drm_attach() points the layer at
 * a GPU somebody else opened - the host test's fake - and clears the state that
 * belonged to the last one.  The caller still owns the GPU it attached.
 */
struct jt_gpu *jackpotlte_drm_gpu(void);
void jackpotlte_drm_attach(struct jt_gpu *gpu);

/*
 * A DRM_IOCTL_* code from its two parts, as drmIoctl() reads them: the number
 * in the low byte, the structure's size at bit 16.  Linux's _IOC and BSD's
 * agree on those, so a caller (and the host test) can build one without either
 * system's ioctl headers.
 */
#define JACKPOTLTE_DRM_REQUEST(number, size) \
	((((unsigned long) (size) & 0x3ffful) << 16) | ((unsigned long) (number) & 0xfful))

/* The kernel's structures, checked where they are declared: userspace.yml
 * compiles the same numbers out of xnu/patches/0154 (0154:82-95). */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(struct jackpotlte_drm_surface) == 40, "0154: jackpotlte_surface");
_Static_assert(offsetof(struct jackpotlte_drm_surface, handle) == 4, "0154: surface.handle");
_Static_assert(offsetof(struct jackpotlte_drm_surface, size) == 8, "0154: surface.size");
_Static_assert(offsetof(struct jackpotlte_drm_surface, row_bytes) == 20, "0154: surface.rowBytes");
_Static_assert(offsetof(struct jackpotlte_drm_surface, planes) == 36, "0154: surface.planes");
#endif

#ifdef __cplusplus
}
#endif

#endif /* JACKPOTLTE_DRM_H */
