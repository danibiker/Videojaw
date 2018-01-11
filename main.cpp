/*
 * To change this license header, choose License Headers in Project Properties.
 * To change this template file, choose Tools | Templates
 * and open the template in the editor.
 */

/* 
 * File:   main.cpp
 * Author: dmarcobo
 *
 * Created on 11 de enero de 2018, 10:26
 */

#include <cstdlib>
#include <string>
#include <array>
#include <SDL/SDL.h>
#include <SDL/SDL_ttf.h>
#include <SDL/SDL_image.h>
#include <SDL/SDL_rotozoom.h>
#include <SDL/SDL_mixer.h>

using namespace std;

extern "C"{
    #include <libavutil/avstring.h>
    #include <libavutil/imgutils.h>
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libavfilter/buffersink.h>
    #include <libavfilter/buffersrc.h>
    #include <libavutil/opt.h>
    #include <libavutil/pixdesc.h>
    #include <libswscale/swscale.h>
}

//Buffer for errors added to c++ compatibility
char buffErrors[AV_ERROR_MAX_STRING_SIZE];

// Set up the pixel format color masks for RGB(A) byte arrays.
// Only STBI_rgb (3) and STBI_rgb_alpha (4) are supported here!
/* SDL interprets each pixel as a 32-bit number, so our masks must depend
  on the endianness (byte order) of the machine */
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
   static const uint32_t rmask = 0xff000000;
   static const uint32_t gmask = 0x00ff0000;
   static const uint32_t bmask = 0x0000ff00;
   static const uint32_t amask = 0x000000ff;
#else
   static const uint32_t rmask = 0x000000ff;
   static const uint32_t gmask = 0x0000ff00;
   static const uint32_t bmask = 0x00ff0000;
   static const uint32_t amask = 0xff000000;
#endif

static AVFormatContext *ifmt_ctx;
static AVFormatContext *ofmt_ctx;
typedef struct FilteringContext {
    AVFilterContext *buffersink_ctx;
    AVFilterContext *buffersrc_ctx;
    AVFilterGraph *filter_graph;
} FilteringContext;
static FilteringContext *filter_ctx;

typedef struct StreamContext {
    AVCodecContext *dec_ctx;
    AVCodecContext *enc_ctx;
} StreamContext;
static StreamContext *stream_ctx;

/**
 * 
 * @param errbuf
 * @param errbuf_size
 * @param errnum
 * @return 
 */
char *av_cplus_make_error_string(char *errbuf, size_t errbuf_size, int errnum)
{
    av_strerror(errnum, errbuf, errbuf_size);
    return errbuf;
}

/**
 * 
 * @param errnum
 * @return 
 */
char *av_cplus_err2str(int errnum){
    memset(buffErrors, '\0', AV_ERROR_MAX_STRING_SIZE);
    av_cplus_make_error_string(buffErrors, AV_ERROR_MAX_STRING_SIZE, errnum);
    return buffErrors;
}

/**
 * 
 * @param filename
 * @return 
 */
static int open_input_file(const char *filename)
{
    int ret;
    unsigned int i;

    ifmt_ctx = NULL;
    if ((ret = avformat_open_input(&ifmt_ctx, filename, NULL, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open input file\n");
        return ret;
    }

    if ((ret = avformat_find_stream_info(ifmt_ctx, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
        return ret;
    }

    stream_ctx = av_mallocz_array(ifmt_ctx->nb_streams, sizeof(*stream_ctx));
    if (!stream_ctx)
        return AVERROR(ENOMEM);

    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVStream *stream = ifmt_ctx->streams[i];
        AVCodec *dec = avcodec_find_decoder(stream->codecpar->codec_id);
        AVCodecContext *codec_ctx;
        if (!dec) {
            av_log(NULL, AV_LOG_ERROR, "Failed to find decoder for stream #%u\n", i);
            return AVERROR_DECODER_NOT_FOUND;
        }
        codec_ctx = avcodec_alloc_context3(dec);
        if (!codec_ctx) {
            av_log(NULL, AV_LOG_ERROR, "Failed to allocate the decoder context for stream #%u\n", i);
            return AVERROR(ENOMEM);
        }
        ret = avcodec_parameters_to_context(codec_ctx, stream->codecpar);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Failed to copy decoder parameters to input decoder context "
                   "for stream #%u\n", i);
            return ret;
        }
        /* Reencode video & audio and remux subtitles etc. */
        if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO
                || codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO)
                codec_ctx->framerate = av_guess_frame_rate(ifmt_ctx, stream, NULL);
            /* Open decoder */
            ret = avcodec_open2(codec_ctx, dec, NULL);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Failed to open decoder for stream #%u\n", i);
                return ret;
            }
        }
        stream_ctx[i].dec_ctx = codec_ctx;
    }

    av_dump_format(ifmt_ctx, 0, filename, 0);
    return 0;
}

