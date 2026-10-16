// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <obs-module.h>
#include <util/platform.h>

// Resolve the public C API from the OBS process. This keeps the Windows build
// independent of an OBS import library; the official SDK headers define every
// function signature and the module's ABI version.
#define VAOBS_API_FUNCTIONS(X) \
    X(obs_register_source_s) X(obs_get_audio_info) X(obs_get_source_by_name) \
    X(obs_source_create_private) X(obs_source_enum_active_sources) X(obs_source_add_active_child) X(obs_source_remove_active_child) \
    X(obs_source_update_properties) \
    X(obs_source_media_get_state) X(obs_property_list_item_disable) \
    X(obs_source_media_get_duration) X(obs_source_media_set_time) X(obs_source_media_play_pause) \
    X(obs_source_set_monitoring_type) \
    X(obs_source_release) X(obs_source_get_name) X(obs_source_removed) \
    X(obs_source_get_output_flags) X(obs_source_get_id) X(obs_source_output_audio) \
    X(obs_source_add_audio_capture_callback) X(obs_source_remove_audio_capture_callback) \
    X(obs_source_get_sync_offset) X(obs_source_get_settings) X(obs_source_update) X(obs_source_media_restart) \
    X(obs_source_get_uuid) X(obs_get_source_by_uuid) X(obs_source_get_unversioned_id) \
    X(obs_source_get_audio_mixers) X(obs_source_set_audio_mixers) \
    X(obs_source_get_volume) X(obs_source_set_volume) X(obs_source_muted) X(obs_source_set_muted) \
    X(obs_data_create) X(obs_data_release) X(obs_data_set_string) X(obs_data_set_bool) X(obs_data_set_int) \
    X(obs_data_get_bool) X(obs_data_unset_user_value) \
    X(obs_data_get_string) X(obs_data_get_int) X(obs_data_get_double) \
    X(obs_data_set_default_string) X(obs_data_set_default_int) X(obs_data_set_default_double) \
    X(obs_properties_create) X(obs_properties_add_list) X(obs_property_list_add_string) X(obs_property_list_clear) \
    X(obs_enum_sources) X(obs_properties_add_int) \
    X(obs_properties_add_text) X(obs_properties_add_button2) X(obs_properties_get) X(obs_property_set_description) \
    X(obs_property_set_long_description) X(obs_property_set_visible) \
    X(os_gettime_ns) X(blog)

namespace obsapi {
#define DECLARE_API(name) inline decltype(&::name) name = nullptr;
VAOBS_API_FUNCTIONS(DECLARE_API)
#undef DECLARE_API
inline bool load()
{
    HMODULE module = GetModuleHandleW(L"obs.dll");
    if (!module) return false;
#define LOAD_API(name) name = reinterpret_cast<decltype(name)>(GetProcAddress(module, #name)); if (!name) return false;
    VAOBS_API_FUNCTIONS(LOAD_API)
#undef LOAD_API
    return true;
}
} // namespace obsapi
