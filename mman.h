/*
 * sys/mman.h
 * mman-win32
 */

#ifndef _SYS_MMAN_H_
#define _SYS_MMAN_H_

#ifndef _WIN32_WINNT		// Allow use of features specific to Windows XP or later.                   
#define _WIN32_WINNT 0x0501	// Change this to the appropriate value to target other versions of Windows.
#endif						

/* All the headers include this file. */
#ifndef _MSC_VER
#include <_mingw.h>
#endif

#include <windows.h>
#include <errno.h>
#include <io.h>

#if defined(MMAN_LIBRARY_DLL)
/* Windows shared libraries (DLL) must be declared export when building the lib and import when building the 
application which links against the library. */
#if defined(MMAN_LIBRARY_EXPORT)
#define MMANSHARED_EXPORT __declspec(dllexport)
#else
#define MMANSHARED_EXPORT __declspec(dllimport)
#endif
#else
#define MMANSHARED_EXPORT
#endif

/* Protections are chosen from these bits, or-ed together */
#define PROT_NONE       0x00    /* no permissions */
#define PROT_READ       0x01    /* pages can be read */
#define PROT_WRITE      0x02    /* pages can be written */
#define PROT_EXEC       0x04    /* pages can be executed */

/* Sharing types (must choose one and only one of these) */
#define MAP_SHARED      0x01    /* Share changes */
#define MAP_PRIVATE     0x02    /* Changes are private */

/* Other flags */
#define MAP_FIXED       0x10    /* Interpret addr exactly */
#define MAP_ANONYMOUS   0x20    /* don't use a file */

/* Error returned from mmap() */
#define MAP_FAILED      ((void *)-1)

/* Flags for msync. */
#define MS_ASYNC        1
#define MS_SYNC         2
#define MS_INVALIDATE   4

static int __map_mman_error(const DWORD err, const int deferr)
{
    if (err == 0)
        return 0;
    //TODO: implement
    return err;
}

static DWORD __map_mmap_prot_page(const int prot)
{
    DWORD protect = 0;

    if (prot == PROT_NONE)
        return protect;

    if (prot & PROT_EXEC) {
        protect = (prot & PROT_WRITE) ?
            PAGE_EXECUTE_READWRITE :
            PAGE_EXECUTE_READ;
    }
    else {
        protect = (prot & PROT_WRITE) ?
            PAGE_READWRITE :
            PAGE_READONLY;
    }

    return protect;
}

static DWORD __map_mmap_prot_file(const int prot)
{
    DWORD desiredAccess = 0;

    if (prot == PROT_NONE)
        return desiredAccess;

    if (prot & PROT_READ)
        desiredAccess |= FILE_MAP_READ;
    if (prot & PROT_WRITE)
        desiredAccess |= FILE_MAP_WRITE;
    if (prot & PROT_EXEC)
        desiredAccess |= FILE_MAP_EXECUTE;

    return desiredAccess;
}

MMANSHARED_EXPORT void* mmap(void *addr, size_t len, int prot, int flags, int fildes, OffsetType off)
{
    HANDLE fm, h;
    void * map = MAP_FAILED;

    if (len == 0 /*|| off < 0*/)
    {
        errno = EINVAL;
        return MAP_FAILED;
    }

    h = ((flags & MAP_ANONYMOUS) ? INVALID_HANDLE_VALUE : (HANDLE)_get_osfhandle(fildes));

    if (h == INVALID_HANDLE_VALUE && !(flags & MAP_ANONYMOUS))
    {
        errno = EBADF;
        return MAP_FAILED;
    }

    fm = CreateFileMapping(h, NULL, __map_mmap_prot_page(prot), (DWORD)((off + len) >> 32), (DWORD)((off + len) & 0xFFFFFFFF), NULL);

    if (fm == NULL)
    {
        errno = __map_mman_error(GetLastError(), EPERM);
        return MAP_FAILED;
    }

    map = MapViewOfFileEx(fm, __map_mmap_prot_file(prot), (DWORD)(off >> 32), (DWORD)(off & 0xFFFFFFFF), len, addr);

    CloseHandle(fm);

    if (map == NULL)
    {
        errno = __map_mman_error(GetLastError(), EPERM);
        return MAP_FAILED;
    }

    return map;
}

MMANSHARED_EXPORT int munmap(void *addr, size_t len)
{
    if (UnmapViewOfFile(addr))
        return 0;

    errno = __map_mman_error(GetLastError(), EPERM);
    return -1;
}

MMANSHARED_EXPORT int _mprotect(void *addr, size_t len, int prot)
{
    DWORD newProtect = __map_mmap_prot_page(prot);
    DWORD oldProtect = 0;

    if (VirtualProtect(addr, len, newProtect, &oldProtect))
        return 0;

    errno = __map_mman_error(GetLastError(), EPERM);
    return -1;
}

MMANSHARED_EXPORT int msync(void *addr, size_t len, int flags)
{
    if (flags & MS_SYNC)
    {
        if (FlushViewOfFile(addr, len))
            return 0;

        errno = __map_mman_error(GetLastError(), EPERM);
        return -1;
    }

    return 0;
}

MMANSHARED_EXPORT int mlock(const void *addr, size_t len)
{
    if (VirtualLock((LPVOID)addr, len))
        return 0;

    errno = __map_mman_error(GetLastError(), EPERM);
    return -1;
}

MMANSHARED_EXPORT int munlock(const void *addr, size_t len)
{
    if (VirtualUnlock((LPVOID)addr, len))
        return 0;

    errno = __map_mman_error(GetLastError(), EPERM);
    return -1;
}

#ifdef __cplusplus
}
#endif

#endif /*  _SYS_MMAN_H_ */
