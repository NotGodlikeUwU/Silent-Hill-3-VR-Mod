#pragma once

#include <windows.h>

#include <cstddef>
#include <cstring>

// Copies a stereo pair with one eye on the caller and the other on the
// Windows thread pool.  Only ordinary CPU memory is touched by the worker;
// graphics API objects remain confined to their original render thread.
// This is important for the legacy D3D8 device, whose mutable state cannot be
// recorded safely from two threads.
class Sh3VrParallelCopyWorker
{
public:
    Sh3VrParallelCopyWorker() = default;
    Sh3VrParallelCopyWorker(const Sh3VrParallelCopyWorker&) = delete;
    Sh3VrParallelCopyWorker& operator=(const Sh3VrParallelCopyWorker&) = delete;

    bool CopyPair(void* destination0, std::size_t destinationPitch0,
        const void* source0, std::size_t sourcePitch0,
        void* destination1, std::size_t destinationPitch1,
        const void* source1, std::size_t sourcePitch1,
        std::size_t rowBytes, std::size_t rowCount, bool enableMulticore)
    {
        if (!destination0 || !source0 || !destination1 || !source1 ||
            rowBytes == 0 || rowCount == 0)
        {
            return false;
        }

        const std::size_t totalBytes = rowBytes * rowCount;
        if (!enableMulticore || totalBytes < kMinimumParallelBytes ||
            !EnsureWorker())
        {
            CopyRows(destination0, destinationPitch0, source0, sourcePitch0,
                rowBytes, rowCount);
            CopyRows(destination1, destinationPitch1, source1, sourcePitch1,
                rowBytes, rowCount);
            return false;
        }

        m_destination = destination1;
        m_destinationPitch = destinationPitch1;
        m_source = source1;
        m_sourcePitch = sourcePitch1;
        m_rowBytes = rowBytes;
        m_rowCount = rowCount;
        ResetEvent(m_completeEvent);
        MemoryBarrier();
        SubmitThreadpoolWork(m_work);

        CopyRows(destination0, destinationPitch0, source0, sourcePitch0,
            rowBytes, rowCount);
        WaitForSingleObject(m_completeEvent, INFINITE);
        return true;
    }

    void Shutdown()
    {
        if (m_work)
        {
            WaitForThreadpoolWorkCallbacks(m_work, TRUE);
            CloseThreadpoolWork(m_work);
            m_work = nullptr;
        }
        if (m_completeEvent)
        {
            CloseHandle(m_completeEvent);
            m_completeEvent = nullptr;
        }
    }

private:
    static constexpr std::size_t kMinimumParallelBytes = 1024u * 1024u;

    static void CopyRows(void* destination, std::size_t destinationPitch,
        const void* source, std::size_t sourcePitch, std::size_t rowBytes,
        std::size_t rowCount)
    {
        auto* destinationBytes = static_cast<unsigned char*>(destination);
        const auto* sourceBytes = static_cast<const unsigned char*>(source);
        if (destinationPitch == rowBytes && sourcePitch == rowBytes)
        {
            std::memcpy(destinationBytes, sourceBytes, rowBytes * rowCount);
            return;
        }
        for (std::size_t row = 0; row < rowCount; ++row)
        {
            std::memcpy(destinationBytes + row * destinationPitch,
                sourceBytes + row * sourcePitch, rowBytes);
        }
    }

    static void CALLBACK WorkCallback(PTP_CALLBACK_INSTANCE,
        void* context, PTP_WORK)
    {
        auto* self = static_cast<Sh3VrParallelCopyWorker*>(context);
        MemoryBarrier();
        CopyRows(self->m_destination, self->m_destinationPitch,
            self->m_source, self->m_sourcePitch, self->m_rowBytes,
            self->m_rowCount);
        MemoryBarrier();
        SetEvent(self->m_completeEvent);
    }

    bool EnsureWorker()
    {
        if (m_work && m_completeEvent)
            return true;
        if (m_initializationFailed)
            return false;

        m_completeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!m_completeEvent)
        {
            m_initializationFailed = true;
            return false;
        }
        m_work = CreateThreadpoolWork(&WorkCallback, this, nullptr);
        if (!m_work)
        {
            CloseHandle(m_completeEvent);
            m_completeEvent = nullptr;
            m_initializationFailed = true;
            return false;
        }
        return true;
    }

    PTP_WORK m_work = nullptr;
    HANDLE m_completeEvent = nullptr;
    bool m_initializationFailed = false;
    void* m_destination = nullptr;
    std::size_t m_destinationPitch = 0;
    const void* m_source = nullptr;
    std::size_t m_sourcePitch = 0;
    std::size_t m_rowBytes = 0;
    std::size_t m_rowCount = 0;
};
