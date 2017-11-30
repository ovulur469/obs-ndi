/*
obs-ndi (NDI I/O in OBS Studio)
Copyright (C) 2016-2017 Stéphane Lepin <stephane.lepin@gmail.com>

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/profiler.h>
#include <media-io/video-frame.h>

#include "obs-ndi.h"

static FORCE_INLINE uint32_t min_uint32(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

struct ndi_output {
    obs_output_t *output;
    const char* ndi_name;
    bool async_sending;
    obs_video_info video_info;
    obs_audio_info audio_info;

    bool started;
    NDIlib_FourCC_type_e frame_format;
    NDIlib_send_instance_t ndi_sender;

    uint8_t* conv_buffer;
    uint32_t conv_linesize;

    gs_texrender_t* texrender;
    gs_stagesurf_t* stagesurface;

    uint32_t videoWidth;
    uint32_t videoHeight;
    uint8_t* videoData;
    uint32_t videoLinesize;

    video_t* video_output;

};

const char* ndi_output_getname(void* data) {
    UNUSED_PARAMETER(data);
    return obs_module_text("NDIPlugin.OutputName");
}

obs_properties_t* ndi_output_getproperties(void* data) {
    UNUSED_PARAMETER(data);

    obs_properties_t* props = obs_properties_create();
    obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

    obs_properties_add_text(props, "ndi_name",
        obs_module_text("NDIPlugin.OutputProps.NDIName"), OBS_TEXT_DEFAULT);

    obs_properties_add_bool(props, "ndi_async_sending", "");

    return props;
}

void ndi_output_rawvideo(void* data, struct video_data* frame);

void ndi_output_mainrender(void* data, uint32_t cx, uint32_t cy) {
    struct ndi_output* o = static_cast<ndi_output*>(data);
    obs_get_video_info(&o->video_info);

    gs_texrender_end(o->texrender);

    if (o->videoWidth != cx || o->videoHeight != cy) {
        gs_stagesurface_unmap(o->stagesurface);
        gs_stagesurface_destroy(o->stagesurface);

        o->stagesurface =
            gs_stagesurface_create(cx, cy, GS_BGRA);
        gs_stagesurface_map(o->stagesurface,
            &o->videoData, &o->videoLinesize);

        video_output_info vi;
        vi.format = VIDEO_FORMAT_BGRA;
        vi.width = cx;
        vi.height = cy;
        vi.fps_den = o->video_info.fps_den;
        vi.fps_num = o->video_info.fps_num;
        vi.cache_size = 1;
        vi.colorspace = VIDEO_CS_DEFAULT;
        vi.range = VIDEO_RANGE_DEFAULT;
        vi.name = obs_output_get_name(o->output);

        video_output_close(o->video_output);
        video_output_open(&o->video_output, &vi);
        video_output_connect(o->video_output,
            nullptr, ndi_output_rawvideo, o);

        o->videoWidth = cx;
        o->videoHeight = cy;
    }

    struct video_frame output_frame;
    if (video_output_lock_frame(o->video_output,
        &output_frame, 1, os_gettime_ns()))
    {
        gs_stage_texture(o->stagesurface,
            gs_texrender_get_texture(o->texrender));

        memcpy(output_frame.data[0], o->videoData,
            o->videoLinesize * o->videoHeight);
        output_frame.linesize[0] = o->videoLinesize;

        video_output_unlock_frame(o->video_output);
    }

    gs_blend_state_pop();

    /* --- BREAK --- */

    gs_texrender_reset(o->texrender);

    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

    gs_texrender_begin(o->texrender, cx, cy);

    struct vec4 background;
    vec4_zero(&background);

    gs_clear(GS_CLEAR_COLOR, &background, 0.0f, 0);
    gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
}

