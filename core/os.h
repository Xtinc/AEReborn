#ifndef AEREBORN_CORE_OS_H
#define AEREBORN_CORE_OS_H

#if defined(_WIN32) || defined(_WIN64)
#include <sdkddkver.h>
#if _WIN32_WINNT >= 0x0600 // Windows Vista
#define WINDOWS_OS_ENVIRONMENT 1
#else
#define WINDOWS_OS_ENVIRONMENT 0
#endif
#else
#define WINDOWS_OS_ENVIRONMENT 0
#endif

#if defined(__linux__) || defined(__linux) || defined(linux)
#define LINUX_OS_ENVIRONMENT 1
#else
#define LINUX_OS_ENVIRONMENT 0
#endif

namespace aereborn
{

/// @brief Set the calling thread's name and request the platform's audio scheduling priority.
/// @param name Null-terminated UTF-8 thread name. Use at most 15 bytes for portability.
/// @note The thread keeps its existing scheduling policy and priority if the OS rejects the request.
void configure_audio_thread(const char *name) noexcept;

} // namespace aereborn

#endif
