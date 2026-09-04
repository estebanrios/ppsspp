#define VMA_IMPLEMENTATION
// STV_VMA_BLOQUES_v1 (2026-09-04): VMA considera "chico" a todo heap de
// hasta 1 GiB y en ese caso IGNORA preferredLargeHeapBlockSize y usa bloques
// de heap/8: en el Mali-G57 del A523 (heap de 819 MB, memoria unificada =
// RAM del sistema) eso son bloques de 102 MB que solo se devuelven cuando
// quedan enteramente vacios. Medido en Ghost of Sparta: 49-77 MB en uso y
// 128-144 MB reservados; el driver le cuenta al proceso 170-320 MB. Con el
// umbral en 256 MB el heap pasa a "grande" y manda el tamano preferido que
// fija VulkanContext.cpp (16 MiB).
#define VMA_SMALL_HEAP_MAX_SIZE (256ULL * 1024 * 1024)

#include "ppsspp_config.h"

#if PPSSPP_PLATFORM(WINDOWS)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#include "Common/GPU/Vulkan/VulkanLoader.h"

using namespace PPSSPP_VK;

#undef VK_NO_PROTOTYPES
#include "vk_mem_alloc.h"
#define VK_NO_PROTOTYPES


// This chunk should be added to vk_mem_alloc.h when upgrading, right below the #ifndef at the top:

/*

// BEGIN PPSSPP HACKS !!!!!
#ifdef USE_CRT_DBG
#undef new
#endif

#if defined(__APPLE__)
#include <AvailabilityMacros.h>

#if defined(__IPHONE_OS_VERSION_MIN_REQUIRED) && (!defined(__IPHONE_10_0) || __IPHONE_OS_VERSION_MIN_REQUIRED < __IPHONE_10_0)
#define VMA_USE_STL_SHARED_MUTEX 0
#endif
#if defined(MAC_OS_X_VERSION_MIN_REQUIRED) && (!defined(MAC_OS_X_VERSION_10_12) || MAC_OS_X_VERSION_MIN_REQUIRED < MAC_OS_X_VERSION_10_12)
#define VMA_USE_STL_SHARED_MUTEX 0
#endif

#endif
// END PPSSPP HACKS


*/
