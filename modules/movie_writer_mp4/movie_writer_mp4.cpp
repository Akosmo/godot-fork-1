/**************************************************************************/
/*  movie_writer_mp4.cpp                                                  */
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

#include "movie_writer_mp4.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/os/os.h"

uint32_t MovieWriterMP4::get_audio_mix_rate() const {
	return mix_rate;
}

AuSE::SpeakerMode MovieWriterMP4::get_audio_speaker_mode() const {
	return speaker_mode;
}

void MovieWriterMP4::get_supported_extensions(List<String> *r_extensions) const {
	r_extensions->push_back("mp4");
}

bool MovieWriterMP4::handles_file(const String &p_path) const {
	return p_path.has_extension("mp4");
}

Error MovieWriterMP4::write_begin(const Size2i &p_movie_size, uint32_t p_fps, const String &p_base_path) {
	// WAV code from `movie_writer_pngwav.cpp`.

	if (binary_path.is_empty()) {
		binary_path = "ffmpeg";
	} else {
		if (binary_path.is_relative_path()) {
			binary_path = ProjectSettings::get_singleton()->globalize_path(binary_path);
		}
		ERR_FAIL_COND_V_MSG(!FileAccess::exists(binary_path), ERR_FILE_NOT_FOUND, "Invalid binary path.");
	}

	fps = p_fps;

	base_path = p_base_path;
	if (base_path.get_basename().is_relative_path()) {
		base_path = ProjectSettings::get_singleton()->globalize_path(base_path);
	}
	ERR_FAIL_COND_V_MSG(!DirAccess::dir_exists_absolute(base_path.get_base_dir()), ERR_DOES_NOT_EXIST, "Invalid save path.");

	// temp_path = base_path.get_basename() + "_temp." + base_path.get_extension();

	String resolution_str = vformat(U"%sx%s", p_movie_size.width, p_movie_size.height);
	String fps_str = vformat(U"%d", fps);

	List<String> ffmpeg_args = {
		"-hide_banner",
		"-y",
		"-f", "rawvideo",
		"-pix_fmt", "rgb24",
		"-s", resolution_str,
		"-r", fps_str,
		// "-vcodec", "rawvideo",
		"-i", "pipe:0",
		"-an",
		"-preset", "slower",
		"-crf", "17",
		"-pix_fmt", "yuv420p",
		"-r", fps_str,
		// "-movflags", "+faststart",
		base_path
	};

	Dictionary pipe_dict = OS::get_singleton()->execute_with_pipe(binary_path, ffmpeg_args);
	ERR_FAIL_COND_V_MSG(pipe_dict.size() == 0, ERR_CANT_CREATE, "Could not execute FFmpeg.");
	pipe_stdio = pipe_dict["stdio"];
	pipe_stderr = pipe_dict["stderr"];
	pipe_pid = pipe_dict["pid"];

	wav_file = FileAccess::open(base_path.get_basename() + ".wav", FileAccess::WRITE_READ);
	ERR_FAIL_COND_V(wav_file.is_null(), ERR_CANT_OPEN);

	wav_file->store_buffer((const uint8_t *)"RIFF", 4);
	wav_file->store_32(36);
	wav_file->store_buffer((const uint8_t *)"WAVE", 4);
	wav_file->store_buffer((const uint8_t *)"fmt ", 4);

	uint32_t channels = 2;
	switch (speaker_mode) {
		case AuSE::SPEAKER_MODE_STEREO:
			channels = 2;
			break;
		case AuSE::SPEAKER_SURROUND_31:
			channels = 4;
			break;
		case AuSE::SPEAKER_SURROUND_51:
			channels = 6;
			break;
		case AuSE::SPEAKER_SURROUND_71:
			channels = 8;
			break;
	}

	wav_file->store_32(16);
	wav_file->store_16(1);
	wav_file->store_16(channels);
	wav_file->store_32(mix_rate);

	int blockalign = 32 / 8 * channels;
	audio_block_size = mix_rate / fps * blockalign;

	wav_file->store_32(mix_rate * blockalign);
	wav_file->store_16(blockalign);
	wav_file->store_16(32);
	wav_file->store_buffer((const uint8_t *)"data", 4);
	wav_file->store_32(0);

	wav_data_size_pos = wav_file->get_position();

	return OK;
}

Error MovieWriterMP4::write_frame(const Ref<Image> &p_image, const int32_t *p_audio_data) {
	if (unlikely(!OS::get_singleton()->is_process_running(pipe_pid) || pipe_stdio->get_error() != OK)) {
		ERR_PRINT_ONCE(pipe_stderr->get_as_text());
		ERR_FAIL_V_MSG(ERR_FILE_CANT_WRITE, "Failed to write to FFmpeg.");
	}
	ERR_FAIL_COND_V_MSG(!DirAccess::dir_exists_absolute(base_path.get_base_dir()), ERR_DOES_NOT_EXIST, "Invalid save path.");

	const PackedByteArray &img_data = p_image->get_data();
	if (img_data.size() != p_image->get_width() * p_image->get_height() * 3) {
		ERR_FAIL_V_MSG(FAILED, "Invalid data size.");
	}

	if (!pipe_stdio->store_buffer(img_data)) {
		ERR_PRINT_ONCE(pipe_stderr->get_as_text());
		ERR_FAIL_V_MSG(ERR_FILE_CANT_WRITE, "Failed to store buffer.");
	}

	// audio_data.push_back((int32_t *)p_audio_data);

	wav_file->store_buffer((const uint8_t *)p_audio_data, audio_block_size);

	frame_count++;

	return OK;
}

