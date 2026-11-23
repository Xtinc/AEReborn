#include "os.h"

#if LINUX_OS_ENVIRONMENT || defined(__APPLE__)
#include <pthread.h>
#include <sched.h>
#elif WINDOWS_OS_ENVIRONMENT
#include <windows.h>
#endif

namespace aereborn
{

void configure_audio_thread(const char *name) noexcept
{
#if LINUX_OS_ENVIRONMENT || defined(__APPLE__)
    const auto thread = pthread_self();
#if LINUX_OS_ENVIRONMENT
    pthread_setname_np(thread, name);
    constexpr auto policy = SCHED_RR;
    const auto priority = sched_get_priority_max(policy) - 1;
#else
    pthread_setname_np(name);
    constexpr auto policy = SCHED_FIFO;
    const auto priority = sched_get_priority_min(policy);
#endif
    if (priority >= 0)
    {
        sched_param params{};
        params.sched_priority = priority;
        pthread_setschedparam(thread, policy, &params);
    }
#elif WINDOWS_OS_ENVIRONMENT
    const auto thread = GetCurrentThread();
    SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL);
    using SetDescription = HRESULT(WINAPI *)(HANDLE, PCWSTR);
    const auto kernel = GetModuleHandleW(L"Kernel32.dll");
    if (kernel)
    {
        if (const auto set_name = reinterpret_cast<SetDescription>(GetProcAddress(kernel, "SetThreadDescription")))
        {
            constexpr int capacity = 16;
            wchar_t wide_name[capacity];
            if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wide_name, capacity))
                set_name(thread, wide_name);
        }
    }
#else
    (void)name;
#endif
}

} // namespace aereborn