bool ndi_output_start(void* data) {
    struct ndi_output* o = static_cast<ndi_output*>(data);

    ndiLib->NDIlib_send_destroy(o->ndi_sender);
    delete o->conv_buffer;

    obs_get_audio_info(&o->audio_info);

    switch (o->video_info.output_format) {
        case VIDEO_FORMAT_NV12:
        case VIDEO_FORMAT_I420:
        case VIDEO_FORMAT_I444:
            o->frame_format = NDIlib_FourCC_type_UYVY;
            o->conv_linesize = o->video_info.output_width * 2;
            o->conv_buffer =
                new uint8_t[o->video_info.output_height * o->conv_linesize * 2]();
            break;

        case VIDEO_FORMAT_RGBA:
            // There won't be transparency in the output data, so
            // ignore the alpha channel
            o->frame_format = NDIlib_FourCC_type_RGBX;
            break;

        case VIDEO_FORMAT_BGRA:
            o->frame_format = NDIlib_FourCC_type_BGRA;
            break;

        case VIDEO_FORMAT_BGRX:
            o->frame_format = NDIlib_FourCC_type_BGRX;
            break;
    }


    NDIlib_send_create_t send_desc;
    send_desc.p_ndi_name = o->ndi_name;
    send_desc.p_groups = NULL;
    send_desc.clock_video = false;
    send_desc.clock_audio = false;

    o->ndi_sender = ndiLib->NDIlib_send_create(&send_desc);

    if (o->ndi_sender) {
        o->started = true;
        //obs_output_begin_data_capture(o->output, 0);

        o->frame_format = NDIlib_FourCC_type_BGRA;
        obs_add_main_render_callback(ndi_output_mainrender, o);

        if (o->async_sending) {
            blog(LOG_INFO, "asynchronous video sending enabled");
        } else {
            blog(LOG_INFO, "asynchronous video sending disabled");
        }
    } else {
        o->started = false;
    }

    return o->started;
}

void ndi_output_stop(void* data, uint64_t ts) {
    struct ndi_output* o = static_cast<ndi_output*>(data);
    o->started = false;

    //obs_output_end_data_capture(o->output);
    obs_remove_main_render_callback(ndi_output_mainrender, o);

    ndiLib->NDIlib_send_destroy(o->ndi_sender);
    delete o->conv_buffer;
}

void ndi_output_update(void* data, obs_data_t* settings) {
    struct ndi_output* o = static_cast<ndi_output*>(data);
    o->ndi_name = obs_data_get_string(settings, "ndi_name");
    o->async_sending = obs_data_get_bool(settings, "ndi_async_sending");
}

void* ndi_output_create(obs_data_t* settings, obs_output_t* output) {
    UNUSED_PARAMETER(settings);

    struct ndi_output* o =
        static_cast<ndi_output*>(bzalloc(sizeof(struct ndi_output)));
    o->output = output;
    o->started = false;

    o->videoWidth = 0;
    o->videoHeight = 0;
    o->texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);

    ndi_output_update(o, settings);

    return o;
}

void ndi_output_destroy(void* data) {
    struct ndi_output* o = static_cast<ndi_output*>(data);
    ndiLib->NDIlib_send_destroy(o->ndi_sender);
    delete o->conv_buffer;
}

void convert_nv12_to_uyvy(uint8_t* input[], uint32_t in_linesize[],
    uint32_t start_y, uint32_t end_y,
    uint8_t* output, uint32_t out_linesize)
{
    uint8_t* _Y;
    uint8_t* _U;
    uint8_t* _V;
    uint8_t* _out;
    uint32_t width = min_uint32(in_linesize[0], out_linesize);
    for (uint32_t y = start_y; y < end_y; y++) {
        _Y = input[0] + (y * in_linesize[0]);
        _U = input[1] + ((y/2) * in_linesize[1]);
        _V = _U + 1;

        _out = output + (y * out_linesize);

        for (uint32_t x = 0; x < width; x+=2) {
            *(_out++) = *(_U++); _U++;
            *(_out++) = *(_Y++);
            *(_out++) = *(_V++); _V++;
            *(_out++) = *(_Y++);
        }
    }
}

void convert_i420_to_uyvy(uint8_t* input[], uint32_t in_linesize[],
    uint32_t start_y, uint32_t end_y,
    uint8_t* output, uint32_t out_linesize)
{
    uint8_t* _Y;
    uint8_t* _U;
    uint8_t* _V;
    uint8_t* _out;
    uint32_t width = min_uint32(in_linesize[0], out_linesize);
    for (uint32_t y = start_y; y < end_y; y++) {
        _Y = input[0] + (y * in_linesize[0]);
        _U = input[1] + ((y/2) * in_linesize[1]);
        _V = input[2] + ((y/2) * in_linesize[2]);

        _out = output + (y * out_linesize);

        for (uint32_t x = 0; x < width; x += 2) {
            *(_out++) = *(_U++);
            *(_out++) = *(_Y++);
            *(_out++) = *(_V++);
            *(_out++) = *(_Y++);
        }
    }
}

void convert_i444_to_uyvy(uint8_t* input[], uint32_t in_linesize[],
    uint32_t start_y, uint32_t end_y,
    uint8_t* output, uint32_t out_linesize)
{
    uint8_t* _Y;
    uint8_t* _U;
    uint8_t* _V;
    uint8_t* _out;
    uint32_t width = min_uint32(in_linesize[0], out_linesize);
    for (uint32_t y = start_y; y < end_y; y++) {
        _Y = input[0] + (y * in_linesize[0]);
        _U = input[1] + (y * in_linesize[1]);
        _V = input[2] + (y * in_linesize[2]);

        _out = output + (y * out_linesize);

        for (uint32_t x = 0; x < width; x += 2) {
            // Quality loss here. Some chroma samples are ignored.
            *(_out++) = *(_U++); _U++;
            *(_out++) = *(_Y++);
            *(_out++) = *(_V++); _V++;
            *(_out++) = *(_Y++);
        }
    }
}

