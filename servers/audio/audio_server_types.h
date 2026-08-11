/**************************************************************************/
/*  audio_server_types.h                                                  */
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

#include "core/templates/rid.h"
//#include "core/variant/variant.h"
#include "servers/audio/audio_effect.h"
#include "servers/audio/audio_frame.h"
#include "servers/audio/audio_server_enums.h"

class AudioStream;
class AudioStreamPlayback;

namespace AudioServerTypes {

// TODO: Some of these structs have placeholder names, which should be replaced by shorter names, like the current ones in audio_server.h

// Consider using RID for this.
struct AudioStage {
	AuSE::StageType type;
	uint8_t area_mask;
	float attenuation_exponent;
	// TODO: Allow changing filter type between high-shelf and low-pass.
	float attenuation_filter_cutoff;
	float attenuation_filter_gain;
	AuSE::AttenuationModel attenuation_model;
	AuSE::DopplerTracking doppler_tracking;
	float emission_angle;
	float emission_angle_filter_attenuation;
	float max_volume;
	float max_distance;
	float panning_strength;
	float unit_size;
};

struct BusEffect {
	RID rid;
	RID audio_bus_rid;
	Ref<AudioEffect> effect;
	bool enabled;
#ifdef DEBUG_ENABLED
	uint64_t prof_time;
#endif // DEBUG_ENABLED
};

struct BusChannelPair {
	bool active;
	Vector<AudioFrame> buffer;
	Vector<Ref<AudioEffectInstance>> effect_instances;
	AudioFrame peak_volume_linear;
	uint64_t last_mix_with_audio;
};

struct AudioBus {
	RID rid;
	StringName name;
	bool master_bus;
	bool active;
	bool soloed;
	bool muted;
	bool bypassed;
	float volume_db;
	Vector<BusEffect *> effects;
	RID send; // TODO: Allow sending to multiple buses with custom volume level sent to each.
	Vector<BusChannelPair> channel_pairs;
};

struct AudioEmitterStream {
	RID rid;
	RID emitter_rid;
	Ref<AudioStream> stream;
	Ref<AudioStreamPlayback> stream_playback;
	Vector<Ref<AudioStreamPlayback>> active_playback_voices;
	bool active;
	float volume_db;
	float pitch_scale;
	// TODO: Add pan property.
	AudioBus *bus;
	AuSE::PlaybackState state;
	AudioFrame lookahead[AuSC::LOOKAHEAD_BUFFER_SIZE];
};

struct AudioEmitter {
	RID rid;
	bool active;
	AuSE::EmitterType type;
	Vector<AudioEmitterStream *> streams;
	Vector<AudioFrame> volume_vector;
	AudioStage stage;
	uint8_t max_voices;
	AuSE::PolyphonyMode polyphony_mode;
	AuSE::PlaybackType playback_type;
	AuSE::MixTarget mix_target;
};

struct AudioListener {
	RID rid;
	AuSE::ListenerType type;
	bool active;
	AuSE::DopplerTracking doppler_tracking;
	Transform3D transform;
};

#ifndef DISABLE_DEPRECATED
// Graveyard.
#endif

} // namespace AudioServerTypes

// Alias to make it easier to use.
#define AuST AudioServerTypes
