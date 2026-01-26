// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "avplayer.h"

#include <Ap4.h>

#include <string_view>

namespace Libraries::AvPlayer {

class AP4_FileStreamer : public AP4_ByteStream {
public:
    AP4_FileStreamer(const AvPlayerFileReplacement& replacement)
        : m_file_replacement(replacement) {}

    bool Init(std::string_view path) {
        const auto ptr = m_file_replacement.object_ptr;
        m_fd = m_file_replacement.open(ptr, path.data());
        if (m_fd < 0) {
            return false;
        }
        m_file_size = m_file_replacement.size(ptr);
        return true;
    }

    ~AP4_FileStreamer() {
        const auto ptr = m_file_replacement.object_ptr;
        m_file_replacement.close(ptr);
    }

private:
    AP4_Result ReadPartial(void* buffer, AP4_Size bytes_to_read, AP4_Size& bytes_read) override {
        const auto read_offset = m_file_replacement.read_offset;
        const auto ptr = m_file_replacement.object_ptr;
        if (m_position + bytes_to_read > m_file_size) {
            bytes_to_read = m_file_size - m_position;
        }
        bytes_read = read_offset(ptr, reinterpret_cast<u8*>(buffer), m_position, bytes_to_read);
        if (bytes_read == 0 && bytes_to_read != 0) {
            return AP4_ERROR_EOS;
        }
        m_position += bytes_read;
        return AP4_SUCCESS;
    }

    AP4_Result WritePartial(const void* buffer, AP4_Size bytes_to_write,
                            AP4_Size& bytes_written) override {
        return AP4_ERROR_NOT_SUPPORTED;
    }

    AP4_Result Seek(AP4_Position position) override {
        if (position > m_file_size) {
            return AP4_FAILURE;
        }
        m_position = position;
        return AP4_SUCCESS;
    }

    AP4_Result Tell(AP4_Position& position) override {
        position = m_position;
        return AP4_SUCCESS;
    }

    AP4_Result GetSize(AP4_LargeSize& size) override {
        size = m_file_size;
        return AP4_SUCCESS;
    }

    AvPlayerFileReplacement m_file_replacement;

    void AddReference() override {
        ++m_reference_count;
    }

    void Release() override {
        --m_reference_count;
    }

    int m_fd = -1;
    u64 m_position{};
    u64 m_file_size{};
    AP4_Ordinal m_reference_count;
};

} // namespace Libraries::AvPlayer
