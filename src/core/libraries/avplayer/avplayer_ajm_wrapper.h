// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/libraries/ajm/ajm.h"
#include "core/libraries/ajm/ajm_aac.h"

namespace Libraries::AvPlayer {

class AjmAacDecoder {
public:
    AjmAacDecoder(u32 context_id, u32 channels)
        : m_context_id(context_id), m_batch_buffer(4096), m_current(m_batch_buffer.data()) {
        using namespace Libraries::Ajm;
        AjmInstanceFlags flags{
            .version = 1,
            .channels = channels,
            .format = static_cast<u32>(AjmFormatEncoding::S16),
            .codec = 0,
        };
        sceAjmInstanceCreate(m_context_id, AjmCodecType::M4aacDec, flags, &m_instance_id);
    }

    void Init(u32 sampling_freq_type) {
        using namespace Libraries::Ajm;
        AjmSidebandResult result{};
        AjmSidebandDecM4AacInitParams init_params{
            .config_type = AjmM4AacConfigType::RAW,
            .sampling_freq_type = sampling_freq_type,
        };
        AjmJobFlags flags{
            .version = 1,
            .control_flags = AjmJobControlFlags::Initialize,
        };
        m_current = m_batch_buffer.data();
        m_current = sceAjmBatchJobControlBufferRa(m_current, m_instance_id, flags.raw, &init_params,
                                                  sizeof(init_params), &result, sizeof(result),
                                                  __builtin_return_address(0));
        AjmBatchError error{};
        u32 batch_id{};
        sceAjmBatchStartBuffer(m_context_id, m_batch_buffer.data(),
                               reinterpret_cast<u8*>(m_current) - m_batch_buffer.data(), 40, &error,
                               &batch_id);
        sceAjmBatchWait(m_context_id, batch_id, -1, &error);
    }

    u32 Decode(std::span<const u8> input, std::span<u8> output) {}

    ~AjmAacDecoder() {
        using namespace Libraries::Ajm;
        sceAjmInstanceDestroy(m_context_id, m_instance_id);
    }

private:
    std::vector<u8> m_batch_buffer;
    void* m_current;

    u32 m_context_id = 0;
    u32 m_instance_id = 0;
};

} // namespace Libraries::AvPlayer
