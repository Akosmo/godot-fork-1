/**************************************************************************/
/*  audio_server.h                                                        */
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

#include "core/templates/rid_owner.h"
#include "core/templates/safe_list.h"
#include "core/variant/variant.h"
#include "servers/audio/audio_bus_layout.h"
#include "servers/audio/audio_effect.h"
#include "servers/audio/audio_filter_sw.h"
#include "servers/audio/audio_frame.h"
#include "servers/audio/audio_server_constants.h"
#include "servers/audio/audio_server_enums.h"
#include "servers/audio/audio_server_types.h"

class AudioSample;
class AudioStream;
class AudioStreamPlayback;
class AudioSamplePlayback;
class AudioBusLayout;

class AudioServer : public Object {
	GDCLASS(AudioServer, Object);

	// public:
	// 	typedef void (*AudioCallback)(void *p_userdata);

private:
	uint64_t mix_time = 0;
	int mix_size = 0;

	uint32_t buffer_size = 0;
	uint64_t mix_count = 0;
	uint64_t mix_frames = 0;
#ifdef DEBUG_ENABLED
	SafeNumeric<uint64_t> prof_time;
#endif

	float channel_disable_threshold_db = 0.0f;
	uint32_t channel_disable_frames = 0;

	int channel_pair_count = 0;
	int to_mix = 0;

	float playback_speed_scale = 1.0f;

	bool tag_used_audio_streams = false;

#ifdef DEBUG_ENABLED
	bool debug_mute = false;
#endif // DEBUG_ENABLED

	bool input_device_active = false;
	int input_buffer_ofs = 0;

	mutable RID_Owner<AuST::AudioEmitter> emitter_owner;
	mutable RID_Owner<AuST::AudioListener> listener_owner;
	mutable RID_Owner<AuST::AudioEmitterStream> stream_owner;
	mutable RID_Owner<AuST::AudioBus> audio_bus_owner;
	mutable RID_Owner<AuST::BusEffect> effect_owner;

	Vector<AuST::AudioEmitter *> audio_emitter_list;
	Vector<AuST::AudioEmitterStream *> emitter_stream_list;
	Vector<AuST::AudioListener *> audio_listener_list;

	// void _cleanup_lists();

	Vector<Vector<AudioFrame>> temp_buffer;
	Vector<AudioFrame> mix_buffer;
	Vector<AuST::AudioBus *> audio_buses;
	HashMap<RID, AuST::AudioBus *> audio_bus_map;

	void _update_bus_effects(RID p_bus);

	static AudioServer *singleton;

	void init_channels_and_buffers();

	void _mix_step();
	void _mix_step_for_channel_pair(AudioFrame *p_out_buf, AudioFrame *p_source_buf, AudioFrame p_vol_start, AudioFrame p_vol_final);

	// TODO: CallbackItem seems to be legacy stuff. If possible, reimplement this. Otherwise, just bring it back.

	// struct CallbackItem {
	// 	AudioCallback callback;
	// 	void *userdata = nullptr;
	// };

	// SafeList<CallbackItem *> update_callback_list;
	// SafeList<CallbackItem *> mix_callback_list;
	// SafeList<CallbackItem *> listener_changed_callback_list;

	friend class AudioDriver;
	void _driver_process(int p_frames, int32_t *p_buffer);

	LocalVector<Ref<AudioSamplePlayback>> sample_playback_list;

protected:
	static void _bind_methods();

public:
	_FORCE_INLINE_ int get_channel_pair_count() const {
		switch (get_speaker_mode()) {
			case AuSE::SPEAKER_MODE_STEREO:
				return 1;
			case AuSE::SPEAKER_SURROUND_31:
				return 2;
			case AuSE::SPEAKER_SURROUND_51:
				return 3;
			case AuSE::SPEAKER_SURROUND_71:
				return 4;
		}
		ERR_FAIL_V(1);
	}

	AudioFrame *get_channel_pair_mix_buffer(RID p_bus, int p_buffer);

#ifdef DEBUG_ENABLED
	void set_debug_mute(bool p_mute);
	bool get_debug_mute() const;
#endif // DEBUG_ENABLED

	// New methods:
	// TODO: Rename all methods.
	// TODO: Figure out how index will be used for audio buses. Ideally, index shouldn't matter in this new framework, but compat must remain unbroken.
	// TODO: Add method for swapping buses, and method for moving effects.
	// TODO: Consider adding method to get bus RID by name.

	// Emitter

