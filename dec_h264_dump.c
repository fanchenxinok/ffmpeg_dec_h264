#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <libavformat/avformat.h>
#include <libavutil/time.h>

// In ffmpeg/doc/APIchanges:
// 2016-04-21 - 7fc329e - lavc 57.37.100 - avcodec.h
//   Add a new audio/video encoding and decoding API with decoupled input
//   and output -- avcodec_send_packet(), avcodec_receive_frame(),
//   avcodec_send_frame() and avcodec_receive_packet().
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 37, 100)
# define SCRCPY_LAVF_HAS_NEW_ENCODING_DECODING_API
#endif

#define DUMP_DEC_DATA	(1)

typedef enum
{
	EN_FALSE = 0,
	EN_TRUE = 1
}enBool;

typedef struct
{
	uint32_t  Width;
	uint32_t  Height;
	uint8_t*  YUV_Data[3];
	uint32_t  YUV_LineSize[3];
	uint32_t  Frame_Num;
}stFrameBuffObj;

typedef int (*frame_handle_callback)(void* usr_data);

typedef struct
{
	AVCodecContext *codec_ctx;
    AVCodecParserContext *parser;
	
	AVFrame *decoding_frame[2];
	int current_decodeing_frame;
	frame_handle_callback frame_handle_cb;
	int queue_id;
	
	void *usr_data;
	enBool decode_start;
}stAvDecStream;

static void dump_nv12_data(AVFrame *frame, int frame_number)
{
	#if DUMP_DEC_DATA
	if(frame_number > 10) return;
	FILE *pf = NULL;
	char file[64] = {'\0'};
	sprintf(file, "./output/frame%04d.NV12", frame_number);
	pf = fopen(file, "wb+");

	/*
		YY YY YY YY
		YY YY YY YY
		UV UV UV UV
	*/
	if(pf) {
		printf("Y: %d, U: %d, V: %d\n",
			frame->linesize[0], frame->linesize[1], frame->linesize[2]);
		// write Y data
		int h = 0;
		uint8_t *pStartY = frame->data[0];
		for(h = 0; h < frame->height; h++) {
			fwrite(pStartY, frame->width, 1, pf);
			pStartY += frame->linesize[0];
		}

		// write UV data
		uint8_t *pStartU = frame->data[1];
		uint8_t *pStartV = frame->data[2];
		uint8_t uv_line[1920] = {0};
		for(h = 0; h < frame->height / 2; h++) {
			int w = 0, idx = 0;
			for(w = 0; w < frame->width; w += 2) {
				uv_line[w] = pStartU[idx];
				uv_line[w + 1] = pStartV[idx];
				idx++;
			}
			
			fwrite(uv_line, frame->width, 1, pf);
			pStartU += frame->linesize[1];
			pStartV += frame->linesize[2];
		}
		printf("Dump: %s\n", file);
	}
	#endif
}

static int decode_packet(stAvDecStream *pAvDecStream, const AVPacket *packet) {
	if(pAvDecStream->decode_start == EN_FALSE) return 0;
		
	#ifdef SCRCPY_LAVF_HAS_NEW_ENCODING_DECODING_API
    int ret;
    if ((ret = avcodec_send_packet(pAvDecStream->codec_ctx, packet)) < 0) {
        printf("Could not send video packet: %d\n", ret);
        return -1;
    }
    ret = avcodec_receive_frame(pAvDecStream->codec_ctx, pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]);
    if (!ret) {
        // a frame was received
        /* pAvDecStream->decoding_frame->data[0] 存储Y分量
		   pAvDecStream->decoding_frame->data[1] 存储U分量
		   pAvDecStream->decoding_frame->data[2] 存储V分量
		*/
        printf("1> cur buf[%d], frameNum = %d, pix_fmt = %d, frame WXH: %d x %d\n",
        	pAvDecStream->current_decodeing_frame,
         	pAvDecStream->codec_ctx->frame_number,
        	pAvDecStream->codec_ctx->pix_fmt, 
        	pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->width,
        	pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->height);
		
		if(AV_PIX_FMT_YUV420P == pAvDecStream->codec_ctx->pix_fmt) {
			dump_nv12_data(pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame], pAvDecStream->codec_ctx->frame_number);
		}
		pAvDecStream->current_decodeing_frame ^= 1;
    } else if (ret != AVERROR(EAGAIN)) {
        printf("Could not receive video frame: %d\n", ret);
        return -1;
    }
	
	#else
	
    int got_picture;
    int len = avcodec_decode_video2(pAvDecStream->codec_ctx,
                                    pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame],
                                    &got_picture,
                                    packet);
    if (len < 0) {
        printf("Could not decode video packet: %d\n", len);
        return -1;
    }
    if (got_picture) {
        /* pAvDecStream->decoding_frame->data[0] 存储Y分量
		   pAvDecStream->decoding_frame->data[1] 存储U分量
		   pAvDecStream->decoding_frame->data[2] 存储V分量
		*/
        printf("2> cur buf[%d], frameNum = %d, pix_fmt = %d, frame WXH: %d x %d\n",
        	pAvDecStream->current_decodeing_frame,
         	pAvDecStream->codec_ctx->frame_number,
        	pAvDecStream->codec_ctx->pix_fmt, 
        	pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->width,
        	pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->height);
		if(AV_PIX_FMT_YUV420P == pAvDecStream->codec_ctx->pix_fmt) {
			dump_nv12_data(pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame], pAvDecStream->codec_ctx->frame_number);
		}
		pAvDecStream->current_decodeing_frame ^= 1;
    }
	#endif

    return 0;
}


