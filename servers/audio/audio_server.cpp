/**************************************************************************/
/*  audio_server.cpp                                                      */
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

#include "audio_server.h"

#include "core/config/project_settings.h"
#include "core/debugger/engine_debugger.h"
#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "scene/resources/audio/audio_stream.h"
#include "scene/scene_string_names.h"
#include "servers/audio/audio_bus_layout.h"
#include "servers/audio/audio_driver.h"
#include "servers/audio/audio_frame.h"
#include "servers/audio/effects/audio_effect_compressor.h"

#ifdef TOOLS_ENABLED
#define MARK_EDITED set_edited(true);
#else
#define MARK_EDITED
#endif

void AudioServer::_driver_process(int p_frames, int32_t *p_buffer) {
	mix_count++;
	int todo = p_frames;

#ifdef DEBUG_ENABLED
	uint64_t prof_ticks = OS::get_singleton()->get_ticks_usec();
#endif

	if (channel_pair_count != get_channel_pair_count()) {
		// Amount of channels changed due to a output_device change
		// reinitialize the buses channels and buffers
		init_channels_and_buffers();
	}

	ERR_FAIL_COND_MSG(audio_buses.is_empty() && todo, "AudioServer bus count is less than 1.");
	while (todo) {
		if (to_mix == 0) {
			_mix_step();
		}

		int to_copy = MIN(to_mix, todo);

		AuST::AudioBus *master;
		for (AuST::AudioBus *audio_bus : audio_buses) {
			if (audio_bus->master_bus) {
				master = audio_bus;
				break;
			}
		}

		int from = buffer_size - to_mix;
		int from_buf = p_frames - todo;

		//master master, send to output
		int cs = master->channel_pairs.size();

		// Take away 1 from the stride, as we are manually incrementing by 1 for stereo.
		uintptr_t stride_minus_one = (cs * 2) - 1;

		for (int k = 0; k < cs; k++) {
			// The destination start for data will be the same in all cases.
			int32_t *dest = &p_buffer[from_buf * (cs * 2) + (k * 2)];

#ifdef DEBUG_ENABLED
			if (!debug_mute && master->channel_pairs[k].active) {
#else
			if (master->channels[k].active) {
#endif // DEBUG_ENABLED
				const AudioFrame *buf = master->channel_pairs[k].buffer.ptr();

				for (int j = 0; j < to_copy; j++) {
					float l = CLAMP(buf[from + j].left, -1.0, 1.0);
					int32_t vl = l * ((1 << 20) - 1);
					int32_t vl2 = (vl < 0 ? -1 : 1) * (Math::abs(vl) << 11);
					*dest = vl2;
					dest++;

					float r = CLAMP(buf[from + j].right, -1.0, 1.0);
					int32_t vr = r * ((1 << 20) - 1);
					int32_t vr2 = (vr < 0 ? -1 : 1) * (Math::abs(vr) << 11);
					*dest = vr2;
					dest += stride_minus_one;
				}

			} else {
				// Bizarrely, profiling indicates that detecting the common case of cs == 1,
				// k == 0, and using memset is SLOWER than setting them individually.
				// Perhaps it gets optimized to a faster instruction than memset.
				for (int j = 0; j < to_copy; j++) {
					*dest = 0;
					dest++;
					*dest = 0;
					dest += stride_minus_one;
				}
			}
		}

		todo -= to_copy;
		to_mix -= to_copy;
	}

#ifdef DEBUG_ENABLED
	prof_time.add(OS::get_singleton()->get_ticks_usec() - prof_ticks);
#endif
}

void AudioServer::_mix_step() {
	bool solo_mode = false;

	for (int i = 0; i < audio_buses.size(); i++) {
		AuST::AudioBus *audio_bus = audio_buses[i];
		// bus->index_cache = i; // Might be moved around by editor, so...
		// for (int k = 0; k < bus->channels.size(); k++) {
		// 	bus->channels.write[k].used = false;
		// }

		if (audio_bus->soloed) {
			//solo chain
			solo_mode = true;
			// bus->soloed = true;
			do {
				if (audio_bus != audio_buses[0]) {
					// Everything has a send, save for master bus.
					if (!audio_bus_map.has(audio_bus->send)) {
						audio_bus = audio_buses[0]; // Send to master.
					}
					// } else {
					// 	int prev_index_cache = bus->index_cache;
					// 	bus = bus_map[bus->send];
					// 	if (prev_index_cache >= bus->index_cache) { //invalid, send to master
					// 		bus = buses[0];
					// 	}
					// }

					// bus->soloed = true;
				} else {
					audio_bus = nullptr;
				}

			} while (audio_bus);
		}
		// } else {
		// 	bus->soloed = false;
		// }
	}

	for (AuST::AudioEmitter *audio_emitter : audio_emitter_list) {
		if (!audio_emitter->active) {
			continue;
		}

		for (AuST::AudioEmitterStream *emitter_stream : audio_emitter->streams) {
			if (!emitter_stream->active) {
				continue;
			}

			if (emitter_stream->state == AuSE::PlaybackState::PAUSED) {
				continue;
			}

			if (emitter_stream->stream_playback->get_is_sample()) {
				continue;
			}

			bool fading_out = emitter_stream->state == AuSE::PlaybackState::FADE_OUT_TO_DEACTIVATION || emitter_stream->state == AuSE::PlaybackState::FADE_OUT_TO_PAUSE;

			AudioFrame *buf = mix_buffer.ptrw();

			for (int i = 0; i < AuSC::LOOKAHEAD_BUFFER_SIZE; i++) {
				buf[i] = emitter_stream->lookahead[i];
			}

			unsigned int mixed_frames = emitter_stream->stream_playback->mix(&buf[AuSC::LOOKAHEAD_BUFFER_SIZE], emitter_stream->pitch_scale, buffer_size);

			if (tag_used_audio_streams && emitter_stream->stream_playback->is_playing()) {
				emitter_stream->stream_playback->tag_used_streams();
			}

			if (mixed_frames != buffer_size) {
				float fadeout_base = 0.94;
				float fadeout_coefficient = 1;
				static_assert(AuSC::LOOKAHEAD_BUFFER_SIZE == 64, "Update fadeout_base and comment here if you change AuSC::LOOKAHEAD_BUFFER_SIZE.");
				for (unsigned int idx = mixed_frames; idx < buffer_size; idx++) {
					fadeout_coefficient *= fadeout_base;
					buf[idx] *= fadeout_coefficient;
				}
				emitter_stream->state = AuSE::PlaybackState::AWAITING_DEACTIVATION;
			} else {
				for (int i = 0; i < AuSC::LOOKAHEAD_BUFFER_SIZE; i++) {
					emitter_stream->lookahead[i] = buf[buffer_size + i];
				}
			}

			for (int channel_pair_idx = 0; channel_pair_idx < channel_pair_count; channel_pair_idx++) {
				AudioFrame *channel_buf = get_channel_pair_mix_buffer(emitter_stream->bus->rid, channel_pair_idx);

				AudioFrame channel_vol;
				// TODO: This `fading_out` check could be replaced with with an exponential fadeout of the samples from the lookahead buffer for more punchy results.
				if (fading_out) {
					channel_vol = AudioFrame(0, 0);
				} else {
					channel_vol = audio_emitter->volume_vector[channel_pair_idx];
				}
				AudioFrame prev_channel_vol = channel_vol;

				_mix_step_for_channel_pair(channel_buf, buf, prev_channel_vol, channel_vol);
			}

			switch (emitter_stream->state) {
				case AuSE::PlaybackState::PLAYING:
				case AuSE::PlaybackState::PAUSED:
					break;
				case AuSE::PlaybackState::FADE_OUT_TO_PAUSE:
					emitter_stream->state = AuSE::PlaybackState::PAUSED;
					break;
				case AuSE::PlaybackState::FADE_OUT_TO_DEACTIVATION:
				case AuSE::PlaybackState::AWAITING_DEACTIVATION:
					emitter_stream->active == false; // TODO: Make sure this is all... at least stop the emitter too, if this is the only stream.
					break;
			}
		}
	}

	// Now that all of the buses have their audio sources mixed into them, we can process the effects and bus sends.
	for (int i = audio_buses.size() - 1; i >= 0; i--) {
		AuST::AudioBus *audio_bus = audio_buses[i];

		AuST::BusChannelPair *channels_ptrw = audio_bus->channel_pairs.ptrw();
		Vector<AudioFrame> *temp_buffer_ptrw = temp_buffer.ptrw();

		// Process effects.
		if (!audio_bus->bypassed) {
			for (int j = 0; j < audio_bus->effects.size(); j++) {
				if (!audio_bus->effects[j]->enabled) {
					continue;
				}

#ifdef DEBUG_ENABLED
				uint64_t ticks = OS::get_singleton()->get_ticks_usec();
#endif

				for (int k = 0; k < audio_bus->channel_pairs.size(); k++) {
					if (!(channels_ptrw[k].active || channels_ptrw[k].effect_instances[j]->process_silence())) {
						continue;
					}
					channels_ptrw[k].effect_instances.write[j]->process(channels_ptrw[k].buffer.ptr(), temp_buffer_ptrw[k].ptrw(), buffer_size);
				}

				// Swap buffers, so internal buffer always has the right data.
				for (int k = 0; k < audio_bus->channel_pairs.size(); k++) {
					if (!(audio_buses[i]->channel_pairs[k].active || channels_ptrw[k].effect_instances[j]->process_silence())) {
						continue;
					}
					SWAP(channels_ptrw[k].buffer, temp_buffer_ptrw[k]);
				}

#ifdef DEBUG_ENABLED
				audio_bus->effects.write[j]->prof_time += OS::get_singleton()->get_ticks_usec() - ticks;
#endif
			}
		}

		// Process send.

		RID send_rid = audio_bus->send;
		AuST::AudioBus *send = nullptr;
		for (int j = 0; j < audio_buses.size(); j++) {
			if (audio_buses[j]->rid == send_rid) {
				send = audio_buses[j];
				break;
			}
		}

		for (int k = 0; k < audio_bus->channel_pairs.size(); k++) {
			if (!audio_bus->channel_pairs[k].active) {
				channels_ptrw[k].peak_volume_linear = AudioFrame(AuSC::AUDIO_MIN_PEAK_DB, AuSC::AUDIO_MIN_PEAK_DB);
				continue;
			}

			AudioFrame *buf = channels_ptrw[k].buffer.ptrw();

			AudioFrame peak = AudioFrame(0, 0);

			float volume_linear = Math::db_to_linear(audio_bus->volume_db);

			if (solo_mode) {
				if (!audio_bus->soloed) {
					volume_linear = 0.0;
				}
			} else {
				if (audio_bus->muted) {
					volume_linear = 0.0;
				}
			}

			// Apply volume and compute peak.
			for (uint32_t j = 0; j < buffer_size; j++) {
				buf[j] *= volume_linear;

				float l = Math::abs(buf[j].left);
				if (l > peak.left) {
					peak.left = l;
				}
				float r = Math::abs(buf[j].right);
				if (r > peak.right) {
					peak.right = r;
				}
			}

			channels_ptrw[k].peak_volume_linear = AudioFrame(Math::linear_to_db(peak.left + AuSC::AUDIO_PEAK_OFFSET), Math::linear_to_db(peak.right + AuSC::AUDIO_PEAK_OFFSET));

			if (channels_ptrw[k].active) {
				if (MAX(peak.right, peak.left) > Math::db_to_linear(channel_disable_threshold_db)) {
					channels_ptrw[k].last_mix_with_audio = mix_frames;
				} else if (mix_frames - audio_bus->channel_pairs[k].last_mix_with_audio > channel_disable_frames) {
					channels_ptrw[k].active = false;
					continue;
				}
			}

			if (send) {
				// If not master bus, send.
				AudioFrame *target_buf = get_channel_pair_mix_buffer(send_rid, k);

				for (uint32_t j = 0; j < buffer_size; j++) {
					target_buf[j] += buf[j];
				}
			}
		}
	}

	mix_frames += buffer_size;
	to_mix = buffer_size;
}

void AudioServer::_mix_step_for_channel_pair(AudioFrame *p_out_buf, AudioFrame *p_source_buf, AudioFrame p_vol_start, AudioFrame p_vol_final) {
	for (unsigned int frame_idx = 0; frame_idx < buffer_size; frame_idx++) {
		// TODO: Make lerp speed buffer-size-invariant if buffer_size ever becomes a project setting to avoid very small buffer sizes causing pops due to too-fast lerps.
		float lerp_param = (float)frame_idx / buffer_size;
		p_out_buf[frame_idx] += (p_vol_final * lerp_param + (1 - lerp_param) * p_vol_start) * p_source_buf[frame_idx];
	}
}

AudioFrame *AudioServer::get_channel_pair_mix_buffer(RID p_bus, int p_buffer) {
	// TODO: Replace with error macro.
	if (!p_bus.is_valid()) {
		return nullptr;
	}

	AudioFrame *data = audio_bus_map[p_bus]->channel_pairs.write[p_buffer].buffer.ptrw();

	if (!audio_bus_map[p_bus]->channel_pairs[p_buffer].active) {
		audio_bus_map[p_bus]->channel_pairs.write[p_buffer].active = true;
		audio_bus_map[p_bus]->channel_pairs.write[p_buffer].last_mix_with_audio = mix_frames;
		for (uint32_t i = 0; i < buffer_size; i++) {
			data[i] = AudioFrame(0, 0);
		}
	}

	return data;
}

#ifdef DEBUG_ENABLED
void AudioServer::set_debug_mute(bool p_mute) {
	debug_mute = p_mute;
}

bool AudioServer::get_debug_mute() const {
	return debug_mute;
}
#endif // DEBUG_ENABLED

// New RID methods:
// TODO: Check relevant setters for all these properties to maintain expected behavior.
// TODO: Add player parameters (e.g. MP3's "looping").
// TODO: Check if ERR_FAIL_COND_V in Stage setters have the right conditional.
// TODO: Use ranges (for error checking) defined inside functions, not ranges defined in the inspector.
// TODO: Consider if some setters should execute, even if playback is not active.
// TODO: Check if the operation succeeded before returning Error::OK

// Emitter

RID AudioServer::create_emitter(AuSE::EmitterType p_type) {
	ERR_FAIL_COND_V(p_type < 0 || p_type > 2, RID::from_uint64(0)); // TODO: Use macro with MSG, and using WARN if error can be fixed internally.

	RID rid = emitter_owner.make_rid();
	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(rid);
	ERR_FAIL_NULL_V(emitter, RID::from_uint64(0));

	emitter->rid = rid;
	emitter->active = false;
	emitter->type = p_type;

	switch (p_type) {
		case AuSE::EmitterType::NON_POSITIONAL:
			AuST::AudioStage stage;
			stage.type = AuSE::StageType::STAGE_DISABLED;
			emitter->stage = stage;
			break;
		case AuSE::EmitterType::POSITIONAL_2D:
			AuST::AudioStage stage;
			stage.type = AuSE::StageType::STAGE_2D;
			stage.area_mask = 0;
			stage.attenuation_exponent = 1.0;
			stage.max_distance = 2000.0;
			stage.panning_strength = 1.0;
			emitter->stage = stage;
			break;
		case AuSE::EmitterType::POSITIONAL_3D:
			AuST::AudioStage stage;
			stage.type = AuSE::StageType::STAGE_3D;
			stage.area_mask = 0;
			stage.attenuation_filter_cutoff = 5000.0;
			stage.attenuation_filter_gain = -24.0;
			stage.attenuation_model = AuSE::AttenuationModel::ATTENUATION_INVERSE_DISTANCE;
			stage.doppler_tracking = AuSE::DopplerTracking::DOPPLER_TRACKING_DISABLED;
			stage.emission_angle = 45.0;
			stage.emission_angle_filter_attenuation = -12.0;
			stage.max_volume = 3.0;
			stage.max_distance = 0.0;
			stage.panning_strength = 1.0;
			stage.unit_size = 10.0;
			emitter->stage = stage;
			break;
	}

	if (p_type == AuSE::EmitterType::NON_POSITIONAL && emitter->stage.type == AuSE::StageType::STAGE_DISABLED) {
		switch (get_speaker_mode()) {
			case AuSE::SpeakerMode::SPEAKER_MODE_STEREO:
				emitter->mix_target = AuSE::MixTarget::MIX_TARGET_STEREO;
				break;
			case AuSE::SpeakerMode::SPEAKER_SURROUND_31:
			case AuSE::SpeakerMode::SPEAKER_SURROUND_51:
			case AuSE::SpeakerMode::SPEAKER_SURROUND_71:
				emitter->mix_target = AuSE::MIX_TARGET_SURROUND;
				break;
		}
	}

	emitter->volume_vector = _get_volume_vector(rid);

	emitter->max_voices = 1;
	emitter->polyphony_mode = AuSE::PolyphonyMode::PREVENT_NEW;
	emitter->playback_type = get_default_playback_type();

	audio_emitter_list.push_back(emitter);

	return rid;
}

Error AudioServer::remove_emitter(RID p_emitter) {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), Error::ERR_INVALID_PARAMETER);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, Error::FAILED);
	audio_emitter_list.erase(emitter);
	emitter_owner.free(emitter->rid);

	return Error::OK;
}