/**
 * 
 * @param filename
 * @return 
 */
static int open_output_file(const char *filename)
{
    AVStream *out_stream;
    AVStream *in_stream;
    AVCodecContext *dec_ctx, *enc_ctx;
    AVCodec *encoder;
    int ret;
    unsigned int i;

    ofmt_ctx = NULL;
    avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, filename);
    if (!ofmt_ctx) {
        av_log(NULL, AV_LOG_ERROR, "Could not create output context\n");
        return AVERROR_UNKNOWN;
    }
    
    /* Guess the desired container format based on the file extension. */
    if (!(ofmt_ctx->oformat = av_guess_format(NULL, filename, NULL))) {
        av_log(NULL, AV_LOG_ERROR, "Could not find output file format\n");
            return AVERROR_UNKNOWN;
    }

    av_strlcpy(ofmt_ctx->filename, filename, sizeof(ofmt_ctx->filename));

    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        out_stream = avformat_new_stream(ofmt_ctx, NULL);
        if (!out_stream) {
            av_log(NULL, AV_LOG_ERROR, "Failed allocating output stream\n");
            return AVERROR_UNKNOWN;
        }

        in_stream = ifmt_ctx->streams[i];
        dec_ctx = stream_ctx[i].dec_ctx;

        if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO
                || dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
            //in this example, we choose transcoding to same codec
//            encoder = avcodec_find_encoder(dec_ctx->codec_id);
            
            //Encoding to the guessed format 
            if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO){
                encoder = avcodec_find_encoder(ofmt_ctx->oformat->video_codec);
                //encoder = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
                
            } else if (dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO){
                encoder = avcodec_find_encoder(ofmt_ctx->oformat->audio_codec);
                //encoder = avcodec_find_encoder(AV_CODEC_ID_MP3);
            }
            
            if (!encoder) {
                av_log(NULL, AV_LOG_FATAL, "Necessary encoder not found\n");
                return AVERROR_INVALIDDATA;
            }
            enc_ctx = avcodec_alloc_context3(encoder);
            if (!enc_ctx) {
                av_log(NULL, AV_LOG_FATAL, "Failed to allocate the encoder context\n");
                return AVERROR(ENOMEM);
            }

            /* In this example, we transcode to same properties (picture size,
             * sample rate etc.). These properties can be changed for output
             * streams easily using filters */
            if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
                enc_ctx->height = dec_ctx->height;
                enc_ctx->width = dec_ctx->width;
                enc_ctx->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;
                /* take first format from list of supported formats */
                if (encoder->pix_fmts)
                    enc_ctx->pix_fmt = encoder->pix_fmts[0];
                else
                    enc_ctx->pix_fmt = dec_ctx->pix_fmt;
                /* video time_base can be set to whatever is handy and supported by encoder */
                if (av_q2d(dec_ctx->framerate) <= 0.0 ){
                   enc_ctx->time_base = av_inv_q(dec_ctx->framerate);
                } else {
                   enc_ctx->time_base = dec_ctx->time_base; 
                   //enc_ctx->time_base = av_make_q(60000,1001);
                }
                
            } else {
                enc_ctx->sample_rate = dec_ctx->sample_rate;
                enc_ctx->channel_layout = dec_ctx->channel_layout;
                enc_ctx->channels = av_get_channel_layout_nb_channels(enc_ctx->channel_layout);
                /* take first format from list of supported formats */
                enc_ctx->sample_fmt = encoder->sample_fmts[0];
                enc_ctx->time_base = (AVRational){1, enc_ctx->sample_rate};
            }

            /* Third parameter can be used to pass settings to encoder */
            ret = avcodec_open2(enc_ctx, encoder, NULL);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Cannot open video encoder for stream #%u\n", i);
                return ret;
            }
            ret = avcodec_parameters_from_context(out_stream->codecpar, enc_ctx);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Failed to copy encoder parameters to output stream #%u\n", i);
                return ret;
            }
            if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
                enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

            out_stream->time_base = enc_ctx->time_base;
            stream_ctx[i].enc_ctx = enc_ctx;
            
        } else if (dec_ctx->codec_type == AVMEDIA_TYPE_UNKNOWN) {
            av_log(NULL, AV_LOG_FATAL, "Elementary stream #%d is of unknown type, cannot proceed\n", i);
            return AVERROR_INVALIDDATA;
        } else {
            /* if this stream must be remuxed */
            ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Copying parameters for stream #%u failed\n", i);
                return ret;
            }
            out_stream->time_base = in_stream->time_base;
        }

    }
    av_dump_format(ofmt_ctx, 0, filename, 1);

    if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&ofmt_ctx->pb, filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Could not open output file '%s'", filename);
            return ret;
        }
    }

    /* init muxer, write output file header */
    ret = avformat_write_header(ofmt_ctx, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error occurred when opening output file\n");
        return ret;
    }

    return 0;
}