	RID create_emitter(AuSE::EmitterType p_type);
	Error remove_emitter(RID p_emitter);
	bool emitter_exists(RID p_emitter) const;
	bool is_emitter_active(RID p_emitter) const;
	AuSE::EmitterType get_emitter_type(RID p_emitter) const;
	RID add_stream(RID p_emitter, const Ref<AudioStream> &p_stream);
	Error remove_stream(RID p_stream);
	bool stream_exists(RID p_stream) const;
	Error clear_streams(RID p_emitter);
	Error emitter_update_position(RID p_emitter, Variant p_position);

	// Emitter: Stage

	Error set_area_mask(RID p_emitter, int p_area_mask);
	uint8_t get_area_mask(RID p_emitter) const;
	Error set_attenuation_exponent(RID p_emitter, float p_exponent);
	float get_attenuation_exponent(RID p_emitter) const;
	Error set_attenuation_filter_cutoff(RID p_emitter, float p_cutoff);
	float get_attenuation_filter_cutoff(RID p_emitter) const;
	Error set_attenuation_filter_gain(RID p_emitter, float p_gain);
	float get_attenuation_filter_gain(RID p_emitter) const;
	Error set_attenuation_model(RID p_emitter, AuSE::AttenuationModel p_model);
	AuSE::AttenuationModel get_attenuation_model(RID p_emitter) const;
	Error set_doppler_tracking(RID p_emitter, AuSE::DopplerTracking p_doppler_tracking);
	AuSE::DopplerTracking get_doppler_tracking(RID p_emitter) const;
	Error set_emission_angle(RID p_emitter, float p_angle);
	float get_emission_angle(RID p_emitter) const;
	Error set_emission_angle_filter_attenuation(RID p_emitter, float p_emission_angle_filter_attenuation);
	float get_emission_angle_filter_attenuation(RID p_emitter) const;
	Error set_max_volume(RID p_emitter, float p_volume);
	float get_max_volume(RID p_emitter) const;
	Error set_max_distance(RID p_emitter, float p_distance);
	float get_max_distance(RID p_emitter) const;
	Error set_panning_strength(RID p_emitter, float p_strength);
	float get_panning_strength(RID p_emitter) const;
	Error set_unit_size(RID p_emitter, float p_size);
	float get_unit_size(RID p_emitter) const;

	// Emitter (contd.)

	Error set_max_voices(RID p_emitter, uint8_t p_voices);
	uint8_t get_max_voices(RID p_emitter) const;
	Error set_polyphony_mode(RID p_emitter, AuSE::PolyphonyMode p_mode);
	AuSE::PolyphonyMode get_polyphony_mode(RID p_emitter) const;
	Error set_emitter_playback_type(RID p_emitter, AuSE::PlaybackType p_type);
	AuSE::PlaybackType get_emitter_playback_type(RID p_emitter) const;
	Error set_mix_target(RID p_emitter, AuSE::MixTarget p_target);
	AuSE::MixTarget get_mix_target(RID p_emitter) const;

	// Listener

	RID create_listener(AuSE::ListenerType p_type, bool p_active = false);
	Error remove_listener(RID p_listener);
	bool listener_exists(RID p_listener) const;
	AuSE::ListenerType get_listener_type(RID p_listener) const;
	Error set_active(RID p_listener, bool p_active);
	bool is_active(RID p_listener) const;
	Error listener_update_position(RID p_listener, Variant p_position);
	Error set_listener_doppler_tracking(RID p_listener, AuSE::DopplerTracking p_doppler_tracking);
	AuSE::DopplerTracking get_listener_doppler_tracking(RID p_listener) const;
	Error listener_update_transform(RID p_listener, Transform3D p_transform);
	Transform3D get_listener_transform(RID p_listener) const;
	RID get_active_listener() const;

	// Stream

	Error play_stream(RID p_stream, float p_time = 0.0);
	Error seek_stream(RID p_stream, float p_time);
	float get_stream_playback_position(RID p_stream) const;
	Error pause_stream(RID p_stream, bool p_pause);
	bool is_stream_paused(RID p_stream) const;
	Error stop_stream(RID p_stream);
	bool is_stream_active(RID p_stream) const;
	Error set_stream_volume(RID p_stream, float p_volume);
	float get_stream_volume(RID p_stream) const;
	Error set_stream_pitch_scale(RID p_stream, float p_scale);
	float get_stream_pitch_scale(RID p_stream) const;
	Error set_stream_audio_bus(RID p_stream, RID p_bus);
	RID get_stream_audio_bus(RID p_stream) const;
	Ref<AudioStreamPlayback> get_stream_playback(RID p_stream) const;
	bool has_stream_playback(RID p_stream) const;

	// Bus