void ndi_output_rawvideo(void* data, struct video_data* frame) {
    struct ndi_output* o = static_cast<ndi_output*>(data);
    if (!o->started)
        return;

    uint32_t width = o->video_info.output_width;
    uint32_t height = o->video_info.output_height;

    NDIlib_video_frame_v2_t video_frame = {0};
    video_frame.xres = width;
    video_frame.yres = height;
    video_frame.frame_rate_N = o->video_info.fps_num;
    video_frame.frame_rate_D = o->video_info.fps_den;
    video_frame.picture_aspect_ratio = (float)width / (float)height;
    video_frame.frame_format_type = NDIlib_frame_format_type_progressive;
    video_frame.timecode = (int64_t)(frame->timestamp / 100.0);

    video_frame.FourCC = o->frame_format;
    if (video_frame.FourCC == NDIlib_FourCC_type_UYVY) {
        video_format source_f = o->video_info.output_format;

        if (source_f == VIDEO_FORMAT_NV12) {
            profile_start("convert_nv12_to_uyvy");
            convert_nv12_to_uyvy(frame->data, frame->linesize,
                0, height,
                o->conv_buffer, o->conv_linesize);
            profile_end("convert_nv12_to_uyvy");
        }
        else if (source_f == VIDEO_FORMAT_I420) {
            profile_start("convert_i420_to_uyvy");
            convert_i420_to_uyvy(frame->data, frame->linesize,
                0, height,
                o->conv_buffer, o->conv_linesize);
            profile_end("convert_i420_to_uyvy");
        }
        else if (source_f == VIDEO_FORMAT_I444) {
            profile_start("convert_i444_to_uyvy");
            convert_i444_to_uyvy(frame->data, frame->linesize,
                0, height,
                o->conv_buffer, o->conv_linesize);
            profile_end("convert_i444_to_uyvy");
        }

        video_frame.p_data = o->conv_buffer;
        video_frame.line_stride_in_bytes = o->conv_linesize;
    }
    else {
        video_frame.p_data = frame->data[0];
        video_frame.line_stride_in_bytes = frame->linesize[0];
    }

    if (o->async_sending)
        ndiLib->NDIlib_send_send_video_async_v2(o->ndi_sender, &video_frame);
    else
        ndiLib->NDIlib_send_send_video_v2(o->ndi_sender, &video_frame);
}

void ndi_output_rawaudio(void* data, struct audio_data* frame) {
    struct ndi_output* o = static_cast<ndi_output*>(data);
    if (!o->started) return;

    NDIlib_audio_frame_v2_t audio_frame = {0};
    audio_frame.sample_rate = o->audio_info.samples_per_sec;
    audio_frame.no_channels = o->audio_info.speakers;
    audio_frame.no_samples = frame->frames;
    audio_frame.channel_stride_in_bytes = frame->frames * 4;

    size_t data_size =
        audio_frame.no_channels * audio_frame.channel_stride_in_bytes;
    uint8_t* audio_data = (uint8_t*)bmalloc(data_size);

    for (int i = 0; i < audio_frame.no_channels; i++) {
        memcpy(&audio_data[i * audio_frame.channel_stride_in_bytes],
            frame->data[i],
            audio_frame.channel_stride_in_bytes);
    }

    audio_frame.p_data = (float*)audio_data;
    audio_frame.timecode = (int64_t)(frame->timestamp / 100.0);

    ndiLib->NDIlib_send_send_audio_v2(o->ndi_sender, &audio_frame);
    bfree(audio_data);
}

struct obs_output_info create_ndi_output_info() {
    struct obs_output_info ndi_output_info = {};
    ndi_output_info.id				= "ndi_output";
    ndi_output_info.flags			= OBS_OUTPUT_AV;
    ndi_output_info.get_name		= ndi_output_getname;
    ndi_output_info.get_properties	= ndi_output_getproperties;
    ndi_output_info.create			= ndi_output_create;
    ndi_output_info.destroy			= ndi_output_destroy;
    ndi_output_info.update			= ndi_output_update;
    ndi_output_info.start			= ndi_output_start;
    ndi_output_info.stop			= ndi_output_stop;
    ndi_output_info.raw_video		= ndi_output_rawvideo;
    ndi_output_info.raw_audio		= ndi_output_rawaudio;

    return ndi_output_info;
}
