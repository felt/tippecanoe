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

/* Determine offset type */
#include <stdint.h>
#if defined(_WIN64)
typedef int64_t OffsetType;
#else
typedef uint32_t OffsetType;
#endif

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Protections are chosen from these bits, or-ed together */
#define PROT_NONE       0x00    /* no permissions */
#define PROT_READ       0x01    /* pages can be read */
#define PROT_WRITE      0x02    /* pages can be written */
#define PROT_EXEC       0x04    /* pages can be executed */

/* Sharing types (must choose one and only one of these) */
#define MAP_FILE        0x00
#define MAP_SHARED      0x01    /* Share changes */
#define MAP_PRIVATE     0x02    /* Changes are private */
#define MAP_TYPE        0x0f

/* Other flags */
#define MAP_FIXED       0x10    /* Interpret addr exactly */
#define MAP_ANONYMOUS   0x20    /* don't use a file */
#define MAP_ANON        MAP_ANONYMOUS

/* Error returned from mmap() */
#define MAP_FAILED      ((void *)-1)

/* Flags for msync. */
#define MS_ASYNC        1
#define MS_SYNC         2
#define MS_INVALIDATE   4

#ifndef FILE_MAP_EXECUTE
#define FILE_MAP_EXECUTE    0x0020
#endif /* FILE_MAP_EXECUTE */

static inline int __map_mman_error(const DWORD err, const int deferr)
{
    if (err == 0)
        return 0;
    //TODO: implement
    return err;
}

static inline DWORD __map_mmap_prot_page(const int prot)
{
    DWORD protect = 0;
    
    if (prot == PROT_NONE)
        return protect;
        
    if ((prot & PROT_EXEC) != 0)
    {
        protect = ((prot & PROT_WRITE) != 0) ? 
                    PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    }
    else
    {
        protect = ((prot & PROT_WRITE) != 0) ?
                    PAGE_READWRITE : PAGE_READONLY;
    }
    
    return protect;
}

static inline DWORD __map_mmap_prot_file(const int prot)
{
    DWORD desiredAccess = 0;
    
    if (prot == PROT_NONE)
        return desiredAccess;
        
    if ((prot & PROT_READ) != 0)
        desiredAccess |= FILE_MAP_READ;
    if ((prot & PROT_WRITE) != 0)
        desiredAccess |= FILE_MAP_WRITE;
    if ((prot & PROT_EXEC) != 0)
        desiredAccess |= FILE_MAP_EXECUTE;
    
    return desiredAccess;
}

MMANSHARED_EXPORT inline void* mmap(void *addr, size_t len, int prot, int flags, int fildes, OffsetType off)
{
    HANDLE fm, h;
    
    void * map = MAP_FAILED;
    
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4293)
#endif

    const DWORD dwFileOffsetLow = (sizeof(OffsetType) <= sizeof(DWORD)) ?
                    (DWORD)off : (DWORD)(off & 0xFFFFFFFFL);
    const DWORD dwFileOffsetHigh = (sizeof(OffsetType) <= sizeof(DWORD)) ?
                    (DWORD)0 : (DWORD)((off >> 32) & 0xFFFFFFFFL);
    const DWORD protect = __map_mmap_prot_page(prot);
    const DWORD desiredAccess = __map_mmap_prot_file(prot);

    const OffsetType maxSize = off + (OffsetType)len;

    const DWORD dwMaxSizeLow = (sizeof(OffsetType) <= sizeof(DWORD)) ?
                    (DWORD)maxSize : (DWORD)(maxSize & 0xFFFFFFFFL);
    const DWORD dwMaxSizeHigh = (sizeof(OffsetType) <= sizeof(DWORD)) ?
                    (DWORD)0 : (DWORD)((maxSize >> 32) & 0xFFFFFFFFL);

#ifdef _MSC_VER
#pragma warning(pop)
#endif

    errno = 0;
    
    if (len == 0 
        /* Usupported protection combinations */
        || prot == PROT_EXEC)
    {
        errno = EINVAL;
        return MAP_FAILED;
    }
    
    h = ((flags & MAP_ANONYMOUS) == 0) ? 
                    (HANDLE)_get_osfhandle(fildes) : INVALID_HANDLE_VALUE;

    if ((flags & MAP_ANONYMOUS) == 0 && h == INVALID_HANDLE_VALUE)
    {
        errno = EBADF;
        return MAP_FAILED;
    }

    fm = CreateFileMapping(h, NULL, protect, dwMaxSizeHigh, dwMaxSizeLow, NULL);

    if (fm == NULL)
    {
        errno = __map_mman_error(GetLastError(), EPERM);
        return MAP_FAILED;
    }
  
    if ((flags & MAP_FIXED) == 0)
    {
        map = MapViewOfFile(fm, desiredAccess, dwFileOffsetHigh, dwFileOffsetLow, len);
    }
    else
    {
        map = MapViewOfFileEx(fm, desiredAccess, dwFileOffsetHigh, dwFileOffsetLow, len, addr);
    }

    CloseHandle(fm);
  
    if (map == NULL)
    {
        errno = __map_mman_error(GetLastError(), EPERM);
        return MAP_FAILED;
    }

    return map;
}

MMANSHARED_EXPORT inline int munmap(void *addr, size_t len)
{
    if (UnmapViewOfFile(addr))
        return 0;

    errno = __map_mman_error(GetLastError(), EPERM);
    return -1;
}

MMANSHARED_EXPORT inline int _mprotect(void *addr, size_t len, int prot)
{
    DWORD newProtect = __map_mmap_prot_page(prot);
    DWORD oldProtect = 0;

    if (VirtualProtect(addr, len, newProtect, &oldProtect))
        return 0;

    errno = __map_mman_error(GetLastError(), EPERM);
    return -1;
}

MMANSHARED_EXPORT inline int msync(void *addr, size_t len, int flags)
{
    if (FlushViewOfFile(addr, len))
        return 0;
    
    errno =  __map_mman_error(GetLastError(), EPERM);
    
    return -1;
}

MMANSHARED_EXPORT inline int mlock(const void *addr, size_t len)
{
    if (VirtualLock((LPVOID)addr, len))
        return 0;

    errno = __map_mman_error(GetLastError(), EPERM);
    return -1;
}

MMANSHARED_EXPORT inline int munlock(const void *addr, size_t len)
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