static int parse_packet(stAvDecStream *pAvDecStream, AVPacket *packet) {	
    uint8_t *in_data = packet->data;
    int in_len = packet->size;
    uint8_t *out_data = NULL;
    int out_len = 0;
    int r = av_parser_parse2(pAvDecStream->parser, pAvDecStream->codec_ctx,
                             &out_data, &out_len, in_data, in_len,
                             AV_NOPTS_VALUE, AV_NOPTS_VALUE, -1);

    // PARSER_FLAG_COMPLETE_FRAMES is set
    if((r != in_len) || (out_len != in_len)) {
		printf("r = %d, in_len = %d, out_len = %d\n", r, in_len, out_len);
		return -1;
    }

    if (pAvDecStream->parser->key_frame == 1) {
        packet->flags |= AV_PKT_FLAG_KEY;
    }

    return decode_packet(pAvDecStream, packet);
}


int av_parse_packet(stAvDecStream *pAvDecStream, void* packet_data, int32_t len, int64_t pts)
{
	static AVPacket packet;
	static int need_offset = 0;
	size_t offset = 0;

	if(need_offset == 0) {
		if (av_new_packet(&packet, len)) {
			printf("Could not allocate packet\n");
			return -1;
		}
	}
	else {
		/* pts 为 -1的帧和下一帧合并 */
		offset = packet.size;
		if (av_grow_packet(&packet, len)) {
            printf("Could not grow packet");
            return -1;
        }
	}

	memcpy(packet.data + offset, packet_data, len);
	packet.pts = (pts != -1) ? pts : AV_NOPTS_VALUE;
	if(packet.pts == AV_NOPTS_VALUE) {
		need_offset = 1;
		return 0;
	}

	parse_packet(pAvDecStream, &packet);

	need_offset = 0;
	
	av_packet_unref(&packet);
}

int av_dec_init(stAvDecStream *pAvDecStream)
{
	/* Initialize libavcodec, and register all codecs and formats. */
	avcodec_register_all();

	/* find the H264 video decoder */
	AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        printf("H.264 decoder not found\n");
        return -1;
    }

    pAvDecStream->codec_ctx = avcodec_alloc_context3(codec);
    if (!pAvDecStream->codec_ctx) {
        printf("Could not allocate codec context\n");
        return -1;
    }

	if (avcodec_open2(pAvDecStream->codec_ctx, codec, NULL) < 0) {
        printf("Could not open codec");
        avcodec_free_context(&pAvDecStream->codec_ctx);
        return -1;
    }

	pAvDecStream->parser = av_parser_init(AV_CODEC_ID_H264);
    if (!pAvDecStream->parser) {
        printf("Could not initialize parser\n");
        avcodec_free_context(&pAvDecStream->codec_ctx);
		return -1;
    }

	 // We must only pass complete frames to av_parser_parse2()!
     // It's more complicated, but this allows to reduce the latency by 1 frame!
     pAvDecStream->parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;

	 /* two frame buffer init */
	 pAvDecStream->decoding_frame[0] = av_frame_alloc();
	 pAvDecStream->decoding_frame[1] = av_frame_alloc();
	 pAvDecStream->current_decodeing_frame = 0;

}

void av_dec_deinit(stAvDecStream *pAvDecStream)
{
	av_parser_close(pAvDecStream->parser);
	avcodec_free_context(&pAvDecStream->codec_ctx);
    av_frame_free(&pAvDecStream->decoding_frame[0]);
	av_frame_free(&pAvDecStream->decoding_frame[1]);
}

void av_loop_dec_file(stAvDecStream *pAvDecStream)
{
	uint8_t *pkg_buff = (uint8_t*)malloc(sizeof(uint8_t) * 1920 * 1080 * 4);
	FILE* fp_h264_stream, *fp_h264_size, *fp_h264_pts;
	fp_h264_size = fopen("./stream_h264_size.txt", "r");
	if(fp_h264_size) {
		fp_h264_pts = fopen("./stream_h264_pts.txt", "r");
		if(fp_h264_pts) {
			fp_h264_stream = fopen("./stream.h264", "r");
			if(fp_h264_stream) {
				char size[128] = {'\0'};
				while(pAvDecStream->decode_start && fgets(size, 128, fp_h264_size) != NULL) {
					char pts[128] = {'\0'};
					fgets(pts, 128, fp_h264_pts);

					int32_t size_v = atoi(size);
					int64_t pts_v = atoi(pts);
					//printf("size = %d, pts = %ld\n", size_v, pts_v);
	
					int r = fread(pkg_buff, 1, size_v, fp_h264_stream);
					av_parse_packet(pAvDecStream, pkg_buff, size_v, pts_v);
				}
			}
			else {
				printf("Open stream.h264 fail!\n");
			}
		}
		else {
			printf("Open stream_h264_pts.txt fail!\n");
		}
	}
	else {
		printf("Open stream_h264_size.txt fail!\n");
	}


	fclose(fp_h264_pts);
	fclose(fp_h264_size);
	fclose(fp_h264_stream);
	if(pkg_buff) {
		free(pkg_buff);
		pkg_buff = NULL;
	}
	pAvDecStream->decode_start = EN_FALSE;
}

int main()
{
	stAvDecStream stream;
	av_dec_init(&stream);
	stream.decode_start = EN_TRUE;
	stream.usr_data = NULL;
	
	av_loop_dec_file(&stream);

AV_DEC_FIN:
	av_dec_deinit(&stream);
	return 0;
}
