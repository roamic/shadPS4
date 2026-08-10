// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <Ap4.h>

#include <memory>

namespace Libraries::AvPlayer {

/// AP4 Byte Stream that reads from a Core::FileSys::IFile handle.
class AP4_HandleStreamer final : public AP4_ByteStream {
public:
    explicit AP4_HandleStreamer(std::unique_ptr<Core::FileSys::IFile> handle)
        : m_handle(std::move(handle)) {}

private:
    AP4_Result ReadPartial(void* buffer, AP4_Size bytes_to_read, AP4_Size& bytes_read) override {
        bytes_read = m_handle->Read(buffer, bytes_to_read);
        return AP4_SUCCESS;
    }

    AP4_Result WritePartial(const void* buffer, AP4_Size bytes_to_write,
                            AP4_Size& bytes_written) override {
        return AP4_ERROR_NOT_SUPPORTED;
    }

    AP4_Result Seek(AP4_Position position) override {
        if (!m_handle->Seek(position, Common::FS::SeekOrigin::SetOrigin)) {
            return AP4_FAILURE;
        }
        return AP4_SUCCESS;
    }

    AP4_Result Tell(AP4_Position& position) override {
        position = static_cast<s64>(m_handle->Tell());
        return AP4_SUCCESS;
    }

    AP4_Result GetSize(AP4_LargeSize& size) override {
        size = m_handle->Size();
        return AP4_SUCCESS;
    }

    void AddReference() override {
        ++m_reference_count;
    }

    void Release() override {
        --m_reference_count;
    }

    std::unique_ptr<Core::FileSys::IFile> m_handle;
    AP4_Ordinal m_reference_count;
};

} // namespace Libraries::AvPlayer