/**
 * 
 * @param fctx
 * @param dec_ctx
 * @param enc_ctx
 * @param filter_spec
 * @return 
 */
static int init_filter(FilteringContext* fctx, AVCodecContext *dec_ctx,
        AVCodecContext *enc_ctx, const char *filter_spec)
{
    char args[512];
    int ret = 0;
    const AVFilter *buffersrc = NULL;
    const AVFilter *buffersink = NULL;
    AVFilterContext *buffersrc_ctx = NULL;
    AVFilterContext *buffersink_ctx = NULL;
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    AVFilterGraph *filter_graph = avfilter_graph_alloc();

    if (!outputs || !inputs || !filter_graph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        buffersrc = avfilter_get_by_name("buffer");
        buffersink = avfilter_get_by_name("buffersink");
        if (!buffersrc || !buffersink) {
            av_log(NULL, AV_LOG_ERROR, "filtering source or sink element not found\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }

        snprintf(args, sizeof(args),
                "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
                dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
                dec_ctx->time_base.num, dec_ctx->time_base.den,
                dec_ctx->sample_aspect_ratio.num,
                dec_ctx->sample_aspect_ratio.den);

        ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
                args, NULL, filter_graph);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot create buffer source\n");
            goto end;
        }

        ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
                NULL, NULL, filter_graph);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot create buffer sink\n");
            goto end;
        }

        ret = av_opt_set_bin(buffersink_ctx, "pix_fmts",
                (uint8_t*)&enc_ctx->pix_fmt, sizeof(enc_ctx->pix_fmt),
                AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot set output pixel format\n");
            goto end;
        }
    } else if (dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
        buffersrc = avfilter_get_by_name("abuffer");
        buffersink = avfilter_get_by_name("abuffersink");
        if (!buffersrc || !buffersink) {
            av_log(NULL, AV_LOG_ERROR, "filtering source or sink element not found\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }

        if (!dec_ctx->channel_layout)
            dec_ctx->channel_layout =
                av_get_default_channel_layout(dec_ctx->channels);
        
        snprintf(args, sizeof(args),
                //"time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%"PRIx64,
                "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%",
                dec_ctx->time_base.num, dec_ctx->time_base.den, dec_ctx->sample_rate,
                av_get_sample_fmt_name(dec_ctx->sample_fmt),
                dec_ctx->channel_layout);
        
        ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
                args, NULL, filter_graph);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot create audio buffer source\n");
            goto end;
        }

        ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
                NULL, NULL, filter_graph);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot create audio buffer sink\n");
            goto end;
        }

        ret = av_opt_set_bin(buffersink_ctx, "sample_fmts",
                (uint8_t*)&enc_ctx->sample_fmt, sizeof(enc_ctx->sample_fmt),
                AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot set output sample format\n");
            goto end;
        }

        ret = av_opt_set_bin(buffersink_ctx, "channel_layouts",
                (uint8_t*)&enc_ctx->channel_layout,
                sizeof(enc_ctx->channel_layout), AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot set output channel layout\n");
            goto end;
        }

        ret = av_opt_set_bin(buffersink_ctx, "sample_rates",
                (uint8_t*)&enc_ctx->sample_rate, sizeof(enc_ctx->sample_rate),
                AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot set output sample rate\n");
            goto end;
        }
    } else {
        ret = AVERROR_UNKNOWN;
        goto end;
    }

    /* Endpoints for the filter graph. */
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    inputs->name       = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;

    if (!outputs->name || !inputs->name) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if ((ret = avfilter_graph_parse_ptr(filter_graph, filter_spec,
                    &inputs, &outputs, NULL)) < 0)
        goto end;

    if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
        goto end;

    /* Fill FilteringContext */
    fctx->buffersrc_ctx = buffersrc_ctx;
    fctx->buffersink_ctx = buffersink_ctx;
    fctx->filter_graph = filter_graph;

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    return ret;
}