	RID add_audio_bus(int p_index = -1);
	Error remove_audio_bus(RID p_bus);
	Error move_audio_bus(RID p_bus, int p_to_index);
	bool audio_bus_exists(RID p_bus) const;
	int get_audio_bus_count() const;
	RID get_audio_bus_rid(int p_index) const;
	RID get_audio_bus_rid_by_name(const String &p_name) const;
	int get_audio_bus_index(RID p_bus) const;
	Error set_audio_bus_name(RID p_bus, const String &p_name);
	String get_audio_bus_name(RID p_bus) const;
	Error set_audio_bus_solo(RID p_bus, bool p_solo);
	bool is_audio_bus_soloed(RID p_bus) const;
	Error set_audio_bus_mute(RID p_bus, bool p_mute);
	bool is_audio_bus_muted(RID p_bus) const;
	Error set_audio_bus_bypass(RID p_bus, bool p_bypass);
	bool is_audio_bus_bypassed(RID p_bus) const;
	Error set_audio_bus_volume(RID p_bus, float p_volume);
	float get_audio_bus_volume(RID p_bus) const;
	RID add_audio_bus_effect(RID p_bus, const Ref<AudioEffect> &p_effect, int p_at_index = -1);
	Error remove_audio_bus_effect(RID p_effect);
	Error clear_audio_bus_effects(RID p_bus);
	Ref<AudioEffect> get_audio_bus_effect(RID p_effect) const;
	Ref<AudioEffectInstance> get_audio_bus_effect_instance(RID p_effect, int p_channel_pair = 0) const;
	Error set_audio_bus_effect_enable(RID p_effect, bool p_enable);
	bool is_audio_bus_effect_enabled(RID p_effect) const;
	RID get_audio_bus_effect_rid(RID p_bus, int p_effect) const;
	int get_audio_bus_effect_index(RID p_effect) const;
	int get_audio_bus_effect_count(RID p_bus) const;
	Error swap_audio_bus_effects(RID p_bus, int p_index_1, int p_index_2);
	Error set_audio_bus_send(RID p_bus, RID p_send);
	RID get_audio_bus_send(RID p_bus) const;
	AudioFrame get_audio_bus_peak_volume(RID p_bus, int p_channel_pair) const;

private:
	Vector<AudioFrame> _get_volume_vector(RID p_emitter);
	void _update_stage(RID p_emitter);
	Error _set_master_audio_bus(RID p_bus, bool p_enable);
	RID _get_master_audio_bus() const;

	// End of new methods.

public:
	// void set_bus_count(int p_count);
	int get_bus_count() const;

	void remove_bus(int p_index);
	void add_bus(int p_at_pos = -1);

	void move_bus(int p_bus, int p_to_pos);

	void set_bus_name(int p_bus, const String &p_name);
	String get_bus_name(int p_bus) const;
	int get_bus_index(const StringName &p_bus_name) const;

	int get_bus_channels(int p_bus) const;

	void set_bus_volume_db(int p_bus, float p_volume_db);
	float get_bus_volume_db(int p_bus) const;

	void set_bus_volume_linear(int p_bus, float p_volume_linear);
	float get_bus_volume_linear(int p_bus) const;

	void set_bus_send(int p_bus, const StringName &p_send);
	StringName get_bus_send(int p_bus) const;

	void set_bus_solo(int p_bus, bool p_enable);
	bool is_bus_solo(int p_bus) const;

	void set_bus_mute(int p_bus, bool p_enable);
	bool is_bus_mute(int p_bus) const;

	void set_bus_bypass_effects(int p_bus, bool p_enable);
	bool is_bus_bypassing_effects(int p_bus) const;

	void add_bus_effect(int p_bus, const Ref<AudioEffect> &p_effect, int p_at_pos = -1);
	void remove_bus_effect(int p_bus, int p_effect);

	int get_bus_effect_count(int p_bus);
	Ref<AudioEffect> get_bus_effect(int p_bus, int p_effect);
	Ref<AudioEffectInstance> get_bus_effect_instance(int p_bus, int p_effect, int p_channel = 0);

	void swap_bus_effects(int p_bus, int p_effect, int p_by_effect);

	void set_bus_effect_enabled(int p_bus, int p_effect, bool p_enabled);
	bool is_bus_effect_enabled(int p_bus, int p_effect) const;

	float get_bus_peak_volume_left_db(int p_bus, int p_channel) const;
	float get_bus_peak_volume_right_db(int p_bus, int p_channel) const;

	// bool is_bus_channel_active(int p_bus, int p_channel) const;

	void set_playback_speed_scale(float p_scale);
	float get_playback_speed_scale() const;

