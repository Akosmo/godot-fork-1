/**************************************************************************/
/*  audio_stream_player_internal.h                                        */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/object/ref_counted.h"
#include "core/templates/safe_refcount.h"
#include "servers/audio/audio_server_enums.h"

class AudioStream;
class AudioStreamPlayback;
// class AudioSamplePlayback;
class Node;
class RID;

class AudioStreamPlayerInternal : public Object {
	GDCLASS(AudioStreamPlayerInternal, Object);

private:
	struct ParameterData {
		StringName path;
		Variant value;
	};

	static inline const String PARAM_PREFIX = "parameters/";

	// Play/stop callables may need to be reintroduced depending on how AudioStreamPlayer2D/3D starts/stops playback.
	Node *node = nullptr;
	// Callable play_callable;
	// Callable stop_callable;
	bool physical = false;
	// AuSE::PlaybackType playback_type = AuSE::PlaybackType::PLAYBACK_TYPE_DEFAULT;
	RID audio_bus_rid;

	HashMap<StringName, ParameterData> playback_parameters;

	void _set_process(bool p_enabled);
	void _update_stream_parameters();

	bool _is_sample();

public:
	RID emitter_rid;
	RID stream_rid;

	// Maybe these properties aren't needed anymore.
	Ref<AudioStream> stream;
	Vector<Ref<AudioStreamPlayback>> stream_playbacks;

	// SafeFlag active;

	float volume_db = 0.0;
	float pitch_scale = 1.0;
	bool playing = false;
	bool autoplay = false;
	bool stream_paused = false;
	int max_voices = 1;
	StringName audio_bus_name;
	AuSE::PlaybackType playback_type = AuSE::PlaybackType::PLAYBACK_TYPE_DEFAULT;

	void process();
	// void ensure_playback_limit();

	void notification(int p_what);
	void validate_property(PropertyInfo &p_property) const;
	bool set(const StringName &p_name, const Variant &p_value);
	bool get(const StringName &p_name, Variant &r_ret) const;
	void get_property_list(List<PropertyInfo> *p_list) const;

	void set_stream_internal(const Ref<AudioStream> &p_stream);
	Ref<AudioStream> get_stream_internal() const;
	void set_volume_internal(float p_volume);
	float get_volume_internal() const;
	void set_pitch_scale_internal(float p_scale);
	float get_pitch_scale_internal() const;
	void play_internal(float p_time = 0.0);
	void set_playing_internal(bool p_playing);
	bool is_playing_internal() const;
	void set_autoplay_internal(bool p_autoplay);
	bool get_autoplay_internal() const;
	void seek_internal(float p_time);
	float get_playback_position_internal() const;
	void pause_internal(bool p_pause);
	bool is_paused_internal() const;
	void stop_internal();
	void set_max_voices_internal(int p_voices);
	int get_max_voices_internal() const;
	void set_audio_bus_internal(const StringName &p_bus);
	StringName get_audio_bus_internal() const;
	void set_playback_type_internal(AuSE::PlaybackType p_type);
	AuSE::PlaybackType get_playback_type_internal() const;
	bool has_stream_playback_internal() const;
	Ref<AudioStreamPlayback> get_stream_playback_internal() const;

	// void set_stream(Ref<AudioStream> p_stream);
	// Ref<AudioStream> get_stream();
	// void set_pitch_scale(float p_pitch_scale);
	// void set_max_polyphony(int p_max_polyphony);

	// StringName get_bus() const;

	// Ref<AudioStreamPlayback> play_basic();
	// void seek(float p_seconds);
	// void stop_basic();
	// bool is_playing() const;
	// float get_playback_position();

	// void set_playing(bool p_enable);
	// bool is_active() const;

	// void set_stream_paused(bool p_pause);
	// bool get_stream_paused() const;

	// bool has_stream_playback();
	// Ref<AudioStreamPlayback> get_stream_playback();

	// void set_playback_type(AuSE::PlaybackType p_playback_type);
	// AuSE::PlaybackType get_playback_type() const;

	AudioStreamPlayerInternal(Node *p_node, AuSE::EmitterType p_emitter_type, bool p_physical);
};