/**
 * 
 * @return 
 */
static int init_filters(void)
{
    const char *filter_spec;
    unsigned int i;
    int ret;
    filter_ctx = av_malloc_array(ifmt_ctx->nb_streams, sizeof(*filter_ctx));
    if (!filter_ctx)
        return AVERROR(ENOMEM);

    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        filter_ctx[i].buffersrc_ctx  = NULL;
        filter_ctx[i].buffersink_ctx = NULL;
        filter_ctx[i].filter_graph   = NULL;
        if (!(ifmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO
                || ifmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO))
            continue;


        if (ifmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            filter_spec = "null"; /* passthrough (dummy) filter for video */
        else
            filter_spec = "anull"; /* passthrough (dummy) filter for audio */
        ret = init_filter(&filter_ctx[i], stream_ctx[i].dec_ctx,
                stream_ctx[i].enc_ctx, filter_spec);
        if (ret)
            return ret;
    }
    return 0;
}

/**
 * 
 * @param filt_frame
 * @param stream_index
 * @param got_frame
 * @return 
 */
static int encode_write_frame(AVFrame *filt_frame, unsigned int stream_index, int *got_frame) {

    int ret;
    int got_frame_local;
    AVPacket enc_pkt;
    
    if (!got_frame)
        got_frame = &got_frame_local;

//    av_log(NULL, AV_LOG_INFO, "Encoding frame\n");
    /* encode filtered frame */
    enc_pkt.data = NULL;
    enc_pkt.size = 0;
    av_init_packet(&enc_pkt);
    
    
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57, 48, 0)  
    int (*enc_func)(AVCodecContext *, AVPacket *, const AVFrame *, int *) =
        (ifmt_ctx->streams[stream_index]->codecpar->codec_type ==
         AVMEDIA_TYPE_VIDEO) ? avcodec_encode_video2 : avcodec_encode_audio2;
  
    ret = enc_func(stream_ctx[stream_index].enc_ctx, &enc_pkt,
            filt_frame, got_frame);
#else
    *got_frame = 0;
    ret = avcodec_send_frame(stream_ctx[stream_index].enc_ctx, filt_frame);
    if ( ret != AVERROR_EOF && ret != AVERROR(EAGAIN) && ret != 0){
        fprintf(stderr, "Could not send frame (error '%s')\n",
                    av_cplus_err2str(ret));
        return ret;
    }
    
    if ( (ret = avcodec_receive_packet(stream_ctx[stream_index].enc_ctx, &enc_pkt)) == 0)
        *got_frame = 1;
       
    if ( ret != AVERROR_EOF && ret != AVERROR(EAGAIN) && ret != 0){
        fprintf(stderr, "Could not receive packet (error '%s')\n",
                    av_cplus_err2str(ret));
        return ret;
    }   
#endif    
    av_frame_free(&filt_frame);
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57, 48, 0)
    if (ret < 0)
        return ret;