	// // Convenience method.
	// void start_playback_stream(Ref<AudioStreamPlayback> p_playback, const StringName &p_bus, Vector<AudioFrame> p_volume_db_vector, float p_start_time = 0, float p_pitch_scale = 1);
	// // Expose all parameters.
	// void start_playback_stream(Ref<AudioStreamPlayback> p_playback, const HashMap<StringName, Vector<AudioFrame>> &p_bus_volumes, float p_start_time = 0, float p_pitch_scale = 1, float p_highshelf_gain = 0, float p_attenuation_cutoff_hz = 0);
	// void stop_playback_stream(Ref<AudioStreamPlayback> p_playback);

	// void set_playback_bus_exclusive(Ref<AudioStreamPlayback> p_playback, const StringName &p_bus, Vector<AudioFrame> p_volumes);
	// void set_playback_bus_volumes_linear(Ref<AudioStreamPlayback> p_playback, const HashMap<StringName, Vector<AudioFrame>> &p_bus_volumes);
	// void set_playback_all_bus_volumes_linear(Ref<AudioStreamPlayback> p_playback, Vector<AudioFrame> p_volumes);
	// void set_playback_pitch_scale(Ref<AudioStreamPlayback> p_playback, float p_pitch_scale);
	// void set_playback_paused(Ref<AudioStreamPlayback> p_playback, bool p_paused);
	// void set_playback_highshelf_params(Ref<AudioStreamPlayback> p_playback, float p_gain, float p_attenuation_cutoff_hz);

	// bool is_playback_active(Ref<AudioStreamPlayback> p_playback);
	// float get_playback_position(Ref<AudioStreamPlayback> p_playback);
	// bool is_playback_paused(Ref<AudioStreamPlayback> p_playback);

	uint64_t get_mix_count() const;
	uint64_t get_mixed_frames() const;

	String get_driver_name() const;

	// void notify_listener_changed();

	virtual void init();
	virtual void finish();
	virtual void update();
	// virtual void load_default_bus_layout();

	/* MISC config */

	virtual void lock();
	virtual void unlock();

	virtual AuSE::SpeakerMode get_speaker_mode() const;
	virtual float get_mix_rate() const;
	virtual float get_input_mix_rate() const;

	virtual float read_output_peak_db() const;

	static AudioServer *get_singleton();

	virtual double get_output_latency() const;
	virtual double get_time_to_next_mix() const;
	virtual double get_time_since_last_mix() const;

	// void add_listener_changed_callback(AudioCallback p_callback, void *p_userdata);
	// void remove_listener_changed_callback(AudioCallback p_callback, void *p_userdata);

	// void add_update_callback(AudioCallback p_callback, void *p_userdata);
	// void remove_update_callback(AudioCallback p_callback, void *p_userdata);

	// void add_mix_callback(AudioCallback p_callback, void *p_userdata);
	// void remove_mix_callback(AudioCallback p_callback, void *p_userdata);

	// void set_bus_layout(const Ref<AudioBusLayout> &p_bus_layout);
	// Ref<AudioBusLayout> generate_bus_layout() const;

	PackedStringArray get_output_device_list();
	String get_output_device();
	void set_output_device(const String &p_name);

	PackedStringArray get_input_device_list();
	String get_input_device();
	void set_input_device(const String &p_name);
	Error set_input_device_active(bool p_is_active);
	int get_input_frames_available();
	int get_input_buffer_length_frames();
	PackedVector2Array get_input_frames(int p_frames);

	void set_enable_tagging_used_audio_streams(bool p_enable);

#ifdef TOOLS_ENABLED
	virtual void get_argument_options(const StringName &p_function, int p_idx, List<String> *r_options) const override;
#endif

	AuSE::PlaybackType get_default_playback_type() const;

	bool is_stream_registered_as_sample(const Ref<AudioStream> &p_stream);
	void register_stream_as_sample(const Ref<AudioStream> &p_stream);
	void unregister_stream_as_sample(const Ref<AudioStream> &p_stream);
	void register_sample(const Ref<AudioSample> &p_sample);
	void unregister_sample(const Ref<AudioSample> &p_sample);
	void start_sample_playback(const Ref<AudioSamplePlayback> &p_playback);
	void stop_sample_playback(const Ref<AudioSamplePlayback> &p_playback);
	void set_sample_playback_pause(const Ref<AudioSamplePlayback> &p_playback, bool p_paused);
	bool is_sample_playback_active(const Ref<AudioSamplePlayback> &p_playback);
	double get_sample_playback_position(const Ref<AudioSamplePlayback> &p_playback);
	void update_sample_playback_pitch_scale(const Ref<AudioSamplePlayback> &p_playback, float p_pitch_scale = 0.0f);

	AudioServer();
	virtual ~AudioServer();
};

VARIANT_ENUM_CAST_EXT(AuSE::SpeakerMode, AudioServer::SpeakerMode);
VARIANT_ENUM_CAST_EXT(AuSE::PlaybackType, AudioServer::PlaybackType);
