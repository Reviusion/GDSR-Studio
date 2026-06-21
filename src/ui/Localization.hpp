#pragma once

namespace Localization {

struct Strings {
    const char* menu_file;
    const char* menu_edit;
    const char* menu_view;
    const char* menu_profile;
    const char* menu_scenes;
    const char* menu_tools;
    const char* menu_help;

    const char* file_settings;
    const char* file_show_recordings;
    const char* file_exit;

    const char* edit_undo;
    const char* edit_redo;
    const char* edit_transform;

    const char* view_fullscreen;
    const char* view_reset_layout;
    const char* view_panels;

    const char* panel_preview;
    const char* panel_scenes;
    const char* panel_sources;
    const char* panel_mixer;
    const char* panel_transitions;
    const char* panel_controls;

    const char* tools_wizard;
    const char* help_about;

    const char* ctrl_start_record;
    const char* ctrl_stop_record;
    const char* ctrl_settings;
    const char* ctrl_saving;

    const char* trans_duration;

    const char* settings_title;
    const char* settings_appearance;
    const char* settings_theme;
    const char* settings_theme_dark;
    const char* settings_theme_light;
    const char* settings_language;
    const char* settings_font_family;
    const char* settings_font_size;
    const char* settings_close;

    const char* about_title;
    const char* about_body;
    const char* about_hotkey;

    const char* status_toggle_hint;
    const char* notify_startup;
    const char* notify_hint;

    const char* recording_fps;
    const char* recording_resolution;
    const char* recording_quality;
    const char* recording_encoder;
    const char* recording_ffmpeg_path;
    const char* recording_output_dir;

    const char* rec_section_video;
    const char* rec_section_output;
    const char* rec_res_same;
    const char* rec_quality_best;
    const char* rec_quality_good;
    const char* rec_quality_medium;
    const char* rec_quality_fast;
    const char* rec_no_hw_encoder;

    const char* mixer_mute;
    const char* mixer_unmute;

    const char* scene_add;
    const char* scene_duplicate;
    const char* scene_reset;
    const char* source_new;

    const char* audio_desktop;
    const char* audio_mic;
    const char* audio_enable;
    const char* audio_device;
    const char* audio_default_dev;
    const char* audio_bitrate;
    const char* audio_section;

    // ---- new fields (previously hardcoded) ----
    const char* perf_potato;
    const char* perf_low;
    const char* perf_balanced;
    const char* perf_quality;

    const char* audio_refresh_devices;

    const char* settings_performance;
    const char* settings_threaded_capture;
    const char* settings_threaded_capture_desc;
    const char* settings_borrow_fps;
    const char* settings_borrow_fps_desc;
    const char* notify_recording_started;
    const char* notify_recording_hint;

    // ---- encoder quality/performance + dual audio tracks ----
    const char* recording_encode_mode;   // label
    const char* encode_mode_items;        // combo: Max Performance\0Balanced\0Max Quality\0
    const char* recording_rate_control;   // label
    const char* rate_control_items;       // combo: Quality\0Bitrate\0
    const char* recording_bitrate;        // label (kbps)
    const char* audio_tracks;             // label
    const char* audio_tracks_items;       // combo: Single\0Dual\0
    const char* recording_full_range;     // checkbox: full-range colour
    const char* recording_chroma444;      // checkbox: 4:4:4 chroma
};

int count();
const char* nativeName(int i);
void setLanguage(int index);
int  language();
const Strings& L();

} // namespace Localization