#endif
    if (!(*got_frame)){
        return 0;
    } 

    /* prepare packet for muxing */
    enc_pkt.stream_index = stream_index;
    av_packet_rescale_ts(&enc_pkt,
                         stream_ctx[stream_index].enc_ctx->time_base,
                         ofmt_ctx->streams[stream_index]->time_base);
    
    av_log(NULL, AV_LOG_DEBUG, "Muxing frame\n");
    /* mux encoded frame */
    ret = av_interleaved_write_frame(ofmt_ctx, &enc_pkt);
    av_packet_unref(&enc_pkt);
    return ret;
}

Uint32 getpixel(SDL_Surface *surface, const int x, const int y)
{
    //int bpp = surface->format->BytesPerPixel;
    /* Here p is the address to the pixel we want to retrieve */
    Uint8 *p = (Uint8 *)surface->pixels + y * surface->pitch + x * surface->format->BytesPerPixel;

    switch(surface->format->BytesPerPixel) {
    case 1:
        return *p;

    case 2:
        return *(Uint16 *)p;

    case 3:
        if(SDL_BYTEORDER == SDL_BIG_ENDIAN)
            return p[0] << 16 | p[1] << 8 | p[2];
        else
            return p[0] | p[1] << 8 | p[2] << 16;

    case 4:
        return *(Uint32 *)p;

    default:
        return 0;       /* shouldn't happen, but avoids warnings */
    }
}

/**
 * 
 * @param frame
 * @param stream_index
 * @return 
 */
static int filter_encode_write_frame(AVFrame *frame, unsigned int stream_index)
{
    int ret;
    AVFrame *filt_frame;

//    av_log(NULL, AV_LOG_INFO, "Pushing decoded frame to filters\n");
    /* push the decoded frame into the filtergraph */
    ret = av_buffersrc_add_frame_flags(filter_ctx[stream_index].buffersrc_ctx,
            frame, 0);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error while feeding the filtergraph\n");
        return ret;
    }

    /* pull filtered frames from the filtergraph */
    while (1) {
        filt_frame = av_frame_alloc();
        if (!filt_frame) {
            ret = AVERROR(ENOMEM);
            break;
        }
//        av_log(NULL, AV_LOG_INFO, "Pulling filtered frame from filters\n");
        ret = av_buffersink_get_frame(filter_ctx[stream_index].buffersink_ctx,
                filt_frame);
        if (ret < 0) {
            /* if no more frames for output - returns AVERROR(EAGAIN)
             * if flushed and no more frames for output - returns AVERROR_EOF
             * rewrite retcode to 0 to show it as normal procedure completion
             */
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                ret = 0;
            av_frame_free(&filt_frame);
            break;
        }
        
        filt_frame->pict_type = AV_PICTURE_TYPE_NONE;
        
        static int nframe = 0;
        av_log(NULL, AV_LOG_INFO, "nframe %d\n", nframe);
        
        //Se obtiene la imagen
#define FRAMEMODIF 0
        
#ifdef FRAMEMODIF 
        int width = filt_frame->width;
        int height = filt_frame->height;
        uint8_t* rgb_data[4];  
        int rgb_linesize[4];
        
        //Especificamos el formato y tamanyo de la imagen origen y tambien la de destino
        SwsContext *sws_ctx = sws_getContext(width, height, filt_frame->format , width, height, 
                AV_PIX_FMT_RGB24, SWS_BICUBIC, NULL, NULL, NULL);
        
        
        //Hacemos espacio para la imagen de destino que modificaremos
        av_image_alloc(rgb_data, rgb_linesize, width, height, AV_PIX_FMT_RGB24, 32); 
        //Escalamos la imagen en el destino
        sws_scale(sws_ctx, filt_frame->data, filt_frame->linesize, 0, height, rgb_data, rgb_linesize);
        // RGB24 is a packed format. It means there is only one plane and all data in it. 
//        size_t rgb_size = width * height * 3;
//        uint8_t *rgb_arr = new uint8_t[rgb_size];
        //std::copy_n(rgb_data[0], rgb_size, rgb_arr);
        //Do something with the image
        SDL_Surface* surf = SDL_CreateRGBSurfaceFrom((uint8_t *)rgb_data[0], width, height, 24, width*3,
                                             rmask, gmask, bmask, amask);
        
        SDL_LockSurface(surf);
        
        SDL_Rect location = {width / 2 - 60 / 2, height / 2 - 60 / 2, 60, 60};
        SDL_FillRect(surf, &location, SDL_MapRGB(surf->format, 255,0,0));
        
        if (surf == NULL) {
            av_log(NULL, AV_LOG_INFO, "Creating surface failed %s\n", SDL_GetError());
            break;
        } 
        
        if (nframe == 10)
        if (SDL_SaveBMP(surf, "C:\\clips\\ejemplo.bmp") != 0){
            av_log(NULL, AV_LOG_INFO, "Save bitmap failed %s\n", SDL_GetError());
        }
        
//        Uint8 r,g,b;
        Uint8 *p;
//        for (int y=0; y < height; y++){
//            for (int x=0; x < width; x++){
//                p=(Uint8 *)surf->pixels + y * surf->pitch + x * surf->format->BytesPerPixel;
//                //SDL_GetRGB(p, surf->format, &r,&g,&b);
//               rgb_data[0][(y*width + x)*3] = p[0];     //r
//               rgb_data[0][(y*width + x)*3 + 1] = p[1]; //g
//               rgb_data[0][(y*width + x)*3 + 2] = p[2]; //b
//            }
//        }
        for (int i=0; i < width * height; i++){
            p=(Uint8 *)surf->pixels + i*3;
            rgb_data[0][i*3] = p[0]; //r
            rgb_data[0][i*3+1] = p[1]; //g
            rgb_data[0][i*3+2] = p[2]; //b
        }
        
        //Volvemos a copiar la imagen modificada en el frame de destino
        //std::copy_n(rgb_arr, rgb_size, rgb_data[0]);
        SwsContext *sws_ctx2 = sws_getContext(width, height, AV_PIX_FMT_RGB24, width, height, filt_frame->format, SWS_BICUBIC, NULL, NULL, NULL);
        sws_scale(sws_ctx2, rgb_data, rgb_linesize, 0, height, filt_frame->data, filt_frame->linesize);
        SDL_UnlockSurface(surf);
        SDL_FreeSurface(surf);
#endif        
        //End do something with the image
//        delete rgb_arr;
        
        ret = encode_write_frame(filt_frame, stream_index, NULL);
        nframe++;
        if (ret < 0)
            break;
    }
    return ret;
}
/**
 * 
 * @param stream_index
 * @return 
 */
