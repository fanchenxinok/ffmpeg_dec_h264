#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <libavformat/avformat.h>
#include <libavutil/time.h>
#include <SDL2/SDL.h>

// In ffmpeg/doc/APIchanges:
// 2016-04-21 - 7fc329e - lavc 57.37.100 - avcodec.h
//   Add a new audio/video encoding and decoding API with decoupled input
//   and output -- avcodec_send_packet(), avcodec_receive_frame(),
//   avcodec_send_frame() and avcodec_receive_packet().
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 37, 100)
# define SCRCPY_LAVF_HAS_NEW_ENCODING_DECODING_API
#endif

#define DUMP_DEC_DATA	(0)
#define EVENT_NEW_FRAME (SDL_USEREVENT + 1)

typedef void (*frame_back_t)(void *frame);

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
	enBool*   Frame_Free;
	pthread_mutex_t* Frame_lock;
	pthread_cond_t* Frame_cond;
	frame_back_t Frame_back;
}stFrameBuff;

typedef int (*frame_handle_callback)(void* usr_data, stFrameBuff *frame);

typedef struct
{
	AVCodecContext *codec_ctx;
    AVCodecParserContext *parser;
	
	AVFrame *decoding_frame[2];
	enBool decodeing_frame_free[2];  //标记frame是否被显示并返回
	pthread_mutex_t decodeing_frame_lock[2];
	pthread_cond_t decodeing_frame_cond[2];
	int current_decodeing_frame;
	frame_handle_callback frame_handle_cb;
	
	void *usr_data;
	enBool dec_start;
}stAvDecStream;

typedef struct
{
	int width;
	int height;
	int format;
	SDL_Window* window;
	SDL_Renderer* render;
	SDL_Texture* texture;
	SDL_Thread* event_thread;
	void* usr_data;
}stSDL2;

static void pthread_create_mutex(pthread_mutex_t *mutex)
{
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr , PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(mutex, &attr);
	pthread_mutexattr_destroy(&attr);
}

static void pthread_create_cond(pthread_cond_t *cond)
{
	pthread_condattr_t attrCond;
	pthread_condattr_init(&attrCond);
	pthread_condattr_setclock(&attrCond, CLOCK_MONOTONIC);
	pthread_cond_init(cond, &attrCond);	
}

int sdl2_init(stSDL2 *pSdl2)
{
	pSdl2->window = SDL_CreateWindow("sdl_win", 
									SDL_WINDOWPOS_UNDEFINED,
									SDL_WINDOWPOS_UNDEFINED,
									pSdl2->width,
									pSdl2->height,
									SDL_WINDOW_SHOWN);
	if(pSdl2->window == NULL) {
		printf("sdl2 create window fail!\n");
		return -1;
	}

	pSdl2->render = SDL_CreateRenderer(pSdl2->window, -1, 0);
	if (pSdl2->render == NULL) {
		printf("sdl2 create render error\n");
		return -1;
	}

	pSdl2->texture = SDL_CreateTexture(pSdl2->render, pSdl2->format, SDL_TEXTUREACCESS_STREAMING, pSdl2->width, pSdl2->height);
	if (pSdl2->texture == NULL) {
		printf("sdl2 create texture error\n");
		return -1;
	}

	return 0;
}

void sdl2_deinit(stSDL2 *pSdl2)
{
	SDL_DestroyWindow(pSdl2->window);
	SDL_DestroyRenderer(pSdl2->render);
	SDL_DestroyTexture(pSdl2->texture);
	SDL_Quit();
}

void sdl2_render(stSDL2 *pSdl2)
{
	SDL_RenderCopy(pSdl2->render, pSdl2->texture, NULL, NULL);
	SDL_RenderPresent(pSdl2->render);
}

int sdl2_show(stSDL2* sdl_info, stFrameBuff *frame)
{
	int ret = -1;
	static int first = 1;

	if(first) {
		SDL_ShowWindow(sdl_info->window);
		first = 0;
	}
	
	SDL_UpdateYUVTexture(sdl_info->texture, NULL,
            frame->YUV_Data[0], frame->YUV_LineSize[0],
            frame->YUV_Data[1], frame->YUV_LineSize[1],
            frame->YUV_Data[2], frame->YUV_LineSize[2]);

	sdl2_render(sdl_info);
	printf("Render: wxh = %d x %d, Frame num = %d\n", 
		frame->Width, frame->Height, frame->Frame_Num);
	return 0;
}

static void frame_wait(stFrameBuff *frame)
{
	pthread_mutex_lock(frame->Frame_lock);
	while(*(frame->Frame_Free) != EN_TRUE) {
		printf("Frame wait.....\n");
		pthread_cond_wait(frame->Frame_cond, frame->Frame_lock);
		printf("Frame get.....\n");
	}
	pthread_mutex_unlock(frame->Frame_lock);
}

// frame is the stFrameBuff pointer
static void frame_back(void *frame)
{
	stFrameBuff *pFrame = (stFrameBuff*)frame;
	pthread_mutex_lock(pFrame->Frame_lock);
	*(pFrame->Frame_Free) = EN_TRUE;
	pthread_mutex_unlock(pFrame->Frame_lock);
	pthread_cond_signal(pFrame->Frame_cond);
}

static void dump_nv12_data(AVFrame *frame, int frame_number)
{
	#if DUMP_DEC_DATA
	if(frame_number > 10) return;
	FILE *pf = NULL;
	char file[64] = {'\0'};
	sprintf(file, "./output/frame%04d.NV12", frame_number);
	pf = fopen(file, "wb");

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
	}
	#endif
}