bool AudioServer::emitter_exists(RID p_emitter) const {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), false);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, false);

	return true;
}

bool AudioServer::is_emitter_active(RID p_emitter) const {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), false);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, false);

	return emitter->active;
}

AuSE::EmitterType AudioServer::get_emitter_type(RID p_emitter) const {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), AuSE::EmitterType::NON_POSITIONAL);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, AuSE::EmitterType::NON_POSITIONAL);

	return emitter->type;
}

RID AudioServer::add_stream(RID p_emitter, const Ref<AudioStream> &p_stream) {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), RID::from_uint64(0));
	ERR_FAIL_NULL_V(p_stream, RID::from_uint64(0));
	// TODO: Check type of stream.

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, RID::from_uint64(0));

	RID rid = stream_owner.make_rid();
	AuST::AudioEmitterStream *stream = stream_owner.get_or_null(rid);
	ERR_FAIL_NULL_V(stream, RID::from_uint64(0));

	AuST::AudioBus *audio_bus = audio_bus_owner.get_or_null(_get_master_audio_bus());
	ERR_FAIL_NULL_V(audio_bus, RID::from_uint64(0));

	stream->rid = rid;
	stream->emitter_rid = emitter->rid;
	stream->stream = p_stream;
	stream->active = false;
	stream->volume_db = 0.0;
	stream->pitch_scale = 1.0;
	stream->bus = audio_bus;

	emitter->streams.push_back(stream);

	emitter_stream_list.push_back(stream);

	return rid;
}

Error AudioServer::remove_stream(RID p_stream) { // Only the stream RID is needed.
	ERR_FAIL_COND_V(p_stream.is_null() || !p_stream.is_valid(), Error::ERR_INVALID_PARAMETER);

	// TODO: Look for the emitters that have the stream and remove from their `streams` property.

	AuST::AudioEmitterStream *stream = stream_owner.get_or_null(p_stream);
	ERR_FAIL_NULL_V(stream, Error::FAILED);
	emitter_stream_list.erase(stream);
	// TODO: Remove from the emitter list of streams.
	stream_owner.free(stream->rid);

	return Error::OK;
}

bool AudioServer::stream_exists(RID p_stream) const {
	ERR_FAIL_COND_V(p_stream.is_null() || !p_stream.is_valid(), false);

	AuST::AudioEmitterStream *stream = stream_owner.get_or_null(p_stream);
	ERR_FAIL_NULL_V(stream, false);

	return true;
}

Error AudioServer::clear_streams(RID p_emitter) {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), Error::ERR_INVALID_PARAMETER);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, Error::FAILED);

	for (AuST::AudioEmitterStream *stream : emitter->streams) {
		RID stream_rid = stream->rid;
		stream_owner.free(stream_rid);
	}
	emitter_stream_list.clear();

	return Error::OK;
}

Error AudioServer::emitter_update_position(RID p_emitter, Variant p_position) {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), Error::ERR_INVALID_PARAMETER);
	// Check if `p_position` is either a Vector2 or Vector3.

	// TODO: Add to this, after figuring out how to replace `_update_panning()` for audio players.

	return Error::OK;
}

// Emitter: Stage

Error AudioServer::set_area_mask(RID p_emitter, int p_area_mask) {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), Error::ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_area_mask < 1 || p_area_mask > 32, Error::ERR_INVALID_PARAMETER);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, Error::FAILED);

	ERR_FAIL_COND_V(emitter->type == AuSE::EmitterType::NON_POSITIONAL || emitter->stage.type == AuSE::StageType::STAGE_DISABLED, Error::ERR_INVALID_PARAMETER);

	emitter->stage.area_mask = p_area_mask;

	return Error::OK;
}

uint8_t AudioServer::get_area_mask(RID p_emitter) const {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), 0);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, 0);
	ERR_FAIL_COND_V(emitter->type == AuSE::EmitterType::NON_POSITIONAL || emitter->stage.type == AuSE::StageType::STAGE_DISABLED, 0);

	return emitter->stage.area_mask;
}

Error AudioServer::set_attenuation_exponent(RID p_emitter, float p_exponent) {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), Error::ERR_INVALID_PARAMETER);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, Error::FAILED);

	ERR_FAIL_COND_V(emitter->type != AuSE::EmitterType::POSITIONAL_2D || emitter->stage.type != AuSE::StageType::STAGE_2D, Error::ERR_INVALID_PARAMETER);

	emitter->stage.attenuation_exponent = p_exponent;

	return Error::OK;
}

float AudioServer::get_attenuation_exponent(RID p_emitter) const {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), -1.0);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, -1.0);
	ERR_FAIL_COND_V(emitter->type == AuSE::EmitterType::NON_POSITIONAL || emitter->stage.type == AuSE::StageType::STAGE_DISABLED, -1.0);

	return emitter->stage.attenuation_exponent;
}

Error AudioServer::set_attenuation_filter_cutoff(RID p_emitter, float p_cutoff) {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), Error::ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_cutoff < 1.0 || p_cutoff > 20500.0, Error::ERR_INVALID_PARAMETER);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, Error::FAILED);

	ERR_FAIL_COND_V(emitter->type != AuSE::EmitterType::POSITIONAL_3D || emitter->stage.type != AuSE::StageType::STAGE_3D, Error::ERR_INVALID_PARAMETER);

	emitter->stage.attenuation_filter_cutoff = p_cutoff;

	return Error::OK;
}

float AudioServer::get_attenuation_filter_cutoff(RID p_emitter) const {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), 0.0);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, 0.0);
	ERR_FAIL_COND_V(emitter->type == AuSE::EmitterType::NON_POSITIONAL || emitter->stage.type == AuSE::StageType::STAGE_DISABLED, 0.0);

	return emitter->stage.attenuation_filter_cutoff;
}

Error AudioServer::set_attenuation_filter_gain(RID p_emitter, float p_gain) {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), Error::ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_gain < -80.0 || p_gain > 0.0, Error::ERR_INVALID_PARAMETER);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, Error::FAILED);

	ERR_FAIL_COND_V(emitter->type != AuSE::EmitterType::POSITIONAL_3D || emitter->stage.type != AuSE::StageType::STAGE_3D, Error::ERR_INVALID_PARAMETER);

	emitter->stage.attenuation_filter_gain = p_gain;

	return Error::OK;
}

float AudioServer::get_attenuation_filter_gain(RID p_emitter) const {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), 1.0);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, 1.0);
	ERR_FAIL_COND_V(emitter->type == AuSE::EmitterType::NON_POSITIONAL || emitter->stage.type == AuSE::StageType::STAGE_DISABLED, 1.0);

	return emitter->stage.attenuation_filter_gain;
}

Error AudioServer::set_attenuation_model(RID p_emitter, AuSE::AttenuationModel p_model) {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), Error::ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_model < 0 || p_model > 3, Error::ERR_INVALID_PARAMETER);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, Error::FAILED);

	ERR_FAIL_COND_V(emitter->type != AuSE::EmitterType::POSITIONAL_3D || emitter->stage.type != AuSE::StageType::STAGE_3D, Error::ERR_INVALID_PARAMETER);

	emitter->stage.attenuation_model = p_model;

	return Error::OK;
}

AuSE::AttenuationModel AudioServer::get_attenuation_model(RID p_emitter) const {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), AuSE::AttenuationModel::ATTENUATION_INVERSE_DISTANCE);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, AuSE::AttenuationModel::ATTENUATION_INVERSE_DISTANCE);
	ERR_FAIL_COND_V(emitter->type == AuSE::EmitterType::NON_POSITIONAL || emitter->stage.type == AuSE::StageType::STAGE_DISABLED, AuSE::AttenuationModel::ATTENUATION_INVERSE_DISTANCE);

	return emitter->stage.attenuation_model;
}

Error AudioServer::set_doppler_tracking(RID p_emitter, AuSE::DopplerTracking p_doppler_tracking) {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), Error::ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_doppler_tracking < 0 || p_doppler_tracking > 2, Error::ERR_INVALID_PARAMETER);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, Error::FAILED);

	ERR_FAIL_COND_V(emitter->type != AuSE::EmitterType::POSITIONAL_3D || emitter->stage.type != AuSE::StageType::STAGE_3D, Error::ERR_INVALID_PARAMETER);

	emitter->stage.doppler_tracking = p_doppler_tracking;

	return Error::OK;
}

AuSE::DopplerTracking AudioServer::get_doppler_tracking(RID p_emitter) const {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), AuSE::DopplerTracking::DOPPLER_TRACKING_DISABLED);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, AuSE::DopplerTracking::DOPPLER_TRACKING_DISABLED);
	ERR_FAIL_COND_V(emitter->type == AuSE::EmitterType::NON_POSITIONAL || emitter->stage.type == AuSE::StageType::STAGE_DISABLED, AuSE::DopplerTracking::DOPPLER_TRACKING_DISABLED);

	return emitter->stage.doppler_tracking;
}

Error AudioServer::set_emission_angle(RID p_emitter, float p_angle) {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), Error::ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_angle < 0.1 || p_angle > 90.0, Error::ERR_INVALID_PARAMETER);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, Error::FAILED);

	ERR_FAIL_COND_V(emitter->type != AuSE::EmitterType::POSITIONAL_3D || emitter->stage.type != AuSE::StageType::STAGE_3D, Error::ERR_INVALID_PARAMETER);

	emitter->stage.emission_angle = p_angle;

	return Error::OK;
}

float AudioServer::get_emission_angle(RID p_emitter) const {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), 0.0);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, 0.0);
	ERR_FAIL_COND_V(emitter->type == AuSE::EmitterType::NON_POSITIONAL || emitter->stage.type == AuSE::StageType::STAGE_DISABLED, 0.0);

	return emitter->stage.emission_angle;
}

Error AudioServer::set_emission_angle_filter_attenuation(RID p_emitter, float p_emission_angle_filter_attenuation) {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), Error::ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_emission_angle_filter_attenuation < -80.0 || p_emission_angle_filter_attenuation > 0.0, Error::ERR_INVALID_PARAMETER);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, Error::FAILED);

	ERR_FAIL_COND_V(emitter->type != AuSE::EmitterType::POSITIONAL_3D || emitter->stage.type != AuSE::StageType::STAGE_3D, Error::ERR_INVALID_PARAMETER);

	emitter->stage.emission_angle_filter_attenuation = p_emission_angle_filter_attenuation;

	return Error::OK;
}

float AudioServer::get_emission_angle_filter_attenuation(RID p_emitter) const {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), 1.0);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, 1.0);
	ERR_FAIL_COND_V(emitter->type == AuSE::EmitterType::NON_POSITIONAL || emitter->stage.type == AuSE::StageType::STAGE_DISABLED, 1.0);

	return emitter->stage.emission_angle_filter_attenuation;
}

Error AudioServer::set_max_volume(RID p_emitter, float p_volume) {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), Error::ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_volume < -24.0 || p_volume > 6.0, Error::ERR_INVALID_PARAMETER);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, Error::FAILED);

	ERR_FAIL_COND_V(emitter->type != AuSE::EmitterType::POSITIONAL_3D || emitter->stage.type != AuSE::StageType::STAGE_3D, Error::ERR_INVALID_PARAMETER);

	emitter->stage.max_volume = p_volume;

	return Error::OK;
}

float AudioServer::get_max_volume(RID p_emitter) const {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), 3.0);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, 3.0);
	ERR_FAIL_COND_V(emitter->type == AuSE::EmitterType::NON_POSITIONAL || emitter->stage.type == AuSE::StageType::STAGE_DISABLED, 3.0);

	return emitter->stage.max_volume;
}

Error AudioServer::set_max_distance(RID p_emitter, float p_distance) {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), Error::ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_distance < 0.0 || p_distance > 4096.0, Error::ERR_INVALID_PARAMETER);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, Error::FAILED);

	ERR_FAIL_COND_V(emitter->type == AuSE::EmitterType::NON_POSITIONAL || emitter->stage.type == AuSE::StageType::STAGE_DISABLED, Error::ERR_INVALID_PARAMETER);

	emitter->stage.max_distance = p_distance;

	return Error::OK;
}

float AudioServer::get_max_distance(RID p_emitter) const {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), -1.0);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, -1.0);
	ERR_FAIL_COND_V(emitter->type == AuSE::EmitterType::NON_POSITIONAL || emitter->stage.type == AuSE::StageType::STAGE_DISABLED, -1.0);

	return emitter->stage.max_distance;
}

Error AudioServer::set_panning_strength(RID p_emitter, float p_strength) {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), Error::ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_strength < 0.0 || p_strength > 3.0, Error::ERR_INVALID_PARAMETER);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, Error::FAILED);

	ERR_FAIL_COND_V(emitter->type == AuSE::EmitterType::NON_POSITIONAL || emitter->stage.type == AuSE::StageType::STAGE_DISABLED, Error::ERR_INVALID_PARAMETER);

	emitter->stage.panning_strength = p_strength;

	return Error::OK;
}

float AudioServer::get_panning_strength(RID p_emitter) const {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), -1.0);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, -1.0);
	ERR_FAIL_COND_V(emitter->type == AuSE::EmitterType::NON_POSITIONAL || emitter->stage.type == AuSE::StageType::STAGE_DISABLED, -1.0);

	return emitter->stage.panning_strength;
}

Error AudioServer::set_unit_size(RID p_emitter, float p_size) {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), Error::ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_size < 0.1 || p_size > 100.0, Error::ERR_INVALID_PARAMETER);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, Error::FAILED);

	ERR_FAIL_COND_V(emitter->type != AuSE::EmitterType::POSITIONAL_3D || emitter->stage.type != AuSE::StageType::STAGE_3D, Error::ERR_INVALID_PARAMETER);

	emitter->stage.unit_size = p_size;

	return Error::OK;
}

float AudioServer::get_unit_size(RID p_emitter) const {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), 0.0);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, 0.0);
	ERR_FAIL_COND_V(emitter->type == AuSE::EmitterType::NON_POSITIONAL || emitter->stage.type == AuSE::StageType::STAGE_DISABLED, 0.0);

	return emitter->stage.unit_size;
}

// Emitter (contd.)

Error AudioServer::set_max_voices(RID p_emitter, uint8_t p_voices) {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), Error::ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_voices < 1 || p_voices > 128, Error::ERR_INVALID_PARAMETER);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, Error::FAILED);

	emitter->max_voices = p_voices;

	return Error::OK;
}

uint8_t AudioServer::get_max_voices(RID p_emitter) const {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), 0);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, 0);

	return emitter->max_voices;
}

