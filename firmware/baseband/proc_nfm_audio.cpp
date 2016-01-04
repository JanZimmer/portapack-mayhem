/*
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc.
 *
 * This file is part of PortaPack.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#include "proc_nfm_audio.hpp"

#include <cstdint>
#include <cstddef>

void NarrowbandFMAudio::execute(const buffer_c8_t& buffer) {
	if( !configured ) {
		return;
	}

	std::array<complex16_t, 512> dst;
	const buffer_c16_t dst_buffer {
		dst.data(),
		dst.size()
	};

	const auto decim_0_out = decim_0.execute(buffer, dst_buffer);
	const auto decim_1_out = decim_1.execute(decim_0_out, dst_buffer);
	const auto channel_out = channel_filter.execute(decim_1_out, dst_buffer);

	feed_channel_stats(channel_out);
	channel_spectrum.feed(channel_out, channel_filter_pass_f, channel_filter_stop_f);

	const buffer_s16_t work_audio_buffer {
		(int16_t*)dst.data(),
		sizeof(*dst.data()) * dst.size()
	};

	auto audio = demod.execute(channel_out, work_audio_buffer);

	static uint64_t audio_present_history = 0;
	const auto audio_present_now = squelch.execute(audio);
	audio_present_history = (audio_present_history << 1) | (audio_present_now ? 1 : 0);
	const bool audio_present = (audio_present_history != 0);

	audio_hpf.execute_in_place(audio);
	audio_deemph.execute_in_place(audio);

	if( !audio_present ) {
		// Zero audio buffer.
		for(size_t i=0; i<audio.count; i++) {
			audio.p[i] = 0;
		}
	}

	fill_audio_buffer(audio);
}

void NarrowbandFMAudio::on_message(const Message* const message) {
	switch(message->id) {
	case Message::ID::UpdateSpectrum:
		channel_spectrum.update();
		break;

	case Message::ID::NBFMConfigure:
		configure(*reinterpret_cast<const NBFMConfigureMessage*>(message));
		break;

	default:
		break;
	}
}

void NarrowbandFMAudio::configure(const NBFMConfigureMessage& message) {
	constexpr size_t baseband_fs = 3072000;

	constexpr size_t decim_0_input_fs = baseband_fs;
	constexpr size_t decim_0_decimation_factor = 8;
	constexpr size_t decim_0_output_fs = decim_0_input_fs / decim_0_decimation_factor;

	constexpr size_t decim_1_input_fs = decim_0_output_fs;
	constexpr size_t decim_1_decimation_factor = 8;
	constexpr size_t decim_1_output_fs = decim_1_input_fs / decim_1_decimation_factor;

	constexpr size_t channel_filter_input_fs = decim_1_output_fs;
	constexpr size_t channel_filter_decimation_factor = 1;
	constexpr size_t channel_filter_output_fs = channel_filter_input_fs / channel_filter_decimation_factor;

	constexpr size_t demod_input_fs = channel_filter_output_fs;

	decim_0.configure(message.decim_0_filter.taps, 33554432);
	decim_1.configure(message.decim_1_filter.taps, 131072);
	channel_filter.configure(message.channel_filter.taps, channel_filter_decimation_factor);
	demod.configure(demod_input_fs, message.deviation);
	channel_filter_pass_f = message.channel_filter.pass_frequency_normalized * channel_filter_input_fs;
	channel_filter_stop_f = message.channel_filter.stop_frequency_normalized * channel_filter_input_fs;
	channel_spectrum.set_decimation_factor(std::floor((channel_filter_output_fs / 2) / ((channel_filter_pass_f + channel_filter_stop_f) / 2)));
	squelch.set_threshold(6144);

	configured = true;
}