static int decode_packet(stAvDecStream *pAvDecStream, const AVPacket *packet) {

	static stFrameBuff frameBuff;
	frameBuff.Frame_Free = &pAvDecStream->decodeing_frame_free[pAvDecStream->current_decodeing_frame];
	frameBuff.Frame_lock = &pAvDecStream->decodeing_frame_lock[pAvDecStream->current_decodeing_frame];
	frameBuff.Frame_cond = &pAvDecStream->decodeing_frame_cond[pAvDecStream->current_decodeing_frame];
	frameBuff.Frame_back = frame_back;
	/* check the current frame buff is free, if not wait... */
	frame_wait(&frameBuff);
	
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

		if(pAvDecStream->frame_handle_cb) {
			frameBuff.Width = pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->width;
			frameBuff.Height = pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->height;
			frameBuff.YUV_Data[0] = pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->data[0];
			frameBuff.YUV_Data[1] = pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->data[1];
			frameBuff.YUV_Data[2] = pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->data[2];
			frameBuff.YUV_LineSize[0] = pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->linesize[0];
			frameBuff.YUV_LineSize[1] = pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->linesize[1];
			frameBuff.YUV_LineSize[2] = pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->linesize[2];
			frameBuff.Frame_Num = pAvDecStream->codec_ctx->frame_number;
			*(frameBuff.Frame_Free) = EN_FALSE;
			pAvDecStream->frame_handle_cb(pAvDecStream->usr_data, &frameBuff);
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

		if(pAvDecStream->frame_handle_cb) {
			frameBuff.Width = pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->width;
			frameBuff.Height = pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->height;
			frameBuff.YUV_Data[0] = pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->data[0];
			frameBuff.YUV_Data[1] = pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->data[1];
			frameBuff.YUV_Data[2] = pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->data[2];
			frameBuff.YUV_LineSize[0] = pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->linesize[0];
			frameBuff.YUV_LineSize[1] = pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->linesize[1];
			frameBuff.YUV_LineSize[2] = pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->linesize[2];
			frameBuff.Frame_Num = pAvDecStream->codec_ctx->frame_number;
			*(frameBuff.Frame_Free) = EN_FALSE;
			pAvDecStream->frame_handle_cb(pAvDecStream->usr_data, &frameBuff);
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
	 pAvDecStream->decodeing_frame_free[0] = EN_TRUE;
	 pAvDecStream->decodeing_frame_free[1] = EN_TRUE;
	 pAvDecStream->current_decodeing_frame = 0;

	 pthread_create_mutex(&pAvDecStream->decodeing_frame_lock[0]);
	 pthread_create_cond(&pAvDecStream->decodeing_frame_cond[0]);
	 pthread_create_mutex(&pAvDecStream->decodeing_frame_lock[1]);
	 pthread_create_cond(&pAvDecStream->decodeing_frame_cond[1]);
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
				while(pAvDecStream->dec_start && fgets(size, 128, fp_h264_size) != NULL) {
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
	pAvDecStream->dec_start = EN_FALSE;
}


static stFrameBuff *s_frame_buff;

int sdl_event_send(void* user_data, stFrameBuff *frame)
{
	static SDL_Event new_frame_event = {
        .type = EVENT_NEW_FRAME,
    };

	s_frame_buff = frame;
    SDL_PushEvent(&new_frame_event);
}

/*
SDL_WindowEvent : Window窗口相关的事件。
SDL_KeyboardEvent : 键盘相关的事件。
SDL_MouseMotionEvent : 鼠标移动相关的事件。
SDL_QuitEvent : 退出事件。
SDL_UserEvent : 用户自定义事件。
*/
static int sdl_event_handle(void* usr_data)
{
	SDL_Event event;
	stSDL2* sdl_info = (stSDL2*)usr_data;
	stAvDecStream *pAvDecStream = (stAvDecStream*)(sdl_info->usr_data);
	while(pAvDecStream->dec_start) {
    	SDL_WaitEvent(&event);
    	switch (event.type) {
        	case SDL_QUIT://退出事件
            	SDL_Log("quit");
           		pAvDecStream->dec_start = EN_FALSE;
                break;
			case EVENT_NEW_FRAME:
				//printf("handle frame: %d\n", pAvDecStream->current_decodeing_frame^1);
				sdl2_show(sdl_info, s_frame_buff);
				s_frame_buff->Frame_back(s_frame_buff);
				break;
        	default:
            	SDL_Log("event type:%d", event.type);
				break;
    	}
	}
	
	return 0;
}


int main()
{
	stAvDecStream stream;
	av_dec_init(&stream);
	stream.dec_start = EN_TRUE;

	//初始化SDL2
	stSDL2 sdl2_obj;
	sdl2_obj.width = 1080;
	sdl2_obj.height = 1920;
	//sdl2_obj.format = SDL_PIXELFORMAT_RGBA8888;
	//sdl2_obj.format = SDL_PIXELFORMAT_YUY2;
	//sdl2_obj.format = SDL_PIXELFORMAT_NV12;
	sdl2_obj.format = SDL_PIXELFORMAT_YV12;
	if(sdl2_init(&sdl2_obj) < 0) {
		printf("sdl2_init fail\n");
		goto AV_DEC_FIN;
	}

	sdl2_obj.usr_data = (void*)&stream;
	sdl2_obj.event_thread = SDL_CreateThread(sdl_event_handle, "eventHdl", (void *)&sdl2_obj);

	stream.frame_handle_cb = sdl_event_send;
	stream.usr_data = (void*)&sdl2_obj;
	
	av_loop_dec_file(&stream);

SDL_FIN:
	sdl2_deinit(&sdl2_obj);

AV_DEC_FIN:
	av_dec_deinit(&stream);
	return 0;
}