Error AudioServer::set_polyphony_mode(RID p_emitter, AuSE::PolyphonyMode p_mode) {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), Error::ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_mode < 0 || p_mode > 1, Error::ERR_INVALID_PARAMETER);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, Error::FAILED);

	emitter->polyphony_mode = p_mode;

	return Error::OK;
}

AuSE::PolyphonyMode AudioServer::get_polyphony_mode(RID p_emitter) const {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), AuSE::PolyphonyMode::PREVENT_NEW);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, AuSE::PolyphonyMode::PREVENT_NEW);

	return emitter->polyphony_mode;
}

Error AudioServer::set_emitter_playback_type(RID p_emitter, AuSE::PlaybackType p_type) {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), Error::ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_type < 0 || p_type > AuSE::PlaybackType::PLAYBACK_TYPE_MAX, Error::ERR_INVALID_PARAMETER);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, Error::FAILED);
	ERR_FAIL_COND_V(!emitter->streams.size(), Error::FAILED);

	emitter->playback_type = p_type;

	return Error::OK;
}

AuSE::PlaybackType AudioServer::get_emitter_playback_type(RID p_emitter) const {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), AuSE::PlaybackType::PLAYBACK_TYPE_STREAM);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, AuSE::PlaybackType::PLAYBACK_TYPE_STREAM);

	return emitter->playback_type;
}

Error AudioServer::set_mix_target(RID p_emitter, AuSE::MixTarget p_target) {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), Error::ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_target < 0 || p_target > 2, Error::ERR_INVALID_PARAMETER);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, Error::FAILED);
	ERR_FAIL_COND_V(emitter->type != AuSE::EmitterType::NON_POSITIONAL || emitter->stage.type != AuSE::StageType::STAGE_DISABLED, Error::ERR_INVALID_PARAMETER);

	emitter->mix_target = p_target;

	return Error::OK;
}

AuSE::MixTarget AudioServer::get_mix_target(RID p_emitter) const {
	ERR_FAIL_COND_V(p_emitter.is_null() || !p_emitter.is_valid(), AuSE::MixTarget::MIX_TARGET_STEREO);

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, AuSE::MixTarget::MIX_TARGET_STEREO);
	ERR_FAIL_COND_V(emitter->type != AuSE::EmitterType::NON_POSITIONAL || emitter->stage.type != AuSE::StageType::STAGE_DISABLED, AuSE::MixTarget::MIX_TARGET_STEREO);

	return emitter->mix_target;
}

// Listener

RID AudioServer::create_listener(AuSE::ListenerType p_type, bool p_active) {
	ERR_FAIL_COND_V(p_type < 0 || p_type > 2, RID::from_uint64(0));

	RID rid = listener_owner.make_rid();
	AuST::AudioListener *listener = listener_owner.get_or_null(rid);
	ERR_FAIL_NULL_V(listener, RID::from_uint64(0));

	listener->rid = rid;
	listener->type = p_type;

	if (p_active && !audio_listener_list.is_empty()) {
		for (AuST::AudioListener *_listener : audio_listener_list) {
			if (_listener->active) {
				_listener->active = false;
			}
		}
	}
	listener->active = p_active;

	if (p_type == AuSE::ListenerType::LISTENER_3D) {
		listener->doppler_tracking = AuSE::DopplerTracking::DOPPLER_TRACKING_DISABLED;
		listener->transform = Transform3D();
	}

	return rid;
}

Error AudioServer::remove_listener(RID p_listener) {
	ERR_FAIL_COND_V(p_listener.is_null() || !p_listener.is_valid(), Error::ERR_INVALID_PARAMETER);

	AuST::AudioListener *listener = listener_owner.get_or_null(p_listener);
	ERR_FAIL_NULL_V(listener, Error::FAILED);
	audio_listener_list.erase(listener);
	listener_owner.free(listener->rid);

	return Error::OK;
}

bool AudioServer::listener_exists(RID p_listener) const {
	ERR_FAIL_COND_V(p_listener.is_null() || !p_listener.is_valid(), false);

	AuST::AudioListener *listener = listener_owner.get_or_null(p_listener);
	ERR_FAIL_NULL_V(listener, false);

	return true;
}

AuSE::ListenerType AudioServer::get_listener_type(RID p_listener) const {
	ERR_FAIL_COND_V(p_listener.is_null() || !p_listener.is_valid(), AuSE::ListenerType::LISTENER_2D);

	AuST::AudioListener *listener = listener_owner.get_or_null(p_listener);
	ERR_FAIL_NULL_V(listener, AuSE::ListenerType::LISTENER_2D);

	return listener->type;
}

Error AudioServer::set_active(RID p_listener, bool p_active) {
	ERR_FAIL_COND_V(p_listener.is_null() || !p_listener.is_valid(), Error::ERR_INVALID_PARAMETER);

	AuST::AudioListener *listener = listener_owner.get_or_null(p_listener);
	ERR_FAIL_NULL_V(listener, Error::FAILED);

	if (p_active && !audio_listener_list.is_empty()) {
		for (AuST::AudioListener *_listener : audio_listener_list) {
			if (_listener->active) {
				_listener->active = false;
			}
		}
	}
	listener->active = p_active;

	return Error::OK;
}

bool AudioServer::is_active(RID p_listener) const {
	ERR_FAIL_COND_V(p_listener.is_null() || !p_listener.is_valid(), false);

	AuST::AudioListener *listener = listener_owner.get_or_null(p_listener);
	ERR_FAIL_NULL_V(listener, false);

	return listener->active;
}

Error AudioServer::listener_update_position(RID p_listener, Variant p_position) {
	ERR_FAIL_COND_V(p_listener.is_null() || !p_listener.is_valid(), Error::ERR_INVALID_PARAMETER);
	// Check if `p_position` is either a Vector2 or Vector3.

	// TODO: Add to this, after figuring out how to replace `_update_panning()` for audio players.

	return Error::OK;
}

Error AudioServer::set_listener_doppler_tracking(RID p_listener, AuSE::DopplerTracking p_doppler_tracking) {
	ERR_FAIL_COND_V(p_listener.is_null() || !p_listener.is_valid(), Error::ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_doppler_tracking < 0 || p_doppler_tracking > 2, Error::ERR_INVALID_PARAMETER);

	AuST::AudioListener *listener = listener_owner.get_or_null(p_listener);
	ERR_FAIL_NULL_V(listener, Error::FAILED);

	ERR_FAIL_COND_V(listener->type != AuSE::ListenerType::LISTENER_3D, Error::ERR_INVALID_PARAMETER);

	listener->doppler_tracking = p_doppler_tracking;

	return Error::OK;
}

AuSE::DopplerTracking AudioServer::get_listener_doppler_tracking(RID p_listener) const {
	ERR_FAIL_COND_V(p_listener.is_null() || !p_listener.is_valid(), AuSE::DopplerTracking::DOPPLER_TRACKING_DISABLED);

	AuST::AudioListener *listener = listener_owner.get_or_null(p_listener);
	ERR_FAIL_NULL_V(listener, AuSE::DopplerTracking::DOPPLER_TRACKING_DISABLED);
	ERR_FAIL_COND_V(listener->type == AuSE::ListenerType::LISTENER_2D, AuSE::DopplerTracking::DOPPLER_TRACKING_DISABLED);

	return listener->doppler_tracking;
}

Error AudioServer::listener_update_transform(RID p_listener, Transform3D p_transform) {
	ERR_FAIL_COND_V(p_listener.is_null() || !p_listener.is_valid(), Error::ERR_INVALID_PARAMETER);
	// Check if `p_transform` is Transform3D.

	// TODO: Add to this.

	return Error::OK;
}

Transform3D AudioServer::get_listener_transform(RID p_listener) const {
	ERR_FAIL_COND_V(p_listener.is_null() || !p_listener.is_valid(), Transform3D());

	AuST::AudioListener *listener = listener_owner.get_or_null(p_listener);
	ERR_FAIL_NULL_V(listener, Transform3D());
	ERR_FAIL_COND_V(listener->type == AuSE::ListenerType::LISTENER_2D, Transform3D());

	return listener->transform;
}

RID AudioServer::get_active_listener() const {
	if (audio_listener_list.is_empty()) {
		return RID::from_uint64(0);
	}

	for (AuST::AudioListener *listener : audio_listener_list) {
		if (listener->active) {
			return listener->rid;
		}
	}

	return RID::from_uint64(0);
}

// Stream