void MovieWriterMP4::write_end() {
	if (pipe_stdio.is_valid() && pipe_stdio->is_open()) {
		pipe_stdio->close();
	}

	// List<String> ffmpeg_args = {
	// 	"-hide_banner",
	// 	"-y",
	//     "-i", temp_path,
	//     "-c:v" "copy"
	//     "-map", "0:v:0",
	//     "-map", "1:a:0",
	//     "-f", "u8",
	//     "-c:a", "aac",
	//     "-b:a", "256k",
	//     "-ar", "44100",
	//     "-channel_layout", "stereo",
	//     "-i", "pipe:0",
	//     "-movflags", "+faststart",
	// 	base_path
	// };

	if (wav_file.is_valid() && wav_file->is_open()) {
		uint32_t datasize = wav_file->get_position() - wav_data_size_pos;
		wav_file->seek(4);
		wav_file->store_32(36 + datasize);
		wav_file->seek(0x28);
		wav_file->store_32(datasize);

		wav_file->close();
	}

	// List<String> ffmpeg_args = {
	// 	// "-hide_banner",
	// 	"-y",
	//     "-i", temp_path,
	//     "-i", base_path.get_basename() + ".wav",
	//     "-c:a", "aac",
	//     "-b:a", "256k",
	//     "-ar", "44100",
	//     "-channel_layout", "stereo",
	//     "-map", "0:v:0",
	//     "-map", "1:a:0",
	//     "-movflags", "+faststart",
	// 	base_path,
	//     "-c:v", "copy",
	// };

	// Dictionary pipe_dict = OS::get_singleton()->execute_with_pipe(binary_path, ffmpeg_args);
	// ERR_PRINT(vformat("Pipe dict size: %d", pipe_dict.size()));
	// ERR_FAIL_COND_MSG(pipe_dict.size() == 0, "Could not execute FFmpeg.");
	// pipe_stdio_2 = pipe_dict["stdio"];
	// pipe_stderr_2 = pipe_dict["stderr"];
	// pipe_pid_2 = pipe_dict["pid"];

	// if (unlikely(!OS::get_singleton()->is_process_running(pipe_pid_2) || pipe_stdio_2->get_error() != OK)) {
	// 	ERR_PRINT(pipe_stderr_2->get_as_text());
	// 	ERR_FAIL_MSG("Failed to write to FFmpeg.");
	// }

	// ProcessID last_pid = 0;
	// if (OS::get_singleton()->create_process(binary_path, ffmpeg_args, &last_pid, true)) {
	//     ERR_FAIL_MSG("Could not execute FFmpeg.");
	// }
	// if (OS::get_singleton()->execute(binary_path, ffmpeg_args, nullptr, nullptr, false, nullptr, true)) {
	//     ERR_FAIL_MSG("Could not execute FFmpeg.");
	// }

	// for (uint32_t i = 0; i < audio_data.size(); i++) {
	//     if (!pipe_stdio->store_32((uint32_t)audio_data[i])) {
	//         ERR_PRINT_ONCE(pipe_stderr->get_as_text());
	//         ERR_FAIL_MSG("Failed to store 32-bit of audio data.");
	//     }
	// }

	// if (!pipe_stdio->store_buffer((const uint8_t *)audio_data.ptr(), audio_data.size())) {
	//     ERR_PRINT_ONCE(pipe_stderr->get_as_text());
	//     ERR_FAIL_MSG("Failed to store audio data.");
	// }

	// if (pipe_stdio_2.is_valid() && pipe_stdio_2->is_open()) {
	//     ERR_PRINT("Closing pipe: " + pipe_stderr_2->get_as_text());
	// 	pipe_stdio_2->close();
	// }

	// if (!OS::get_singleton()->kill(last_pid)) {
	//     Error err_1 = DirAccess::remove_absolute(temp_path);
	//     if (err_1) {
	//         ERR_PRINT(vformat("Failed to remove temporary file: " + temp_path + " - Code: %d", err_1));
	//         Error err_2 = OS::get_singleton()->move_to_trash(temp_path);
	//         if (err_2) {
	//             ERR_PRINT(vformat("Failed to trash temporary file: " + temp_path + " - Code: %d", err_2));
	//         }
	//     }
	// } else {
	//     ERR_PRINT("Could not kill process.");
	// }
}

MovieWriterMP4::MovieWriterMP4() {
	binary_path = GLOBAL_GET("editor/movie_writer/mp4/binary_path");
	mix_rate = GLOBAL_GET("editor/movie_writer/mix_rate");
	speaker_mode = AuSE::SpeakerMode(int(GLOBAL_GET("editor/movie_writer/speaker_mode")));
}