static int flush_encoder(unsigned int stream_index)
{
    int ret;
    int got_frame;

    if (!(stream_ctx[stream_index].enc_ctx->codec->capabilities &
                AV_CODEC_CAP_DELAY))
        return 0;

    while (1) {
        //av_log(NULL, AV_LOG_INFO, "Flushing stream #%u encoder\n", stream_index);
        ret = encode_write_frame(NULL, stream_index, &got_frame);
        if (ret < 0)
            break;
        if (!got_frame)
            return 0;
    }
    return ret;
}

/*
 * 
 */
/**
 * 
 * @param argc
 * @param argv
 * @return 
 */
int main(int argc, char **argv)
{
    int ret;
    AVPacket packet; 
    packet.data = NULL;
    packet.size = 0;
    AVFrame *frame = NULL;
    unsigned int stream_index;
    unsigned int i;
    int got_frame;
    
    SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER);			// Initialize SDL
    SDL_Surface* screen = SDL_SetVideoMode(640, 480, 24, SDL_SWSURFACE|SDL_RESIZABLE);
    
    if (argc != 3) {
        av_log(NULL, AV_LOG_ERROR, "Usage: %s <input file> <output file>\n", argv[0]);
        return 1;
    }
    
    av_register_all();
    avfilter_register_all();

    if ((ret = open_input_file(argv[1])) < 0)
        goto end;
    if ((ret = open_output_file(argv[2])) < 0)
        goto end;
    if ((ret = init_filters()) < 0)
        goto end;

    /* read all packets */
    while (1) {
        if ((ret = av_read_frame(ifmt_ctx, &packet)) < 0)
            break;
        stream_index = packet.stream_index;
        
        av_log(NULL, AV_LOG_DEBUG, "Demuxer gave frame of stream_index %u\n",
                stream_index);

        if (filter_ctx[stream_index].filter_graph) {
            av_log(NULL, AV_LOG_DEBUG, "Going to reencode&filter the frame\n");
            frame = av_frame_alloc();
            if (!frame) {
                ret = AVERROR(ENOMEM);
                break;
            }
            av_packet_rescale_ts(&packet,
                                 ifmt_ctx->streams[stream_index]->time_base,
                                 stream_ctx[stream_index].dec_ctx->time_base);
            
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57, 48, 0)  
            enum AVMediaType type = ifmt_ctx->streams[packet.stream_index]->codecpar->codec_type;
            int (*dec_func)(AVCodecContext *, AVFrame *, int *, const AVPacket *);
            dec_func = (type == AVMEDIA_TYPE_VIDEO) ? avcodec_decode_video2 :
                avcodec_decode_audio4;
            
            ret = dec_func(stream_ctx[stream_index].dec_ctx, frame,
                    &got_frame, &packet);
            if (ret < 0) {
                av_frame_free(&frame);
                av_log(NULL, AV_LOG_ERROR, "Decoding failed\n");
                break;
            }
#else
            int ret;
            // AVERROR(EAGAIN) means that we need to feed more
            // That we can decode Frame or Packet
            do {
                do {
                    ret = avcodec_send_packet(stream_ctx[stream_index].dec_ctx, &packet);
                } while(ret == AVERROR(EAGAIN));

                if(ret == AVERROR_EOF || ret == AVERROR(EINVAL)) {
                    printf("AVERROR(EAGAIN): %d, AVERROR_EOF: %d, AVERROR(EINVAL): %d\n", AVERROR(EAGAIN), AVERROR_EOF, AVERROR(EINVAL));
                    printf("fe_read_frame: Frame getting error (%d)!\n", ret);
                    return ret;
                } else {
                    got_frame = 1;
                }
                ret = avcodec_receive_frame(stream_ctx[stream_index].dec_ctx, frame);
            } while(ret == AVERROR(EAGAIN));

            if(ret == AVERROR_EOF){
                got_frame = 0;
            }

            if(ret == AVERROR(EINVAL)) {
                // An error or EOF occured,index break out and return what
                // we have so far.
                fprintf(stderr, "Could not decode frame (error '%s')\n",
                av_cplus_err2str(ret));
                av_packet_unref(&packet);
                return ret;
            }
#endif

            if (got_frame) {
                frame->pts = frame->best_effort_timestamp;
                ret = filter_encode_write_frame(frame, stream_index);
                av_frame_free(&frame);
                if (ret < 0)
                    goto end;
            } else {
                av_frame_free(&frame);
            }
        } else {
            /* remux this frame without reencoding */
            av_packet_rescale_ts(&packet,
                                 ifmt_ctx->streams[stream_index]->time_base,
                                 ofmt_ctx->streams[stream_index]->time_base);

            ret = av_interleaved_write_frame(ofmt_ctx, &packet);
            if (ret < 0)
                goto end;
        }
        av_packet_unref(&packet);
    }

    /* flush filters and encoders */
    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        /* flush filter */
        if (!filter_ctx[i].filter_graph)
            continue;
        ret = filter_encode_write_frame(NULL, i);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Flushing filter failed\n");
            goto end;
        }

        /* flush encoder */
        ret = flush_encoder(i);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Flushing encoder failed\n");
            goto end;
        }
    }

    av_write_trailer(ofmt_ctx);
end:
    av_packet_unref(&packet);
    av_frame_free(&frame);
    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        avcodec_free_context(&stream_ctx[i].dec_ctx);
        if (ofmt_ctx && ofmt_ctx->nb_streams > i && ofmt_ctx->streams[i] && stream_ctx[i].enc_ctx)
            avcodec_free_context(&stream_ctx[i].enc_ctx);
        if (filter_ctx && filter_ctx[i].filter_graph)
            avfilter_graph_free(&filter_ctx[i].filter_graph);
    }
    av_free(filter_ctx);
    av_free(stream_ctx);
    avformat_close_input(&ifmt_ctx);
    if (ofmt_ctx && !(ofmt_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&ofmt_ctx->pb);
    avformat_free_context(ofmt_ctx);

    if (ret < 0){
        av_log(NULL, AV_LOG_ERROR, "Error occurred\n");
    }
    
    SDL_Quit();
    return ret ? 1 : 0;
}