// TODO: Confirm behavior works for all emitter types (see how every AudioStreamPlayer node executes a play call).
Error AudioServer::play_stream(RID p_stream, float p_time) {
	ERR_FAIL_COND_V(p_stream.is_null() || !p_stream.is_valid(), Error::ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(Math::is_nan(p_time), Error::ERR_INVALID_PARAMETER);

	AuST::AudioEmitterStream *stream = stream_owner.get_or_null(p_stream);
	ERR_FAIL_NULL_V(stream, Error::FAILED);
	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(stream->emitter_rid);
	ERR_FAIL_NULL_V(emitter, Error::FAILED);

	if (stream->active_playback_voices.size() >= emitter->max_voices && emitter->polyphony_mode == AuSE::PolyphonyMode::PREVENT_NEW) {
		return Error::OK;
	}
	if (!stream->stream_playback.is_null() && stream->stream_playback->is_playing() && stream->stream->is_monophonic()) {
		stream->stream_playback->stop();
	}
	if (stream->active_playback_voices.size() >= emitter->max_voices && emitter->polyphony_mode == AuSE::PolyphonyMode::STOP_OLDEST) {
		stream->active_playback_voices[0]->stop();
		stream->active_playback_voices.remove_at(0);
	}
	stream->stream_playback = stream->stream->instantiate_playback();
	ERR_FAIL_COND_V(stream->stream_playback.is_null(), Error::FAILED);

	Ref<AudioSamplePlayback> sample_playback;
	if (emitter->playback_type == AuSE::PlaybackType::PLAYBACK_TYPE_SAMPLE) {
		if (stream->stream->can_be_sampled()) {
			stream->stream_playback->set_is_sample(true);
			if (stream->stream_playback->get_is_sample() && stream->stream_playback->get_sample_playback().is_null()) {
				if (!is_stream_registered_as_sample(stream->stream)) {
					register_stream_as_sample(stream->stream);
				}
				sample_playback.instantiate();
				sample_playback->stream = stream->stream;
				stream->stream_playback->set_sample_playback(sample_playback);
				ERR_FAIL_COND_V(stream->stream_playback.is_null(), Error::FAILED);
			}
		}
	}

	if (emitter->playback_type == AuSE::PlaybackType::PLAYBACK_TYPE_STREAM) {
		stream->stream_playback->start(p_time);

		for (AudioFrame &frame : stream->lookahead) {
			frame = AudioFrame(0, 0);
		}

		stream->state = AuSE::PlaybackState::PLAYING;

		if (!stream->active) {
			stream->active = true;
		}
		if (!emitter->active) {
			emitter->active = true;
		}
		stream->active_playback_voices.push_back(stream->stream_playback);
	} else if (emitter->playback_type == AuSE::PlaybackType::PLAYBACK_TYPE_SAMPLE && !stream->stream_playback->get_sample_playback().is_valid()) {
		sample_playback = stream->stream_playback->get_sample_playback();
		sample_playback->offset = p_time;
		sample_playback->volume_vector = emitter->volume_vector;
		sample_playback->pitch_scale = stream->pitch_scale;
		sample_playback->bus = stream->bus->name; // TODO: Make sample playback work with RID too.
		start_sample_playback(sample_playback);
	} else {
		return Error::FAILED;
	}

	return Error::OK;
}

Error AudioServer::seek_stream(RID p_stream, float p_time) {
	ERR_FAIL_COND_V(p_stream.is_null() || !p_stream.is_valid(), Error::ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(Math::is_nan(p_time), Error::ERR_INVALID_PARAMETER);

	AuST::AudioEmitterStream *stream = stream_owner.get_or_null(p_stream);
	ERR_FAIL_NULL_V(stream, Error::FAILED);
	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(stream->emitter_rid);
	ERR_FAIL_NULL_V(emitter, Error::FAILED);

	if (emitter->playback_type == AuSE::PlaybackType::PLAYBACK_TYPE_STREAM && stream->active) {
		stream->stream_playback->seek(p_time); // TODO: Make sure this is all.
		return Error::OK;
	} else if (emitter->playback_type == AuSE::PlaybackType::PLAYBACK_TYPE_SAMPLE && stream->stream_playback->get_sample_playback().is_valid()) {
		Ref<AudioSamplePlayback> sample_playback = stream->stream_playback->get_sample_playback();
		// TODO: Finish this.
	}

	return Error::FAILED;
}

float AudioServer::get_stream_playback_position(RID p_stream) const {
	ERR_FAIL_COND_V(p_stream.is_null() || !p_stream.is_valid(), -1.0);

	AuST::AudioEmitterStream *stream = stream_owner.get_or_null(p_stream);
	ERR_FAIL_NULL_V(stream, -1.0);
	ERR_FAIL_COND_V(!stream->active, -1.0);
	ERR_FAIL_COND_V(stream->active_playback_voices.is_empty(), -1.0);

	return stream->active_playback_voices[stream->active_playback_voices.size() - 1]->get_playback_position();
}

Error AudioServer::pause_stream(RID p_stream, bool p_pause) {
	ERR_FAIL_COND_V(p_stream.is_null() || !p_stream.is_valid(), Error::ERR_INVALID_PARAMETER);

	AuST::AudioEmitterStream *stream = stream_owner.get_or_null(p_stream);
	ERR_FAIL_NULL_V(stream, Error::FAILED);
	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(stream->emitter_rid);
	ERR_FAIL_NULL_V(emitter, Error::FAILED);

	if (emitter->playback_type == AuSE::PlaybackType::PLAYBACK_TYPE_STREAM && stream->active) {
		if (p_pause && !(stream->state == AuSE::PlaybackState::FADE_OUT_TO_PAUSE || stream->state == AuSE::PlaybackState::PAUSED)) {
			stream->state = AuSE::PlaybackState::PAUSED;
		} else if (!p_pause && (stream->state == AuSE::PlaybackState::FADE_OUT_TO_PAUSE || stream->state == AuSE::PlaybackState::PAUSED)) {
			stream->state == AuSE::PlaybackState::PLAYING;
		}
		return Error::OK;
	} else if (emitter->playback_type == AuSE::PlaybackType::PLAYBACK_TYPE_SAMPLE && stream->stream_playback->get_sample_playback().is_valid()) {
		Ref<AudioSamplePlayback> sample_playback = stream->stream_playback->get_sample_playback();
		set_sample_playback_pause(sample_playback, p_pause);
		return Error::OK;
	}

	return Error::FAILED;
}

bool AudioServer::is_stream_paused(RID p_stream) const {
	ERR_FAIL_COND_V(p_stream.is_null() || !p_stream.is_valid(), false);

	AuST::AudioEmitterStream *stream = stream_owner.get_or_null(p_stream);
	ERR_FAIL_NULL_V(stream, false);
	ERR_FAIL_COND_V(!stream->active, false);

	return stream->state == AuSE::PlaybackState::FADE_OUT_TO_PAUSE || stream->state == AuSE::PlaybackState::PAUSED;
}

// TODO: Confirm behavior works for all emitter types (see how every AudioStreamPlayer node executes a stop call).
Error AudioServer::stop_stream(RID p_stream) {
	ERR_FAIL_COND_V(p_stream.is_null() || !p_stream.is_valid(), Error::ERR_INVALID_PARAMETER);

	AuST::AudioEmitterStream *stream = stream_owner.get_or_null(p_stream);
	ERR_FAIL_NULL_V(stream, Error::FAILED);
	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(stream->emitter_rid);
	ERR_FAIL_NULL_V(emitter, Error::FAILED);

	if (emitter->playback_type == AuSE::PlaybackType::PLAYBACK_TYPE_STREAM && stream->active) {
		stream->stream_playback->stop();
		if (stream->state != AuSE::PlaybackState::AWAITING_DEACTIVATION) {
			stream->state = AuSE::PlaybackState::FADE_OUT_TO_DEACTIVATION;
		}
		stream->active_playback_voices.erase(stream->stream_playback);
		return Error::OK;
	} else if (emitter->playback_type == AuSE::PlaybackType::PLAYBACK_TYPE_SAMPLE && stream->stream_playback->get_sample_playback().is_valid()) {
		Ref<AudioSamplePlayback> sample_playback = stream->stream_playback->get_sample_playback();
		stop_sample_playback(sample_playback);
		return Error::OK;
	}

	return Error::FAILED;
}

bool AudioServer::is_stream_active(RID p_stream) const {
	ERR_FAIL_COND_V(p_stream.is_null() || !p_stream.is_valid(), false);

	AuST::AudioEmitterStream *stream = stream_owner.get_or_null(p_stream);
	ERR_FAIL_NULL_V(stream, false);

	return stream->active;
}

Error AudioServer::set_stream_volume(RID p_stream, float p_volume) {
	ERR_FAIL_COND_V(p_stream.is_null() || !p_stream.is_valid(), Error::ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(Math::is_nan(p_volume), Error::ERR_INVALID_PARAMETER); // TODO: Replace with range.

	AuST::AudioEmitterStream *stream = stream_owner.get_or_null(p_stream);
	ERR_FAIL_NULL_V(stream, Error::FAILED);

	stream->volume_db = p_volume;

	// TODO: Make sure this is all.

	return Error::OK;
}

float AudioServer::get_stream_volume(RID p_stream) const {
	ERR_FAIL_COND_V(p_stream.is_null() || !p_stream.is_valid(), 0.0);

	AuST::AudioEmitterStream *stream = stream_owner.get_or_null(p_stream);
	ERR_FAIL_NULL_V(stream, 0.0);

	return stream->volume_db;
}

Error AudioServer::set_stream_pitch_scale(RID p_stream, float p_scale) {
	ERR_FAIL_COND_V(p_stream.is_null() || !p_stream.is_valid(), Error::ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_scale <= 0.0, Error::ERR_INVALID_PARAMETER); // TODO: Consider adding upper bound.

	AuST::AudioEmitterStream *stream = stream_owner.get_or_null(p_stream);
	ERR_FAIL_NULL_V(stream, Error::FAILED);
	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(stream->emitter_rid);
	ERR_FAIL_NULL_V(emitter, Error::FAILED);

	if (emitter->playback_type == AuSE::PlaybackType::PLAYBACK_TYPE_STREAM) {
		stream->pitch_scale = p_scale;
		return Error::OK;
	} else if (emitter->playback_type == AuSE::PlaybackType::PLAYBACK_TYPE_SAMPLE && stream->stream_playback->get_sample_playback().is_valid()) {
		Ref<AudioSamplePlayback> sample_playback = stream->stream_playback->get_sample_playback();
		update_sample_playback_pitch_scale(sample_playback, p_scale);
		return Error::OK;
	}

	stream->pitch_scale = p_scale;

	return Error::FAILED;
}

float AudioServer::get_stream_pitch_scale(RID p_stream) const {
	ERR_FAIL_COND_V(p_stream.is_null() || !p_stream.is_valid(), 0.0);

	AuST::AudioEmitterStream *stream = stream_owner.get_or_null(p_stream);
	ERR_FAIL_NULL_V(stream, 0.0);

	return stream->pitch_scale;
}

Error AudioServer::set_stream_audio_bus(RID p_stream, RID p_bus) {
	ERR_FAIL_COND_V(p_stream.is_null() || !p_stream.is_valid(), Error::ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_bus.is_null() || !p_bus.is_valid(), Error::ERR_INVALID_PARAMETER);

	AuST::AudioEmitterStream *stream = stream_owner.get_or_null(p_stream);
	ERR_FAIL_NULL_V(stream, Error::FAILED);
	AuST::AudioBus *audio_bus = audio_bus_owner.get_or_null(p_bus);
	ERR_FAIL_NULL_V(audio_bus, Error::FAILED);

	stream->bus = audio_bus;

	return Error::OK;
}

RID AudioServer::get_stream_audio_bus(RID p_stream) const {
	ERR_FAIL_COND_V(p_stream.is_null() || !p_stream.is_valid(), RID::from_uint64(0));

	AuST::AudioEmitterStream *stream = stream_owner.get_or_null(p_stream);
	ERR_FAIL_NULL_V(stream, RID::from_uint64(0));

	return stream->bus->rid;
}

Ref<AudioStreamPlayback> AudioServer::get_stream_playback(RID p_stream) const {
	ERR_FAIL_COND_V(p_stream.is_null() || !p_stream.is_valid(), RID::from_uint64(0));

	AuST::AudioEmitterStream *stream = stream_owner.get_or_null(p_stream);
	ERR_FAIL_NULL_V(stream, RID::from_uint64(0));

	return stream->stream_playback;
}

bool AudioServer::has_stream_playback(RID p_stream) const {
	ERR_FAIL_COND_V(p_stream.is_null() || !p_stream.is_valid(), false);

	AuST::AudioEmitterStream *stream = stream_owner.get_or_null(p_stream);
	ERR_FAIL_NULL_V(stream, false);

	ERR_FAIL_COND_V(stream->stream_playback.is_null() || stream->stream_playback.is_valid() || stream->stream_playback->is_playing(), false);

	return true;
}

// Bus

RID AudioServer::add_audio_bus(int p_index) {
	ERR_FAIL_COND_V(p_index < -1 || p_index > audio_buses.size(), RID::from_uint64(0));

	RID rid = audio_bus_owner.make_rid();
	AuST::AudioBus *audio_bus = audio_bus_owner.get_or_null(rid);
	ERR_FAIL_NULL_V(audio_bus, RID::from_uint64(0));

	audio_bus->rid = rid;

	String name_attempt = "New Bus";
	int attempts = 1;
	while (true) {
		bool name_free = true;
		for (int i = 0; i < audio_buses.size(); i++) {
			if (audio_buses[i]->name == name_attempt) {
				name_free = false;
				break;
			}
		}

		if (!name_free) {
			attempts++;
			name_attempt = "New Bus " + itos(attempts);
		} else {
			break;
		}
	}
	audio_bus->name = name_attempt;

	audio_bus->master_bus = false;
	audio_bus->active = false;
	audio_bus->soloed = false;
	audio_bus->muted = false;
	audio_bus->bypassed = false;
	audio_bus->volume_db = 0.0;
	audio_bus->send = _get_master_audio_bus();

	audio_bus->channel_pairs.resize(channel_pair_count);
	for (AuST::BusChannelPair channel_pair : audio_bus->channel_pairs) {
		channel_pair.buffer.resize(buffer_size);
	}

	audio_bus_map[rid] = audio_bus;

	if (p_index < 0) {
		audio_buses.push_back(audio_bus);
	} else {
		if (p_index == 0) {
			audio_buses.insert(1, audio_bus);
		} else {
			audio_buses.insert(p_index, audio_bus);
		}
	}

	AudioDriver::get_singleton()->add_sample_bus(p_index);

	emit_signal(SNAME("bus_layout_changed"));

	return rid;
}

Error AudioServer::remove_audio_bus(RID p_bus) {
	ERR_FAIL_COND_V(p_bus.is_null() || !p_bus.is_valid(), Error::ERR_INVALID_PARAMETER);

	AuST::AudioBus *audio_bus = audio_bus_owner.get_or_null(p_bus);
	ERR_FAIL_NULL_V(audio_bus, Error::FAILED);
	ERR_FAIL_COND_V(audio_bus->master_bus, Error::ERR_INVALID_PARAMETER);

	audio_bus_map.erase(p_bus);
	audio_buses.remove_at(get_audio_bus_index(p_bus));

	AudioDriver::get_singleton()->remove_sample_bus(get_audio_bus_index(p_bus));

	audio_bus_owner.free(p_bus);

	emit_signal(SNAME("bus_layout_changed"));

	return Error::OK;
}

bool AudioServer::audio_bus_exists(RID p_bus) const {
	ERR_FAIL_COND_V(p_bus.is_null() || !p_bus.is_valid(), false);

	AuST::AudioBus *audio_bus = audio_bus_owner.get_or_null(p_bus);
	ERR_FAIL_NULL_V(audio_bus, false);

	return true;
}

Error AudioServer::move_audio_bus(RID p_bus, int p_to_index) {
	ERR_FAIL_COND_V(p_bus.is_null() || !p_bus.is_valid(), Error::ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_to_index < -1 || p_to_index > audio_buses.size(), Error::ERR_INVALID_PARAMETER);

	AuST::AudioBus *audio_bus = audio_bus_owner.get_or_null(p_bus);
	ERR_FAIL_NULL_V(audio_bus, Error::FAILED);
	ERR_FAIL_COND_V(audio_bus->master_bus, Error::ERR_INVALID_PARAMETER);

	if (p_to_index == -1) {
		audio_buses.push_back(audio_bus);
	} else if (p_to_index == 0) {
		audio_buses.insert(1, audio_bus);
	} else if (p_to_index < get_audio_bus_index(p_bus)) {
		audio_buses.insert(p_to_index, audio_bus);
	} else {
		audio_buses.insert(p_to_index - 1, audio_bus);
	}

	AudioDriver::get_singleton()->move_sample_bus(get_audio_bus_index(p_bus), p_to_index);

	emit_signal(SNAME("bus_layout_changed"));

	return Error::OK;
}

int AudioServer::get_audio_bus_count() const {
	return audio_buses.size();
}

RID AudioServer::get_audio_bus_rid(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, audio_buses.size(), RID::from_uint64(0));

	return audio_buses[p_index]->rid;
}

RID AudioServer::get_audio_bus_rid_by_name(const String &p_name) const {
	for (AuST::AudioBus *audio_bus : audio_buses) {
		if (audio_bus->name == p_name) {
			return audio_bus->rid;
		}
	}

	return RID::from_uint64(0);
}

int AudioServer::get_audio_bus_index(RID p_bus) const {
	ERR_FAIL_COND_V(p_bus.is_null() || !p_bus.is_valid(), -1);

	for (int i = 0; i < audio_buses.size(); i++) {
		if (audio_buses[i]->rid == p_bus) {
			return i;
		}
	}

	return -1;
}

Error AudioServer::set_audio_bus_name(RID p_bus, const String &p_name) {
	ERR_FAIL_COND_V(p_bus.is_null() || !p_bus.is_valid(), Error::ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_name == SceneStringName(Master) || p_name.is_empty(), Error::ERR_INVALID_PARAMETER);

	AuST::AudioBus *audio_bus = audio_bus_owner.get_or_null(p_bus);
	ERR_FAIL_NULL_V(audio_bus, Error::FAILED);
	ERR_FAIL_COND_V(audio_bus->master_bus, Error::ERR_INVALID_PARAMETER);

	String old_name = audio_bus->name;
	String name_attempt = p_name;
	int attempts = 1;
	while (true) {
		bool name_free = true;
		for (int i = 0; i < audio_buses.size(); i++) {
			if (audio_buses[i]->name == name_attempt) {
				name_free = false;
				break;
			}
		}

		if (!name_free) {
			attempts++;
			name_attempt = "New Bus " + itos(attempts);
		} else {
			break;
		}
	}
	audio_bus->name = name_attempt;

	emit_signal(SNAME("bus_renamed"), p_bus, old_name, name_attempt);

	return Error::OK;
}

String AudioServer::get_audio_bus_name(RID p_bus) const {
	ERR_FAIL_COND_V(p_bus.is_null() || !p_bus.is_valid(), String());

	AuST::AudioBus *audio_bus = audio_bus_owner.get_or_null(p_bus);
	ERR_FAIL_NULL_V(audio_bus, String());

	return audio_bus->name;
}

Error AudioServer::set_audio_bus_solo(RID p_bus, bool p_solo) {
	ERR_FAIL_COND_V(p_bus.is_null() || !p_bus.is_valid(), Error::ERR_INVALID_PARAMETER);

	AuST::AudioBus *audio_bus = audio_bus_owner.get_or_null(p_bus);
	ERR_FAIL_NULL_V(audio_bus, Error::FAILED);

	audio_bus->soloed = p_solo;

	AudioDriver::get_singleton()->set_sample_bus_solo(get_audio_bus_index(p_bus), p_solo);

	return Error::OK;
}

bool AudioServer::is_audio_bus_soloed(RID p_bus) const {
	ERR_FAIL_COND_V(p_bus.is_null() || !p_bus.is_valid(), false);

	AuST::AudioBus *audio_bus = audio_bus_owner.get_or_null(p_bus);
	ERR_FAIL_NULL_V(audio_bus, false);

	return audio_bus->soloed;
}

Error AudioServer::set_audio_bus_mute(RID p_bus, bool p_mute) {
	ERR_FAIL_COND_V(p_bus.is_null() || !p_bus.is_valid(), Error::ERR_INVALID_PARAMETER);

	AuST::AudioBus *audio_bus = audio_bus_owner.get_or_null(p_bus);
	ERR_FAIL_NULL_V(audio_bus, Error::FAILED);

	audio_bus->muted = p_mute;

	AudioDriver::get_singleton()->set_sample_bus_mute(get_audio_bus_index(p_bus), p_mute);

	return Error::OK;
}

bool AudioServer::is_audio_bus_muted(RID p_bus) const {
	ERR_FAIL_COND_V(p_bus.is_null() || !p_bus.is_valid(), false);

	AuST::AudioBus *audio_bus = audio_bus_owner.get_or_null(p_bus);
	ERR_FAIL_NULL_V(audio_bus, false);

	return audio_bus->muted;
}

Error AudioServer::set_audio_bus_bypass(RID p_bus, bool p_bypass) {
	ERR_FAIL_COND_V(p_bus.is_null() || !p_bus.is_valid(), Error::ERR_INVALID_PARAMETER);

	AuST::AudioBus *audio_bus = audio_bus_owner.get_or_null(p_bus);
	ERR_FAIL_NULL_V(audio_bus, Error::FAILED);

	audio_bus->bypassed = p_bypass;

	return Error::OK;
}

bool AudioServer::is_audio_bus_bypassed(RID p_bus) const {
	ERR_FAIL_COND_V(p_bus.is_null() || !p_bus.is_valid(), false);

	AuST::AudioBus *audio_bus = audio_bus_owner.get_or_null(p_bus);
	ERR_FAIL_NULL_V(audio_bus, false);

	return audio_bus->bypassed;
}

Error AudioServer::set_audio_bus_volume(RID p_bus, float p_volume) {
	ERR_FAIL_COND_V(p_bus.is_null() || !p_bus.is_valid(), Error::ERR_INVALID_PARAMETER);

	AuST::AudioBus *audio_bus = audio_bus_owner.get_or_null(p_bus);
	ERR_FAIL_NULL_V(audio_bus, Error::FAILED);

	audio_bus->volume_db = p_volume;

	AudioDriver::get_singleton()->set_sample_bus_volume_db(get_audio_bus_index(p_bus), p_volume);

	return Error::OK;
}

float AudioServer::get_audio_bus_volume(RID p_bus) const {
	ERR_FAIL_COND_V(p_bus.is_null() || !p_bus.is_valid(), 0.0);

	AuST::AudioBus *audio_bus = audio_bus_owner.get_or_null(p_bus);
	ERR_FAIL_NULL_V(audio_bus, 0.0);

	return audio_bus->volume_db;
}

RID AudioServer::add_audio_bus_effect(RID p_bus, const Ref<AudioEffect> &p_effect, int p_at_index) {
	ERR_FAIL_COND_V(p_bus.is_null() || !p_bus.is_valid(), RID::from_uint64(0));
	ERR_FAIL_NULL_V(p_effect, RID::from_uint64(0));
	ERR_FAIL_COND_V(p_at_index < -1 || p_at_index > audio_buses.size(), RID::from_uint64(0));

	AuST::AudioBus *audio_bus = audio_bus_owner.get_or_null(p_bus);
	ERR_FAIL_NULL_V(audio_bus, RID::from_uint64(0));

	RID rid = effect_owner.make_rid();
	AuST::BusEffect *effect = effect_owner.get_or_null(rid);
	ERR_FAIL_NULL_V(effect, RID::from_uint64(0));

	effect->rid = rid;
	effect->audio_bus_rid = audio_bus->rid;
	effect->effect = p_effect;
	effect->enabled = false;

#ifdef DEBUG_ENABLED
	effect->prof_time = 0;
#endif // DEBUG_ENABLED

	if (p_at_index < 0) {
		audio_bus->effects.push_back(effect);
	} else {
		audio_bus->effects.insert(p_at_index, effect);
	}

	_update_bus_effects(p_bus);

	return rid;
}

Error AudioServer::remove_audio_bus_effect(RID p_effect) {
	ERR_FAIL_COND_V(p_effect.is_null() || !p_effect.is_valid(), Error::ERR_INVALID_PARAMETER);

	AuST::BusEffect *effect = effect_owner.get_or_null(p_effect);
	ERR_FAIL_NULL_V(effect, Error::FAILED);

	AuST::AudioBus *audio_bus = audio_bus_owner.get_or_null(effect->audio_bus_rid);
	ERR_FAIL_NULL_V(audio_bus, Error::FAILED);

	audio_bus->effects.remove_at(get_audio_bus_effect_index(p_effect));

	effect_owner.free(p_effect);

	_update_bus_effects(audio_bus->rid);

	return Error::OK;
}

Error AudioServer::clear_audio_bus_effects(RID p_bus) {
	ERR_FAIL_COND_V(p_bus.is_null() || !p_bus.is_valid(), Error::ERR_INVALID_PARAMETER);

	AuST::AudioBus *audio_bus = audio_bus_owner.get_or_null(p_bus);
	ERR_FAIL_NULL_V(audio_bus, Error::FAILED);

	for (AuST::BusEffect *effect : audio_bus->effects) {
		RID effect_rid = effect->rid;
		effect_owner.free(effect_rid);
	}
	audio_bus->effects.clear();

	_update_bus_effects(p_bus);

	return Error::OK;
}

Ref<AudioEffect> AudioServer::get_audio_bus_effect(RID p_effect) const {
	ERR_FAIL_COND_V(p_effect.is_null() || !p_effect.is_valid(), Ref<AudioEffect>());

	AuST::BusEffect *effect = effect_owner.get_or_null(p_effect);
	ERR_FAIL_NULL_V(effect, Ref<AudioEffect>());

	return effect->effect;
}

Ref<AudioEffectInstance> AudioServer::get_audio_bus_effect_instance(RID p_effect, int p_channel_pair) const {
	ERR_FAIL_COND_V(p_effect.is_null() || !p_effect.is_valid(), Ref<AudioEffectInstance>());
	ERR_FAIL_COND_V(p_channel_pair > channel_pair_count - 1, Ref<AudioEffectInstance>());

	AuST::BusEffect *effect = effect_owner.get_or_null(p_effect);
	ERR_FAIL_NULL_V(effect, Ref<AudioEffectInstance>());

	AuST::AudioBus *audio_bus = audio_bus_owner.get_or_null(effect->audio_bus_rid);
	ERR_FAIL_NULL_V(audio_bus, Error::FAILED);

	return audio_bus->channel_pairs[p_channel_pair].effect_instances[get_audio_bus_effect_index(p_effect)];
}

Error AudioServer::set_audio_bus_effect_enable(RID p_effect, bool p_enable) {
	ERR_FAIL_COND_V(p_effect.is_null() || !p_effect.is_valid(), Error::ERR_INVALID_PARAMETER);

	AuST::BusEffect *effect = effect_owner.get_or_null(p_effect);
	ERR_FAIL_NULL_V(effect, Error::FAILED);

	effect->enabled = p_enable;

	return Error::OK;
}

bool AudioServer::is_audio_bus_effect_enabled(RID p_effect) const {
	ERR_FAIL_COND_V(p_effect.is_null() || !p_effect.is_valid(), Error::ERR_INVALID_PARAMETER);

	AuST::BusEffect *effect = effect_owner.get_or_null(p_effect);
	ERR_FAIL_NULL_V(effect, Error::FAILED);

	return effect->enabled;
}

RID AudioServer::get_audio_bus_effect_rid(RID p_bus, int p_effect) const {
	ERR_FAIL_COND_V(p_bus.is_null() || !p_bus.is_valid(), RID::from_uint64(0));

	AuST::AudioBus *audio_bus = audio_bus_owner.get_or_null(p_bus);
	ERR_FAIL_NULL_V(audio_bus, RID::from_uint64(0));

	ERR_FAIL_COND_V(p_effect >= audio_bus->effects.size());

	return audio_bus->effects[p_effect]->rid;
}

int AudioServer::get_audio_bus_effect_index(RID p_effect) const {
	ERR_FAIL_COND_V(p_effect.is_null() || !p_effect.is_valid(), -1);

	AuST::BusEffect *effect = effect_owner.get_or_null(p_effect);
	ERR_FAIL_NULL_V(effect, -1);

	AuST::AudioBus *audio_bus = audio_bus_owner.get_or_null(effect->audio_bus_rid);
	ERR_FAIL_NULL_V(audio_bus, -1);

	for (int i = 0; i < audio_bus->effects.size(); i++) {
		if (audio_bus->effects[i]->rid == p_effect) {
			return i;
		}
	}

	return -1;
}
int AudioServer::get_audio_bus_effect_count(RID p_bus) const {
	ERR_FAIL_COND_V(p_bus.is_null() || !p_bus.is_valid(), Error::ERR_INVALID_PARAMETER);

	AuST::AudioBus *audio_bus = audio_bus_owner.get_or_null(p_bus);
	ERR_FAIL_NULL_V(audio_bus, Error::FAILED);

	return audio_bus->effects.size();
}

Error AudioServer::swap_audio_bus_effects(RID p_bus, int p_index_1, int p_index_2) {
	ERR_FAIL_COND_V(p_bus.is_null() || !p_bus.is_valid(), Error::ERR_INVALID_PARAMETER);

	AuST::AudioBus *audio_bus = audio_bus_owner.get_or_null(p_bus);
	ERR_FAIL_NULL_V(audio_bus, Error::FAILED);
	ERR_FAIL_COND_V(MAX(p_index_1, p_index_2) >= audio_bus->effects.size(), Error::ERR_INVALID_PARAMETER);

	// TODO: Use `ptrw()`.
	SWAP(audio_bus->effects.write[p_index_1], audio_bus->effects.write[p_index_2]);

	_update_bus_effects(p_bus);
}

Error AudioServer::set_audio_bus_send(RID p_bus, RID p_send) {
	ERR_FAIL_COND_V(p_bus.is_null() || !p_bus.is_valid(), Error::ERR_INVALID_PARAMETER);

	AuST::AudioBus *audio_bus = audio_bus_owner.get_or_null(p_bus);
	ERR_FAIL_NULL_V(audio_bus, Error::FAILED);
	AuST::AudioBus *send = audio_bus_owner.get_or_null(p_bus);
	ERR_FAIL_NULL_V(send, Error::FAILED);

	audio_bus->send = p_send;

	AudioDriver::get_singleton()->set_sample_bus_send(get_audio_bus_index(p_bus), send->name);

	return Error::OK;
}

RID AudioServer::get_audio_bus_send(RID p_bus) const {
	ERR_FAIL_COND_V(p_bus.is_null() || !p_bus.is_valid(), RID::from_uint64(0));

	AuST::AudioBus *audio_bus = audio_bus_owner.get_or_null(p_bus);
	ERR_FAIL_NULL_V(audio_bus, RID::from_uint64(0));

	return audio_bus->send;
}

AudioFrame AudioServer::get_audio_bus_peak_volume(RID p_bus, int p_channel_pair) const {
	ERR_FAIL_COND_V(p_bus.is_null() || !p_bus.is_valid(), AudioFrame(0.0, 0.0));
	ERR_FAIL_COND_V(p_channel_pair >= channel_pair_count, AudioFrame(0.0, 0.0));

	AuST::AudioBus *audio_bus = audio_bus_owner.get_or_null(p_bus);
	ERR_FAIL_NULL_V(audio_bus, AudioFrame(0.0, 0.0));

	return audio_bus->channel_pairs[p_channel_pair].peak_volume_linear;
}

// Private helpers

Vector<AudioFrame> AudioServer::_get_volume_vector(RID p_emitter) {
	ERR_FAIL_COND(p_emitter.is_null() || !p_emitter.is_valid());

	Vector<AudioFrame> volume_vector;
	volume_vector.resize(AuSC::MAX_CHANNEL_PAIRS_PER_BUS);

	for (AudioFrame &channel_pair_volume : volume_vector) {
		channel_pair_volume = AudioFrame(0, 0);
	}

	AuST::AudioEmitter *emitter = emitter_owner.get_or_null(p_emitter);
	ERR_FAIL_NULL_V(emitter, volume_vector);

	if (emitter->type != AuSE::EmitterType::NON_POSITIONAL && emitter->stage.type != AuSE::StageType::STAGE_DISABLED) {
		for (AudioFrame &channel_pair_volume : volume_vector) {
			channel_pair_volume = AudioFrame(1.0, 1.0);
		}
		return volume_vector;
	}

	AudioFrame *volume_vector_ptrw = volume_vector.ptrw();
	switch (emitter->mix_target) {
		case AuSE::MixTarget::MIX_TARGET_STEREO:
			volume_vector_ptrw[0] = AudioFrame(1.0, 1.0);
			break;
		case AuSE::MixTarget::MIX_TARGET_SURROUND:
			volume_vector_ptrw[0] = AudioFrame(1.0, 1.0);
			volume_vector_ptrw[1] = AudioFrame(1.0, 1.0);
			volume_vector_ptrw[2] = AudioFrame(1.0, 1.0);
			volume_vector_ptrw[3] = AudioFrame(1.0, 1.0);
			break;
		case AuSE::MixTarget::MIX_TARGET_CENTER:
			volume_vector_ptrw[1] = AudioFrame(1.0, 1.0);
	}

	return volume_vector;
}

// This calculates attenuation filters. Should be called in `mix_step()`.
void AudioServer::_update_stage(RID p_emitter) {
	ERR_FAIL_COND(p_emitter.is_null() || !p_emitter.is_valid());

	// TODO: Add to this, after figuring out how to implement effects.
}

Error AudioServer::_set_master_audio_bus(RID p_bus, bool p_enable) {
	ERR_FAIL_COND_V(p_bus.is_null() || !p_bus.is_valid(), Error::ERR_INVALID_PARAMETER);

	AuST::AudioBus *audio_bus = audio_bus_owner.get_or_null(p_bus);
	ERR_FAIL_NULL_V(audio_bus, Error::FAILED);

	audio_bus->master_bus = p_enable;

	return Error::OK;
}

RID AudioServer::_get_master_audio_bus() const {
	ERR_FAIL_COND_V(audio_buses.is_empty(), RID::from_uint64(0));

	for (AuST::AudioBus *audio_bus : audio_buses) {
		if (audio_bus->master_bus) {
			return audio_bus->rid;
		}
	}

	return RID::from_uint64(0);
}

// End of new RID methods.

// Old methods.

// TODO: These methods should be deprecated, and replaced throughout the codebase.

void AudioServer::remove_bus(int p_index) {
	ERR_FAIL_INDEX(p_index, audio_buses.size());
	ERR_FAIL_COND(p_index == 0);

	MARK_EDITED

	remove_audio_bus(get_audio_bus_rid(p_index));
}

void AudioServer::add_bus(int p_at_pos) {
	MARK_EDITED

	add_audio_bus(p_at_pos);
}

void AudioServer::move_bus(int p_bus, int p_to_pos) {
	ERR_FAIL_COND(p_bus < 1 || p_bus >= audio_buses.size());
	ERR_FAIL_COND(p_to_pos != -1 && (p_to_pos < 1 || p_to_pos > audio_buses.size()));

	MARK_EDITED

	if (p_bus == p_to_pos) {
		return;
	}

	move_audio_bus(get_audio_bus_rid(p_bus), p_to_pos);
}

int AudioServer::get_bus_count() const {
	return get_audio_bus_count();
}

void AudioServer::set_bus_name(int p_bus, const String &p_name) {
	ERR_FAIL_INDEX(p_bus, audio_buses.size());
	if (p_bus == 0 && p_name != SceneStringName(Master)) {
		return; // Bus 0 is always "Master".
	}

	MARK_EDITED

	set_audio_bus_name(get_audio_bus_rid(p_bus), p_name);
}

String AudioServer::get_bus_name(int p_bus) const {
	ERR_FAIL_INDEX_V(p_bus, audio_buses.size(), String());

	return get_audio_bus_name(get_audio_bus_rid(p_bus));
}

int AudioServer::get_bus_index(const StringName &p_bus_name) const {
	return get_audio_bus_index(get_audio_bus_rid_by_name(p_bus_name));
}

void AudioServer::set_bus_volume_db(int p_bus, float p_volume_db) {
	ERR_FAIL_INDEX(p_bus, audio_buses.size());

	MARK_EDITED

	set_audio_bus_volume(get_audio_bus_rid(p_bus), p_volume_db);
}

float AudioServer::get_bus_volume_db(int p_bus) const {
	ERR_FAIL_INDEX_V(p_bus, audio_buses.size(), 0);

	return get_audio_bus_volume(get_audio_bus_rid(p_bus));
}

void AudioServer::set_bus_volume_linear(int p_bus, float p_volume_linear) {
	set_audio_bus_volume(get_audio_bus_rid(p_bus), Math::linear_to_db(p_volume_linear));
}

float AudioServer::get_bus_volume_linear(int p_bus) const {
	ERR_FAIL_INDEX_V(p_bus, audio_buses.size(), 0);

	return Math::db_to_linear(get_audio_bus_volume(get_audio_bus_rid(p_bus)));
}

// TODO: This should just return `channel_pair_count`.
int AudioServer::get_bus_channels(int p_bus) const {
	ERR_FAIL_INDEX_V(p_bus, audio_buses.size(), 0);

	return audio_buses[p_bus]->channel_pairs.size();
}

void AudioServer::set_bus_send(int p_bus, const StringName &p_send) {
	ERR_FAIL_INDEX(p_bus, audio_buses.size());

	MARK_EDITED

	set_audio_bus_send(get_audio_bus_rid(p_bus), get_audio_bus_rid(get_bus_index(p_send)));
}

StringName AudioServer::get_bus_send(int p_bus) const {
	ERR_FAIL_INDEX_V(p_bus, audio_buses.size(), StringName());

	AuST::AudioBus *send = audio_bus_owner.get_or_null(get_audio_bus_send(get_audio_bus_rid(p_bus)));
	ERR_FAIL_NULL_V(send, StringName());

	return send->name;
}

void AudioServer::set_bus_solo(int p_bus, bool p_enable) {
	ERR_FAIL_INDEX(p_bus, audio_buses.size());

	MARK_EDITED

	set_audio_bus_solo(get_audio_bus_rid(p_bus), p_enable);
}

bool AudioServer::is_bus_solo(int p_bus) const {
	ERR_FAIL_INDEX_V(p_bus, audio_buses.size(), false);

	return is_audio_bus_soloed(get_audio_bus_rid(p_bus));
}

void AudioServer::set_bus_mute(int p_bus, bool p_enable) {
	ERR_FAIL_INDEX(p_bus, audio_buses.size());

	MARK_EDITED

	set_audio_bus_mute(get_audio_bus_rid(p_bus), p_enable);
}

bool AudioServer::is_bus_mute(int p_bus) const {
	ERR_FAIL_INDEX_V(p_bus, audio_buses.size(), false);

	return is_audio_bus_muted(get_audio_bus_rid(p_bus));
}

void AudioServer::set_bus_bypass_effects(int p_bus, bool p_enable) {
	ERR_FAIL_INDEX(p_bus, audio_buses.size());

	MARK_EDITED

	set_audio_bus_bypass(get_audio_bus_rid(p_bus), p_enable);
}

bool AudioServer::is_bus_bypassing_effects(int p_bus) const {
	ERR_FAIL_INDEX_V(p_bus, audio_buses.size(), false);

	return is_audio_bus_bypassed(get_audio_bus_rid(p_bus));
}

void AudioServer::_update_bus_effects(RID p_bus) {
	AuST::BusChannelPair *channel_pairs_ptrw = audio_bus_map[p_bus]->channel_pairs.ptrw();
	Ref<AudioEffectInstance> *effect_instances_ptrw;
	for (int i = 0; i < audio_bus_map[p_bus]->channel_pairs.size(); i++) {
		channel_pairs_ptrw[i].effect_instances.resize(audio_bus_map[p_bus]->effects.size());
		effect_instances_ptrw = channel_pairs_ptrw[i].effect_instances.ptrw();
		for (int j = 0; j < audio_bus_map[p_bus]->effects.size(); j++) {
			Ref<AudioEffectInstance> effect_instance = audio_bus_map[p_bus]->effects.write[j]->effect->instantiate();
			if (Object::cast_to<AudioEffectCompressorInstance>(*effect_instance)) {
				Object::cast_to<AudioEffectCompressorInstance>(*effect_instance)->set_current_channel_pair(i);
			}
			effect_instances_ptrw[j] = effect_instance;
		}
	}
}

void AudioServer::add_bus_effect(int p_bus, const Ref<AudioEffect> &p_effect, int p_at_pos) {
	ERR_FAIL_COND(p_effect.is_null());
	ERR_FAIL_INDEX(p_bus, audio_buses.size());

	MARK_EDITED

	add_audio_bus_effect(get_audio_bus_rid(p_bus), p_effect, p_at_pos);
}

void AudioServer::remove_bus_effect(int p_bus, int p_effect) {
	ERR_FAIL_INDEX(p_bus, audio_buses.size());

	MARK_EDITED

	remove_audio_bus_effect(get_audio_bus_effect_rid(get_audio_bus_rid(p_bus), p_effect));
}

int AudioServer::get_bus_effect_count(int p_bus) {
	ERR_FAIL_INDEX_V(p_bus, audio_buses.size(), 0);

	return get_audio_bus_effect_count(get_audio_bus_rid(p_bus));
}

Ref<AudioEffectInstance> AudioServer::get_bus_effect_instance(int p_bus, int p_effect, int p_channel) {
	ERR_FAIL_INDEX_V(p_bus, audio_buses.size(), Ref<AudioEffectInstance>());
	ERR_FAIL_COND_V(p_channel >= channel_pair_count, Ref<AudioEffectInstance>());

	return get_audio_bus_effect_instance(get_audio_bus_effect_rid(get_audio_bus_rid(p_bus), p_effect), p_channel);
}

Ref<AudioEffect> AudioServer::get_bus_effect(int p_bus, int p_effect) {
	ERR_FAIL_INDEX_V(p_bus, audio_buses.size(), Ref<AudioEffect>());

	return get_audio_bus_effect(get_audio_bus_effect_rid(get_audio_bus_rid(p_bus), p_effect));
}

void AudioServer::swap_bus_effects(int p_bus, int p_effect, int p_by_effect) {
	ERR_FAIL_INDEX(p_bus, audio_buses.size());

	MARK_EDITED

	swap_audio_bus_effects(get_audio_bus_rid(p_bus), p_effect, p_by_effect);
}

void AudioServer::set_bus_effect_enabled(int p_bus, int p_effect, bool p_enabled) {
	ERR_FAIL_INDEX(p_bus, audio_buses.size());

	MARK_EDITED

	set_audio_bus_effect_enable(get_audio_bus_effect_rid(get_audio_bus_rid(p_bus), p_effect), p_enabled);
}

bool AudioServer::is_bus_effect_enabled(int p_bus, int p_effect) const {
	ERR_FAIL_INDEX_V(p_bus, audio_buses.size(), false);

	return is_audio_bus_effect_enabled(get_audio_bus_effect_rid(get_audio_bus_rid(p_bus), p_effect));
}

float AudioServer::get_bus_peak_volume_left_db(int p_bus, int p_channel) const {
	ERR_FAIL_INDEX_V(p_bus, audio_buses.size(), 0.0);

	return Math::linear_to_db(get_audio_bus_peak_volume(get_audio_bus_rid(p_bus), p_channel).left);
}

float AudioServer::get_bus_peak_volume_right_db(int p_bus, int p_channel) const {
	ERR_FAIL_INDEX_V(p_bus, audio_buses.size(), 0.0);

	return Math::linear_to_db(get_audio_bus_peak_volume(get_audio_bus_rid(p_bus), p_channel).right);
}

// bool AudioServer::is_bus_channel_active(int p_bus, int p_channel) const {
// 	ERR_FAIL_INDEX_V(p_bus, buses.size(), false);
// 	ERR_FAIL_INDEX_V(p_channel, buses[p_bus]->channels.size(), false);

// 	return buses[p_bus]->channels[p_channel].active;
// }

void AudioServer::set_playback_speed_scale(float p_scale) {
	ERR_FAIL_COND(p_scale <= 0);

	playback_speed_scale = p_scale;
}

float AudioServer::get_playback_speed_scale() const {
	return playback_speed_scale;
}

// void AudioServer::start_playback_stream(Ref<AudioStreamPlayback> p_playback, const StringName &p_bus, Vector<AudioFrame> p_volume_db_vector, float p_start_time, float p_pitch_scale) {
// 	ERR_FAIL_COND(p_playback.is_null());

// 	HashMap<StringName, Vector<AudioFrame>> map;
// 	map[p_bus] = p_volume_db_vector;

// 	start_playback_stream(p_playback, map, p_start_time, p_pitch_scale);
// }

// void AudioServer::start_playback_stream(Ref<AudioStreamPlayback> p_playback, const HashMap<StringName, Vector<AudioFrame>> &p_bus_volumes, float p_start_time, float p_pitch_scale, float p_highshelf_gain, float p_attenuation_cutoff_hz) {
// 	ERR_FAIL_COND(p_playback.is_null());

// 	AudioStreamPlaybackListNode *playback_node = new AudioStreamPlaybackListNode();
// 	playback_node->stream_playback = p_playback;
// 	playback_node->stream_playback->start(p_start_time);

// 	AudioStreamPlaybackBusDetails *new_bus_details = new AudioStreamPlaybackBusDetails();
// 	int idx = 0;
// 	for (KeyValue<StringName, Vector<AudioFrame>> pair : p_bus_volumes) {
// 		if (pair.value.size() < channel_count || pair.value.size() != AuSC::MAX_CHANNELS_PER_BUS) {
// 			delete playback_node;
// 			delete new_bus_details;
// 			ERR_FAIL();
// 		}

// 		new_bus_details->bus_active[idx] = true;
// 		new_bus_details->bus[idx] = pair.key;
// 		for (int channel_idx = 0; channel_idx < AuSC::MAX_CHANNELS_PER_BUS; channel_idx++) {
// 			new_bus_details->volume[idx][channel_idx] = pair.value[channel_idx];
// 		}
// 		idx++;
// 	}
// 	playback_node->bus_details.store(new_bus_details);
// 	playback_node->prev_bus_details = new AudioStreamPlaybackBusDetails();

// 	playback_node->pitch_scale.set(p_pitch_scale);
// 	playback_node->highshelf_gain.set(p_highshelf_gain);
// 	playback_node->attenuation_filter_cutoff_hz.set(p_attenuation_cutoff_hz);

// 	memset(playback_node->prev_bus_details->volume, 0, sizeof(playback_node->prev_bus_details->volume));

// 	for (AudioFrame &frame : playback_node->lookahead) {
// 		frame = AudioFrame(0, 0);
// 	}

// 	playback_node->state.store(AudioStreamPlaybackListNode::PLAYING);

// 	playback_list.insert(playback_node);
// }

// void AudioServer::stop_playback_stream(Ref<AudioStreamPlayback> p_playback) {
// 	ERR_FAIL_COND(p_playback.is_null());

// 	// Handle sample playback.
// 	if (p_playback->get_is_sample()) {
// 		if (p_playback->get_sample_playback().is_valid()) {
// 			AudioServer::get_singleton()->stop_sample_playback(p_playback->get_sample_playback());
// 		} else {
// 			_delete_stream_playback(p_playback);
// 		}
// 		return;
// 	}

// 	if (!p_playback->is_playing()) {
// 		p_playback->stop();
// 	}

// 	AudioStreamPlaybackListNode *playback_node = _find_playback_list_node(p_playback);
// 	if (!playback_node) {
// 		return;
// 	}

// 	AudioStreamPlaybackListNode::PlaybackState new_state, old_state;
// 	do {
// 		old_state = playback_node->state.load();
// 		if (old_state == AudioStreamPlaybackListNode::AWAITING_DELETION) {
// 			break; // Don't fade out again.
// 		}
// 		new_state = AudioStreamPlaybackListNode::FADE_OUT_TO_DELETION;

// 	} while (!playback_node->state.compare_exchange_strong(old_state, new_state));
// }

// void AudioServer::set_playback_bus_exclusive(Ref<AudioStreamPlayback> p_playback, const StringName &p_bus, Vector<AudioFrame> p_volumes) {
// 	ERR_FAIL_COND(p_volumes.size() != AuSC::MAX_CHANNELS_PER_BUS);

// 	HashMap<StringName, Vector<AudioFrame>> map;
// 	map[p_bus] = p_volumes;

// 	set_playback_bus_volumes_linear(p_playback, map);
// }

// void AudioServer::set_playback_bus_volumes_linear(Ref<AudioStreamPlayback> p_playback, const HashMap<StringName, Vector<AudioFrame>> &p_bus_volumes) {
// 	ERR_FAIL_COND(p_bus_volumes.size() > AuSC::MAX_BUSES_PER_PLAYBACK);

// 	// Samples.
// 	if (p_playback->get_is_sample() && p_playback->get_sample_playback().is_valid()) {
// 		Ref<AudioSamplePlayback> sample_playback = p_playback->get_sample_playback();
// 		AudioDriver::get_singleton()->set_sample_playback_bus_volumes_linear(sample_playback, p_bus_volumes);
// 		return;
// 	}

// 	AudioStreamPlaybackListNode *playback_node = _find_playback_list_node(p_playback);
// 	if (!playback_node) {
// 		return;
// 	}
// 	AudioStreamPlaybackBusDetails *old_bus_details, *new_bus_details = new AudioStreamPlaybackBusDetails();

// 	int idx = 0;
// 	for (KeyValue<StringName, Vector<AudioFrame>> pair : p_bus_volumes) {
// 		if (idx >= AuSC::MAX_BUSES_PER_PLAYBACK) {
// 			break;
// 		}
// 		if (pair.value.size() < channel_count || pair.value.size() != AuSC::MAX_CHANNELS_PER_BUS) {
// 			delete new_bus_details;
// 			ERR_FAIL();
// 		}

// 		new_bus_details->bus_active[idx] = true;
// 		new_bus_details->bus[idx] = pair.key;
// 		for (int channel_idx = 0; channel_idx < AuSC::MAX_CHANNELS_PER_BUS; channel_idx++) {
// 			new_bus_details->volume[idx][channel_idx] = pair.value[channel_idx];
// 		}
// 		idx++;
// 	}

// 	do {
// 		old_bus_details = playback_node->bus_details.load();
// 	} while (!playback_node->bus_details.compare_exchange_strong(old_bus_details, new_bus_details));

// 	bus_details_graveyard.insert(old_bus_details);
// }

// void AudioServer::set_playback_all_bus_volumes_linear(Ref<AudioStreamPlayback> p_playback, Vector<AudioFrame> p_volumes) {
// 	ERR_FAIL_COND(p_playback.is_null());
// 	ERR_FAIL_COND(p_volumes.size() != AuSC::MAX_CHANNELS_PER_BUS);

// 	HashMap<StringName, Vector<AudioFrame>> map;

// 	AudioStreamPlaybackListNode *playback_node = _find_playback_list_node(p_playback);
// 	if (!playback_node) {
// 		return;
// 	}
// 	for (int bus_idx = 0; bus_idx < AuSC::MAX_BUSES_PER_PLAYBACK; bus_idx++) {
// 		if (playback_node->bus_details.load()->bus_active[bus_idx]) {
// 			map[playback_node->bus_details.load()->bus[bus_idx]] = p_volumes;
// 		}
// 	}

// 	set_playback_bus_volumes_linear(p_playback, map);
// }

// void AudioServer::set_playback_pitch_scale(Ref<AudioStreamPlayback> p_playback, float p_pitch_scale) {
// 	ERR_FAIL_COND(p_playback.is_null());

// 	// Samples.
// 	if (p_playback->get_is_sample() && p_playback->get_sample_playback().is_valid()) {
// 		Ref<AudioSamplePlayback> sample_playback = p_playback->get_sample_playback();
// 		AudioServer::get_singleton()->update_sample_playback_pitch_scale(sample_playback, p_pitch_scale);
// 		return;
// 	}

// 	AudioStreamPlaybackListNode *playback_node = _find_playback_list_node(p_playback);
// 	if (!playback_node) {
// 		return;
// 	}

// 	playback_node->pitch_scale.set(p_pitch_scale);
// }

// void AudioServer::set_playback_paused(Ref<AudioStreamPlayback> p_playback, bool p_paused) {
// 	ERR_FAIL_COND(p_playback.is_null());

// 	AudioStreamPlaybackListNode *playback_node = _find_playback_list_node(p_playback);
// 	if (!playback_node) {
// 		return;
// 	}

// 	AudioStreamPlaybackListNode::PlaybackState new_state, old_state;
// 	do {
// 		old_state = playback_node->state.load();
// 		new_state = p_paused ? AudioStreamPlaybackListNode::FADE_OUT_TO_PAUSE : AudioStreamPlaybackListNode::PLAYING;
// 		if (!p_paused && old_state == AudioStreamPlaybackListNode::PLAYING) {
// 			return; // No-op.
// 		}
// 		if (p_paused && (old_state == AudioStreamPlaybackListNode::PAUSED || old_state == AudioStreamPlaybackListNode::FADE_OUT_TO_PAUSE)) {
// 			return; // No-op.
// 		}

// 	} while (!playback_node->state.compare_exchange_strong(old_state, new_state));
// }

// void AudioServer::set_playback_highshelf_params(Ref<AudioStreamPlayback> p_playback, float p_gain, float p_attenuation_cutoff_hz) {
// 	ERR_FAIL_COND(p_playback.is_null());

// 	AudioStreamPlaybackListNode *playback_node = _find_playback_list_node(p_playback);
// 	if (!playback_node) {
// 		return;
// 	}

// 	playback_node->attenuation_filter_cutoff_hz.set(p_attenuation_cutoff_hz);
// 	playback_node->highshelf_gain.set(p_gain);
// }

// bool AudioServer::is_playback_active(Ref<AudioStreamPlayback> p_playback) {
// 	ERR_FAIL_COND_V(p_playback.is_null(), false);

// 	if (p_playback->get_is_sample()) {
// 		if (p_playback->get_sample_playback().is_valid()) {
// 			return sample_playback_list.has(p_playback->get_sample_playback());
// 		} else {
// 			return false;
// 		}
// 	}

// 	AudioStreamPlaybackListNode *playback_node = _find_playback_list_node(p_playback);
// 	if (!playback_node) {
// 		return false;
// 	}

// 	return playback_node->state.load() == AudioStreamPlaybackListNode::PLAYING;
// }

// float AudioServer::get_playback_position(Ref<AudioStreamPlayback> p_playback) {
// 	ERR_FAIL_COND_V(p_playback.is_null(), 0);

// 	// Samples.
// 	if (p_playback->get_is_sample() && p_playback->get_sample_playback().is_valid()) {
// 		Ref<AudioSamplePlayback> sample_playback = p_playback->get_sample_playback();
// 		return AudioServer::get_singleton()->get_sample_playback_position(sample_playback);
// 	}

// 	AudioStreamPlaybackListNode *playback_node = _find_playback_list_node(p_playback);
// 	if (!playback_node) {
// 		return 0;
// 	}

// 	return playback_node->stream_playback->get_playback_position();
// }

// bool AudioServer::is_playback_paused(Ref<AudioStreamPlayback> p_playback) {
// 	ERR_FAIL_COND_V(p_playback.is_null(), false);

// 	AudioStreamPlaybackListNode *playback_node = _find_playback_list_node(p_playback);
// 	if (!playback_node) {
// 		return false;
// 	}

// 	return playback_node->state.load() == AudioStreamPlaybackListNode::PAUSED || playback_node->state.load() == AudioStreamPlaybackListNode::FADE_OUT_TO_PAUSE;
// }

uint64_t AudioServer::get_mix_count() const {
	return mix_count;
}

uint64_t AudioServer::get_mixed_frames() const {
	return mix_frames;
}

String AudioServer::get_driver_name() const {
	return AudioDriver::get_singleton()->get_name();
}

// void AudioServer::notify_listener_changed() {
// 	for (CallbackItem *ci : listener_changed_callback_list) {
// 		ci->callback(ci->userdata);
// 	}
// }

void AudioServer::init_channels_and_buffers() {
	// TODO: Use `ptrw()`.
	channel_pair_count = get_channel_pair_count();
	temp_buffer.resize(channel_pair_count);
	mix_buffer.resize(buffer_size + AuSC::LOOKAHEAD_BUFFER_SIZE);

	// Vector<AudioFrame> *temp_buffer_ptrw = temp_buffer.ptrw();
	for (int i = 0; i < temp_buffer.size(); i++) {
		temp_buffer.write[i].resize(buffer_size);
	}

	// AuST::AudioBus *audio_buses_ptrw = audio_buses.ptrw();
	AuST::BusChannelPair *channels_ptrw;
	for (int i = 0; i < audio_buses.size(); i++) {
		audio_buses.write[i]->channel_pairs.resize(channel_pair_count);
		channels_ptrw = audio_buses.write[i]->channel_pairs.ptrw();
		for (int j = 0; j < channel_pair_count; j++) {
			channels_ptrw[i].buffer.resize(buffer_size);
		}
		_update_bus_effects(get_audio_bus_rid(i));
	}
}

void AudioServer::init() {
	channel_disable_threshold_db = GLOBAL_DEF_RST(PropertyInfo(Variant::FLOAT, "audio/buses/channel_disable_threshold_db", PROPERTY_HINT_RANGE, "-80,0,0.1,suffix:dB"), -60.0);
	channel_disable_frames = float(GLOBAL_DEF_RST(PropertyInfo(Variant::FLOAT, "audio/buses/channel_disable_time", PROPERTY_HINT_RANGE, "0,5,0.01,or_greater"), 2.0)) * get_mix_rate();
	// TODO: Buffer size is hardcoded for now. This would be really nice to have as a project setting because currently it limits audio latency to an absolute minimum of 11ms with default mix rate, but there's some additional work required to make that happen. See TODOs in `_mix_step_for_channel`.
	// When this becomes a project setting, it should be specified in milliseconds rather than raw sample count, because 512 samples at 192khz is shorter than it is at 48khz, for example.
	buffer_size = 512;

	init_channels_and_buffers();

	mix_count = 0;
	// set_bus_count(1);
	// set_bus_name(0, "Master");
	RID master_bus_rid = add_audio_bus(0);
	set_audio_bus_name(master_bus_rid, SceneStringName(Master));
	_set_master_audio_bus(master_bus_rid, true);

	if (AudioDriver::get_singleton()) {
		AudioDriver::get_singleton()->start();
		AudioDriver::get_singleton()->set_sample_bus_count(1);
	}

#ifdef TOOLS_ENABLED
	set_edited(false); // Avoid editors from thinking this was edited.
#endif

	GLOBAL_DEF_RST(PropertyInfo(Variant::INT, "audio/video/video_delay_compensation_ms", PROPERTY_HINT_RANGE, "-1000,1000,1,suffix:ms"), 0);
}

void AudioServer::update() {
#ifdef DEBUG_ENABLED
	if (EngineDebugger::is_profiling(SNAME("servers"))) {
		// Driver time includes server time + effects times
		// Server time includes effects times
		uint64_t driver_time = AudioDriver::get_singleton()->get_profiling_time();
		uint64_t server_time = prof_time.get();

		// Subtract the server time from the driver time
		if (driver_time > server_time) {
			driver_time -= server_time;
		}

		Array values;

		for (int i = audio_buses.size() - 1; i >= 0; i--) {
			AuST::AudioBus *audio_bus = audio_buses[i];
			if (audio_bus->bypassed) {
				continue;
			}

			for (int j = 0; j < audio_bus->effects.size(); j++) {
				if (!audio_bus->effects[j]->enabled) {
					continue;
				}

				values.push_back(String(audio_bus->name) + audio_bus->effects[j]->effect->get_name());
				values.push_back(USEC_TO_SEC(audio_bus->effects[j]->prof_time));

				// Subtract the effect time from the driver and server times.
				if (driver_time > audio_bus->effects[j]->prof_time) {
					driver_time -= audio_bus->effects[j]->prof_time;
				}
				if (server_time > audio_bus->effects[j]->prof_time) {
					server_time -= audio_bus->effects[j]->prof_time;
				}
			}
		}

		values.push_back("audio_server");
		values.push_back(USEC_TO_SEC(server_time));
		values.push_back("audio_driver");
		values.push_back(USEC_TO_SEC(driver_time));

		values.push_front("audio_thread");
		EngineDebugger::profiler_add_frame_data("servers", values);
	}

	// Reset profiling times
	for (int i = audio_buses.size() - 1; i >= 0; i--) {
		AuST::AudioBus *audio_bus = audio_buses[i];
		if (audio_bus->bypassed) {
			continue;
		}

		for (int j = 0; j < audio_bus->effects.size(); j++) {
			if (!audio_bus->effects[j]->enabled) {
				continue;
			}

			audio_bus->effects.write[j]->prof_time = 0;
		}
	}

	AudioDriver::get_singleton()->reset_profiling_time();
	prof_time.set(0);
#endif

	// for (CallbackItem *ci : update_callback_list) {
	// 	ci->callback(ci->userdata);
	// }

	// _cleanup_lists();
}

// void AudioServer::_cleanup_lists() {
// 	mix_callback_list.maybe_cleanup();
// 	update_callback_list.maybe_cleanup();
// 	listener_changed_callback_list.maybe_cleanup();
// 	playback_list.maybe_cleanup();
// 	for (AudioStreamPlaybackBusDetails *bus_details : bus_details_graveyard_frame_old) {
// 		bus_details_graveyard_frame_old.erase(bus_details, [](AudioStreamPlaybackBusDetails *d) { delete d; });
// 	}
// 	for (AudioStreamPlaybackBusDetails *bus_details : bus_details_graveyard) {
// 		bus_details_graveyard_frame_old.insert(bus_details);
// 		bus_details_graveyard.erase(bus_details);
// 	}
// 	bus_details_graveyard.maybe_cleanup();
// 	bus_details_graveyard_frame_old.maybe_cleanup();
// }

void AudioServer::load_default_bus_layout() {
	String layout_path = GLOBAL_GET("audio/buses/default_bus_layout");

	if (ResourceLoader::exists(layout_path)) {
		Ref<AudioBusLayout> default_layout = ResourceLoader::load(layout_path);
		if (default_layout.is_valid()) {
			set_bus_layout(default_layout);
		}
	}
}

void AudioServer::finish() {
	for (int i = 0; i < AudioDriverManager::get_driver_count(); i++) {
		AudioDriverManager::get_driver(i)->finish();
	}

	// for (int i = 0; i < buses.size(); i++) {
	// 	memdelete(buses[i]);
	// }

	// buses.clear();

	// TODO: Clear lists and free RIDs?
}

/* MISC config */

void AudioServer::lock() {
	AudioDriver::get_singleton()->lock();
}

void AudioServer::unlock() {
	AudioDriver::get_singleton()->unlock();
}

AuSE::SpeakerMode AudioServer::get_speaker_mode() const {
	return (AuSE::SpeakerMode)AudioDriver::get_singleton()->get_speaker_mode();
}

float AudioServer::get_mix_rate() const {
	return AudioDriver::get_singleton()->get_mix_rate();
}

float AudioServer::get_input_mix_rate() const {
	return AudioDriver::get_singleton()->get_input_mix_rate();
}

float AudioServer::read_output_peak_db() const {
	return 0;
}

AudioServer *AudioServer::get_singleton() {
	return singleton;
}

double AudioServer::get_output_latency() const {
	return AudioDriver::get_singleton()->get_latency();
}

double AudioServer::get_time_to_next_mix() const {
	return AudioDriver::get_singleton()->get_time_to_next_mix();
}

double AudioServer::get_time_since_last_mix() const {
	return AudioDriver::get_singleton()->get_time_since_last_mix();
}

AudioServer *AudioServer::singleton = nullptr;

// void AudioServer::add_update_callback(AudioCallback p_callback, void *p_userdata) {
// 	CallbackItem *ci = new CallbackItem();
// 	ci->callback = p_callback;
// 	ci->userdata = p_userdata;
// 	update_callback_list.insert(ci);
// }

// void AudioServer::remove_update_callback(AudioCallback p_callback, void *p_userdata) {
// 	for (CallbackItem *ci : update_callback_list) {
// 		if (ci->callback == p_callback && ci->userdata == p_userdata) {
// 			update_callback_list.erase(ci, [](CallbackItem *c) { delete c; });
// 		}
// 	}
// }

// void AudioServer::add_mix_callback(AudioCallback p_callback, void *p_userdata) {
// 	CallbackItem *ci = new CallbackItem();
// 	ci->callback = p_callback;
// 	ci->userdata = p_userdata;
// 	mix_callback_list.insert(ci);
// }

// void AudioServer::remove_mix_callback(AudioCallback p_callback, void *p_userdata) {
// 	for (CallbackItem *ci : mix_callback_list) {
// 		if (ci->callback == p_callback && ci->userdata == p_userdata) {
// 			mix_callback_list.erase(ci, [](CallbackItem *c) { delete c; });
// 		}
// 	}
// }

// void AudioServer::add_listener_changed_callback(AudioCallback p_callback, void *p_userdata) {
// 	CallbackItem *ci = new CallbackItem();
// 	ci->callback = p_callback;
// 	ci->userdata = p_userdata;
// 	listener_changed_callback_list.insert(ci);
// }

// void AudioServer::remove_listener_changed_callback(AudioCallback p_callback, void *p_userdata) {
// 	for (CallbackItem *ci : listener_changed_callback_list) {
// 		if (ci->callback == p_callback && ci->userdata == p_userdata) {
// 			listener_changed_callback_list.erase(ci, [](CallbackItem *c) { delete c; });
// 		}
// 	}
// }

// TODO: Rewrite `audio_bus_layout.h`.
void AudioServer::set_bus_layout(const Ref<AudioBusLayout> &p_bus_layout) {
	ERR_FAIL_COND(p_bus_layout.is_null() || p_bus_layout->buses.is_empty());

	lock();
	for (int i = 0; i < buses.size(); i++) {
		memdelete(buses[i]);
	}
	buses.resize(p_bus_layout->buses.size());
	bus_map.clear();

	AudioDriver::get_singleton()->set_sample_bus_count(buses.size());

	for (int i = 0; i < p_bus_layout->buses.size(); i++) {
		Bus *bus = memnew(Bus);
		if (i == 0) {
			bus->name = SceneStringName(Master);
		} else {
			bus->name = p_bus_layout->buses[i].name;
			bus->send = p_bus_layout->buses[i].send;
			AudioDriver::get_singleton()->set_sample_bus_send(i, bus->send);
		}

		bus->solo = p_bus_layout->buses[i].solo;
		bus->mute = p_bus_layout->buses[i].mute;
		bus->bypass = p_bus_layout->buses[i].bypass;
		bus->volume_db = p_bus_layout->buses[i].volume_db;

		AudioDriver::get_singleton()->set_sample_bus_solo(i, bus->solo);
		AudioDriver::get_singleton()->set_sample_bus_mute(i, bus->mute);
		AudioDriver::get_singleton()->set_sample_bus_volume_db(i, bus->volume_db);

		for (int j = 0; j < p_bus_layout->buses[i].effects.size(); j++) {
			Ref<AudioEffect> fx = p_bus_layout->buses[i].effects[j].effect;

			if (fx.is_valid()) {
				Bus::Effect bfx;
				bfx.effect = fx;
				bfx.enabled = p_bus_layout->buses[i].effects[j].enabled;
#ifdef DEBUG_ENABLED
				bfx.prof_time = 0;
#endif
				bus->effects.push_back(bfx);
			}
		}

		bus_map[bus->name] = bus;
		buses.write[i] = bus;

		buses[i]->channels.resize(channel_count);
		for (int j = 0; j < channel_count; j++) {
			buses.write[i]->channels.write[j].buffer.resize(buffer_size);
		}
		_update_bus_effects(i);
	}
#ifdef TOOLS_ENABLED
	set_edited(false);
#endif
	unlock();

	// Samples bus sync.
}

Ref<AudioBusLayout> AudioServer::generate_bus_layout() const {
	Ref<AudioBusLayout> state;
	state.instantiate();

	state->buses.resize(buses.size());

	for (int i = 0; i < buses.size(); i++) {
		state->buses.write[i].name = buses[i]->name;
		state->buses.write[i].send = buses[i]->send;
		state->buses.write[i].mute = buses[i]->mute;
		state->buses.write[i].solo = buses[i]->solo;
		state->buses.write[i].bypass = buses[i]->bypass;
		state->buses.write[i].volume_db = buses[i]->volume_db;
		for (int j = 0; j < buses[i]->effects.size(); j++) {
			AudioBusLayout::Bus::Effect fx;
			fx.effect = buses[i]->effects[j].effect;
			fx.enabled = buses[i]->effects[j].enabled;
			state->buses.write[i].effects.push_back(fx);
		}
	}

	return state;
}

PackedStringArray AudioServer::get_output_device_list() {
	return AudioDriver::get_singleton()->get_output_device_list();
}

String AudioServer::get_output_device() {
	return AudioDriver::get_singleton()->get_output_device();
}

void AudioServer::set_output_device(const String &p_name) {
	AudioDriver::get_singleton()->set_output_device(p_name);
}

PackedStringArray AudioServer::get_input_device_list() {
	return AudioDriver::get_singleton()->get_input_device_list();
}

String AudioServer::get_input_device() {
	return AudioDriver::get_singleton()->get_input_device();
}

void AudioServer::set_input_device(const String &p_name) {
	AudioDriver::get_singleton()->set_input_device(p_name);
}

Error AudioServer::set_input_device_active(bool p_is_active) {
	if (input_device_active == p_is_active) {
		return OK;
	}
	if (p_is_active) {
		if (!GLOBAL_GET("audio/driver/enable_input")) {
			WARN_PRINT("You must enable the project setting \"audio/driver/enable_input\" to use audio capture.");
			return FAILED;
		}

		input_buffer_ofs = 0;
		input_device_active = true;
		return AudioDriver::get_singleton()->input_start();
	} else {
		input_device_active = false;
		return AudioDriver::get_singleton()->input_stop();
	}
}

int AudioServer::get_input_frames_available() {
	AudioDriver *ad = AudioDriver::get_singleton();
	ad->lock();
	int64_t input_position = ad->get_input_position();
	if (input_position < input_buffer_ofs) {
		input_position += ad->get_input_buffer().size();
	}
	ad->unlock();
	return (int)((input_position - input_buffer_ofs) / 2); // Buffer is stereo.
}

int AudioServer::get_input_buffer_length_frames() {
	AudioDriver *ad = AudioDriver::get_singleton();
	ad->lock();
	int buffsize = ad->get_input_buffer().size();
	ad->unlock();
	return buffsize / 2;
}

PackedVector2Array AudioServer::get_input_frames(int p_frames) {
	PackedVector2Array ret;
	AudioDriver *ad = AudioDriver::get_singleton();
	ad->lock();
	int input_position = ad->get_input_position();
	Vector<int32_t> buf = ad->get_input_buffer();
	if (input_position < input_buffer_ofs) {
		input_position += buf.size();
	}
	if ((input_buffer_ofs + p_frames * 2 <= input_position) && (p_frames >= 0)) {
		ret.resize(p_frames);
		for (int i = 0; i < p_frames; i++) {
			float l = (buf[input_buffer_ofs++] >> 16) / 32768.f;
			if (input_buffer_ofs >= buf.size()) {
				input_buffer_ofs = 0;
			}
			float r = (buf[input_buffer_ofs++] >> 16) / 32768.f;
			if (input_buffer_ofs >= buf.size()) {
				input_buffer_ofs = 0;
			}
			ret.write[i] = Vector2(l, r);
		}
	}
	ad->unlock();
	return ret;
}

void AudioServer::set_enable_tagging_used_audio_streams(bool p_enable) {
	tag_used_audio_streams = p_enable;
}

#ifdef TOOLS_ENABLED
void AudioServer::get_argument_options(const StringName &p_function, int p_idx, List<String> *r_options) const {
	const String pf = p_function;
	if ((p_idx == 0 && pf == "get_bus_index") || (p_idx == 1 && pf == "set_bus_send")) {
		for (const AuST::AudioBus *E : audio_buses) {
			r_options->push_back(String(E->name).quote());
		}
	}

	Object::get_argument_options(p_function, p_idx, r_options);
}
#endif

AuSE::PlaybackType AudioServer::get_default_playback_type() const {
	int playback_type = GLOBAL_GET_CACHED(int, "audio/general/default_playback_type");
	ERR_FAIL_COND_V_MSG(
			playback_type < 0 || playback_type >= AuSE::PlaybackType::PLAYBACK_TYPE_MAX,
			AuSE::PlaybackType::PLAYBACK_TYPE_STREAM,
			vformat(R"(Project settings value (%s) for "audio/general/default_playback_type" is not supported)", playback_type));

	switch (playback_type) {
		case 1: {
			return AuSE::PlaybackType::PLAYBACK_TYPE_SAMPLE;
		} break;

		case 0:
		default: {
			return AuSE::PlaybackType::PLAYBACK_TYPE_STREAM;
		} break;
	}
}

bool AudioServer::is_stream_registered_as_sample(const Ref<AudioStream> &p_stream) {
	ERR_FAIL_COND_V_MSG(p_stream.is_null(), false, "Parameter p_stream is null.");
	return AudioDriver::get_singleton()->is_stream_registered_as_sample(p_stream);
}

void AudioServer::register_stream_as_sample(const Ref<AudioStream> &p_stream) {
	ERR_FAIL_COND_MSG(p_stream.is_null(), "Parameter p_stream is null.");
	ERR_FAIL_COND_MSG(!(p_stream->can_be_sampled()), "Parameter p_stream cannot be sampled.");
	Ref<AudioSample> sample = p_stream->generate_sample();
	register_sample(sample);
}

void AudioServer::unregister_stream_as_sample(const Ref<AudioStream> &p_stream) {
	ERR_FAIL_COND_MSG(p_stream.is_null(), "Parameter p_stream is null.");
	ERR_FAIL_COND_MSG(!(p_stream->can_be_sampled()), "Parameter p_stream cannot be sampled.");
	Ref<AudioSample> sample = p_stream->generate_sample();
	unregister_sample(sample);
}

void AudioServer::register_sample(const Ref<AudioSample> &p_sample) {
	ERR_FAIL_COND_MSG(p_sample.is_null(), "Parameter p_sample is null.");
	ERR_FAIL_COND_MSG(p_sample->stream.is_null(), "Parameter p_sample->stream is null.");
	ERR_FAIL_COND_MSG(!(p_sample->stream->can_be_sampled()), "Parameter p_stream cannot be sampled.");
	AudioDriver::get_singleton()->register_sample(p_sample);
}

void AudioServer::unregister_sample(const Ref<AudioSample> &p_sample) {
	ERR_FAIL_COND_MSG(p_sample.is_null(), "Parameter p_sample is null.");
	ERR_FAIL_COND_MSG(p_sample->stream.is_null(), "Parameter p_sample->stream is null.");
	AudioDriver::get_singleton()->unregister_sample(p_sample);
}

void AudioServer::start_sample_playback(const Ref<AudioSamplePlayback> &p_playback) {
	ERR_FAIL_COND_MSG(p_playback.is_null(), "Parameter p_playback is null.");
	AudioDriver::get_singleton()->start_sample_playback(p_playback);
	sample_playback_list.ordered_insert(p_playback);
}

void AudioServer::stop_sample_playback(const Ref<AudioSamplePlayback> &p_playback) {
	ERR_FAIL_COND_MSG(p_playback.is_null(), "Parameter p_playback is null.");
	if (!sample_playback_list.has(p_playback)) {
		return;
	}
	sample_playback_list.erase(p_playback);
	AudioDriver::get_singleton()->stop_sample_playback(p_playback);
	p_playback->stream_playback->set_sample_playback(nullptr);
	stop_playback_stream(p_playback->stream_playback);
}

void AudioServer::set_sample_playback_pause(const Ref<AudioSamplePlayback> &p_playback, bool p_paused) {
	ERR_FAIL_COND_MSG(p_playback.is_null(), "Parameter p_playback is null.");
	AudioDriver::get_singleton()->set_sample_playback_pause(p_playback, p_paused);
}

bool AudioServer::is_sample_playback_active(const Ref<AudioSamplePlayback> &p_playback) {
	ERR_FAIL_COND_V_MSG(p_playback.is_null(), false, "Parameter p_playback is null.");
	return sample_playback_list.has(p_playback);
}

double AudioServer::get_sample_playback_position(const Ref<AudioSamplePlayback> &p_playback) {
	ERR_FAIL_COND_V_MSG(p_playback.is_null(), false, "Parameter p_playback is null.");
	return AudioDriver::get_singleton()->get_sample_playback_position(p_playback);
}

void AudioServer::update_sample_playback_pitch_scale(const Ref<AudioSamplePlayback> &p_playback, float p_pitch_scale) {
	ERR_FAIL_COND_MSG(p_playback.is_null(), "Parameter p_playback is null.");
	return AudioDriver::get_singleton()->update_sample_playback_pitch_scale(p_playback, p_pitch_scale);
}

// TODO: Bind new methods.
void AudioServer::_bind_methods() {
	// ClassDB::bind_method(D_METHOD("set_bus_count", "amount"), &AudioServer::set_bus_count); // TODO: Restore and deprecate.
	ClassDB::bind_method(D_METHOD("get_bus_count"), &AudioServer::get_bus_count);

	ClassDB::bind_method(D_METHOD("remove_bus", "index"), &AudioServer::remove_bus);
	ClassDB::bind_method(D_METHOD("add_bus", "at_position"), &AudioServer::add_bus, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("move_bus", "index", "to_index"), &AudioServer::move_bus);

	ClassDB::bind_method(D_METHOD("set_bus_name", "bus_idx", "name"), &AudioServer::set_bus_name);
	ClassDB::bind_method(D_METHOD("get_bus_name", "bus_idx"), &AudioServer::get_bus_name);
	ClassDB::bind_method(D_METHOD("get_bus_index", "bus_name"), &AudioServer::get_bus_index);

	ClassDB::bind_method(D_METHOD("get_bus_channels", "bus_idx"), &AudioServer::get_bus_channels);

	ClassDB::bind_method(D_METHOD("set_bus_volume_db", "bus_idx", "volume_db"), &AudioServer::set_bus_volume_db);
	ClassDB::bind_method(D_METHOD("get_bus_volume_db", "bus_idx"), &AudioServer::get_bus_volume_db);

	ClassDB::bind_method(D_METHOD("set_bus_volume_linear", "bus_idx", "volume_linear"), &AudioServer::set_bus_volume_linear);
	ClassDB::bind_method(D_METHOD("get_bus_volume_linear", "bus_idx"), &AudioServer::get_bus_volume_linear);

	ClassDB::bind_method(D_METHOD("set_bus_send", "bus_idx", "send"), &AudioServer::set_bus_send);
	ClassDB::bind_method(D_METHOD("get_bus_send", "bus_idx"), &AudioServer::get_bus_send);

	ClassDB::bind_method(D_METHOD("set_bus_solo", "bus_idx", "enable"), &AudioServer::set_bus_solo);
	ClassDB::bind_method(D_METHOD("is_bus_solo", "bus_idx"), &AudioServer::is_bus_solo);

	ClassDB::bind_method(D_METHOD("set_bus_mute", "bus_idx", "enable"), &AudioServer::set_bus_mute);
	ClassDB::bind_method(D_METHOD("is_bus_mute", "bus_idx"), &AudioServer::is_bus_mute);

	ClassDB::bind_method(D_METHOD("set_bus_bypass_effects", "bus_idx", "enable"), &AudioServer::set_bus_bypass_effects);
	ClassDB::bind_method(D_METHOD("is_bus_bypassing_effects", "bus_idx"), &AudioServer::is_bus_bypassing_effects);

	ClassDB::bind_method(D_METHOD("add_bus_effect", "bus_idx", "effect", "at_position"), &AudioServer::add_bus_effect, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("remove_bus_effect", "bus_idx", "effect_idx"), &AudioServer::remove_bus_effect);

	ClassDB::bind_method(D_METHOD("get_bus_effect_count", "bus_idx"), &AudioServer::get_bus_effect_count);
	ClassDB::bind_method(D_METHOD("get_bus_effect", "bus_idx", "effect_idx"), &AudioServer::get_bus_effect);
	ClassDB::bind_method(D_METHOD("get_bus_effect_instance", "bus_idx", "effect_idx", "channel"), &AudioServer::get_bus_effect_instance, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("swap_bus_effects", "bus_idx", "effect_idx", "by_effect_idx"), &AudioServer::swap_bus_effects);

	ClassDB::bind_method(D_METHOD("set_bus_effect_enabled", "bus_idx", "effect_idx", "enabled"), &AudioServer::set_bus_effect_enabled);
	ClassDB::bind_method(D_METHOD("is_bus_effect_enabled", "bus_idx", "effect_idx"), &AudioServer::is_bus_effect_enabled);

	ClassDB::bind_method(D_METHOD("get_bus_peak_volume_left_db", "bus_idx", "channel"), &AudioServer::get_bus_peak_volume_left_db);
	ClassDB::bind_method(D_METHOD("get_bus_peak_volume_right_db", "bus_idx", "channel"), &AudioServer::get_bus_peak_volume_right_db);

	ClassDB::bind_method(D_METHOD("set_playback_speed_scale", "scale"), &AudioServer::set_playback_speed_scale);
	ClassDB::bind_method(D_METHOD("get_playback_speed_scale"), &AudioServer::get_playback_speed_scale);

	ClassDB::bind_method(D_METHOD("lock"), &AudioServer::lock);
	ClassDB::bind_method(D_METHOD("unlock"), &AudioServer::unlock);

	ClassDB::bind_method(D_METHOD("get_speaker_mode"), &AudioServer::get_speaker_mode);
	ClassDB::bind_method(D_METHOD("get_mix_rate"), &AudioServer::get_mix_rate);
	ClassDB::bind_method(D_METHOD("get_input_mix_rate"), &AudioServer::get_input_mix_rate);

	ClassDB::bind_method(D_METHOD("get_driver_name"), &AudioServer::get_driver_name);

	ClassDB::bind_method(D_METHOD("get_output_device_list"), &AudioServer::get_output_device_list);
	ClassDB::bind_method(D_METHOD("get_output_device"), &AudioServer::get_output_device);
	ClassDB::bind_method(D_METHOD("set_output_device", "name"), &AudioServer::set_output_device);

	ClassDB::bind_method(D_METHOD("get_time_to_next_mix"), &AudioServer::get_time_to_next_mix);
	ClassDB::bind_method(D_METHOD("get_time_since_last_mix"), &AudioServer::get_time_since_last_mix);
	ClassDB::bind_method(D_METHOD("get_output_latency"), &AudioServer::get_output_latency);

	ClassDB::bind_method(D_METHOD("get_input_device_list"), &AudioServer::get_input_device_list);
	ClassDB::bind_method(D_METHOD("get_input_device"), &AudioServer::get_input_device);
	ClassDB::bind_method(D_METHOD("set_input_device", "name"), &AudioServer::set_input_device);
	ClassDB::bind_method(D_METHOD("set_input_device_active", "active"), &AudioServer::set_input_device_active);
	ClassDB::bind_method(D_METHOD("get_input_frames_available"), &AudioServer::get_input_frames_available);
	ClassDB::bind_method(D_METHOD("get_input_buffer_length_frames"), &AudioServer::get_input_buffer_length_frames);
	ClassDB::bind_method(D_METHOD("get_input_frames", "frames"), &AudioServer::get_input_frames);

	ClassDB::bind_method(D_METHOD("set_bus_layout", "bus_layout"), &AudioServer::set_bus_layout);
	ClassDB::bind_method(D_METHOD("generate_bus_layout"), &AudioServer::generate_bus_layout);

	ClassDB::bind_method(D_METHOD("set_enable_tagging_used_audio_streams", "enable"), &AudioServer::set_enable_tagging_used_audio_streams);

	ClassDB::bind_method(D_METHOD("is_stream_registered_as_sample", "stream"), &AudioServer::is_stream_registered_as_sample);
	ClassDB::bind_method(D_METHOD("register_stream_as_sample", "stream"), &AudioServer::register_stream_as_sample);

	// ADD_PROPERTY(PropertyInfo(Variant::INT, "bus_count"), "set_bus_count", "get_bus_count");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "output_device"), "set_output_device", "get_output_device");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "input_device"), "set_input_device", "get_input_device");
	// The default value may be set to an empty string by the platform-specific audio driver.
	// Override for class reference generation purposes.
	ADD_PROPERTY_DEFAULT("input_device", "Default");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "playback_speed_scale"), "set_playback_speed_scale", "get_playback_speed_scale");

	ADD_SIGNAL(MethodInfo("bus_layout_changed"));
	ADD_SIGNAL(MethodInfo("bus_renamed", PropertyInfo(Variant::INT, "bus_index"), PropertyInfo(Variant::STRING_NAME, "old_name"), PropertyInfo(Variant::STRING_NAME, "new_name")));

	BIND_ENUM_CONSTANT(AuSE::SPEAKER_MODE_STEREO);
	BIND_ENUM_CONSTANT(AuSE::SPEAKER_SURROUND_31);
	BIND_ENUM_CONSTANT(AuSE::SPEAKER_SURROUND_51);
	BIND_ENUM_CONSTANT(AuSE::SPEAKER_SURROUND_71);

	BIND_ENUM_CONSTANT(AuSE::PLAYBACK_TYPE_DEFAULT);
	BIND_ENUM_CONSTANT(AuSE::PLAYBACK_TYPE_STREAM);
	BIND_ENUM_CONSTANT(AuSE::PLAYBACK_TYPE_SAMPLE);
	BIND_ENUM_CONSTANT(AuSE::PLAYBACK_TYPE_MAX);

	// TODO: Bind new enums.
}

AudioServer::AudioServer() {
	singleton = this;
}

AudioServer::~AudioServer() {
	// Cleanup resources while we still have an active AudioServer singleton,
	// for resources that depend on the singleton still existing.
	// _cleanup_lists();

	singleton = nullptr;
}
