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
#define AV_FRAME_BUFF_NUM	(2)

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

////////////////////////////////////////////////
#define FRAME_BUFF_OBJ_SIZE	 			(sizeof(stFrameBuffObj))
#define MAX_FRAME_BUFF_OBJ_NUM	 		(AV_FRAME_BUFF_NUM * 2) // 必须比实际的Object多
#define MAX_FRAME_QUEUE_NUM  			(1)
#define TOTAL_FRAME_BUFF_OBJ_NUM        (MAX_FRAME_QUEUE_NUM * MAX_FRAME_BUFF_OBJ_NUM)

#define CHECK_POINTER_NULL(pointer, ret) \
    do{\
        if(pointer == NULL){ \
            printf("Check '%s' is NULL at:%u\n", #pointer, __LINE__);\
            return ret; \
        }\
    }while(0)

#define CHECK_POINTER_VALID(P) \
	do{ \
		if(NULL == P){ \
			printf("Pointer: %s is NULL!", #P); \
			return; \
		} \
	}while(0)

#define SAFETY_MUTEX_LOCK(mutex) \
do{ \
	pthread_cleanup_push(pthread_mutex_unlock, &(mutex)); \
	pthread_mutex_lock(&(mutex))
						
#define SAFETY_MUTEX_UNLOCK(mutex) \
	pthread_mutex_unlock(&mutex); \
	pthread_cleanup_pop(0); \
}while(0)


typedef enum
{
	AVAIL_STATUS_FREE,
	AVAIL_STATUS_USED
}enAvailableStatus;

typedef enum
{
	LOOP_QUEUE_NORMAL,
	LOOP_QUEUE_FULL,
	LOOP_QUEUE_EMPTY
}enLoopQueueStatus;

typedef struct
{
	void *pAddr;
	struct stFrameBuffListNode *pNext;
}stFrameBuffListNode;

typedef struct
{
	stFrameBuffListNode *pFreeList;
	stFrameBuffListNode *pUsedList;
	stFrameBuffListNode *pFreeListTail; // the tail pointer of free list
	volatile int freeCnt;
	pthread_mutex_t mutex;
}stFrameBuffListMng;

typedef struct
{
	volatile int readIdx;
	volatile int writeIdx;
	void** ppQueue;
	int maxCnt;
}stLoopQueue;

typedef struct{
	enAvailableStatus state;
	stLoopQueue *pLoopQueue;
	pthread_mutex_t mutex;
	pthread_cond_t  pushCond;
	pthread_cond_t  popCond;
}stFrameBuffQueueMng;

static void *pFrameBuffObjs = NULL;  // define for free memery
static stFrameBuffListNode *pFrameBuffList = NULL; // define for free memery
static stFrameBuffListMng s_frame_buff_list_mng;

static stFrameBuffQueueMng s_frame_buff_queue_mngs[MAX_FRAME_QUEUE_NUM];
static pthread_mutex_t s_frame_buff_queue_mutex = PTHREAD_MUTEX_INITIALIZER;

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

static stLoopQueue* CreateLoopQueue(int queueMaxCnt)
{
	if(queueMaxCnt <= 0) return NULL;
	stLoopQueue *pLoopQueue = (stLoopQueue*)malloc(sizeof(stLoopQueue));
	if(pLoopQueue){
		pLoopQueue->readIdx = 0;
		pLoopQueue->writeIdx = 0;
		pLoopQueue->ppQueue = (void**)malloc(sizeof(void*) * (queueMaxCnt + 1));
		pLoopQueue->maxCnt = queueMaxCnt + 1;
	}
	return pLoopQueue;
}

static void DeleteLoopQueue(stLoopQueue *pLoopQueue)
{
	if(pLoopQueue){
		free(pLoopQueue->ppQueue);
		free(pLoopQueue);
		pLoopQueue = NULL;
	}
	return;
}

static void calcLoopQueueNextIdx(volatile int *pIndex, int maxCnt)
{
	CHECK_POINTER_VALID(pIndex);
	int newIndex = __sync_add_and_fetch(pIndex, 1);
	if (newIndex >= maxCnt){
		/*bool __sync_bool_compare_and_swap (type *ptr, type oldval type newval, ...)
		   if *ptr == oldval, use newval to update *ptr value */
		__sync_bool_compare_and_swap(pIndex, newIndex, newIndex % maxCnt);
	}
	return;
}

static enBool checkLoopQueueEmpty(stLoopQueue *pLoopQueue)
{
	CHECK_POINTER_NULL(pLoopQueue, EN_FALSE);
	return (pLoopQueue->readIdx == pLoopQueue->writeIdx) ? EN_TRUE : EN_FALSE;
}

static enBool checkLoopQueueFull(stLoopQueue *pLoopQueue)
{
	CHECK_POINTER_NULL(pLoopQueue, EN_FALSE);
	return ((pLoopQueue->writeIdx+1)%pLoopQueue->maxCnt== pLoopQueue->readIdx) ? EN_TRUE : EN_FALSE;
}

static enLoopQueueStatus checkLoopQueueStatus(stLoopQueue *pLoopQueue)
{
	CHECK_POINTER_NULL(pLoopQueue, LOOP_QUEUE_EMPTY);
	if(pLoopQueue->readIdx == pLoopQueue->writeIdx) {
		return LOOP_QUEUE_EMPTY;
	}
	else if(((pLoopQueue->writeIdx+1) % pLoopQueue->maxCnt) == pLoopQueue->readIdx) {
		return LOOP_QUEUE_FULL;
	}
	else {
		return LOOP_QUEUE_NORMAL;
	}
}

static int getLoopQueueNums(stLoopQueue *pLoopQueue)
{
	CHECK_POINTER_NULL(pLoopQueue, 0);
	return (pLoopQueue->writeIdx - pLoopQueue->readIdx + pLoopQueue->maxCnt) % pLoopQueue->maxCnt;
}

static enBool WriteLoopQueue(stLoopQueue *pLoopQueue, void **ppIn)
{
	CHECK_POINTER_NULL(pLoopQueue, EN_FALSE);
	CHECK_POINTER_NULL(ppIn, EN_FALSE);
	if(checkLoopQueueFull(pLoopQueue)){
		printf("loop queue have not enough space to write, readIdx=%d,writeIdx=%d\n",
			pLoopQueue->readIdx,pLoopQueue->writeIdx);
		return EN_FALSE;
	}

	pLoopQueue->ppQueue[pLoopQueue->writeIdx] = *ppIn;
	printf("write[%d]: 0x%x\n", pLoopQueue->writeIdx, *ppIn);
	calcLoopQueueNextIdx(&pLoopQueue->writeIdx, pLoopQueue->maxCnt);
	return EN_TRUE;
}

static enBool ReadLoopQueue(stLoopQueue *pLoopQueue, void **ppOut)
{
	CHECK_POINTER_NULL(pLoopQueue, EN_FALSE);
	CHECK_POINTER_NULL(ppOut, EN_FALSE);
	if(checkLoopQueueEmpty(pLoopQueue)){
		//printf("Loop queue is empty!!!\n");
		return EN_FALSE;
	}
	*ppOut = pLoopQueue->ppQueue[pLoopQueue->readIdx];
	printf("read[%d]: 0x%x\n", pLoopQueue->readIdx, *ppOut);
	calcLoopQueueNextIdx(&pLoopQueue->readIdx, pLoopQueue->maxCnt);
	return EN_TRUE;
}


static enBool frame_buff_list_create()
{
	if(pFrameBuffObjs) return EN_TRUE;
	pFrameBuffObjs = (void*)malloc(TOTAL_FRAME_BUFF_OBJ_NUM * FRAME_BUFF_OBJ_SIZE);
	if(!pFrameBuffObjs) return EN_FALSE;
	
	pFrameBuffList = (stFrameBuffListNode*)malloc(sizeof(stFrameBuffListNode) * TOTAL_FRAME_BUFF_OBJ_NUM);
	if(!pFrameBuffList){
		free(pFrameBuffObjs);
		pFrameBuffObjs = NULL;
		return EN_FALSE;
	}

	unsigned char *pAddr = (unsigned char*)pFrameBuffObjs;
	stFrameBuffListNode *pNode = pFrameBuffList;
	
	s_frame_buff_list_mng.freeCnt = TOTAL_FRAME_BUFF_OBJ_NUM;
	s_frame_buff_list_mng.pFreeList = pNode;
	s_frame_buff_list_mng.pFreeList->pAddr = pAddr;
	s_frame_buff_list_mng.pFreeList->pNext = NULL;

	pAddr += FRAME_BUFF_OBJ_SIZE;
	pNode++;
	stFrameBuffListNode* pCur = s_frame_buff_list_mng.pFreeList;
	int i = 0;
	for(; i < TOTAL_FRAME_BUFF_OBJ_NUM - 1; i++){
		stFrameBuffListNode* pNew = pNode;
		pNew->pAddr = pAddr;
		pNew->pNext = NULL;
		pAddr += FRAME_BUFF_OBJ_SIZE;
		pNode++;
		pCur->pNext = pNew;
		pCur = pNew;
	}
	s_frame_buff_list_mng.pUsedList = NULL;
	s_frame_buff_list_mng.pFreeListTail = pCur;
	
	pthread_create_mutex(&s_frame_buff_list_mng.mutex);

	printf("Frame buffer list create success, create %d frame buffers\n", TOTAL_FRAME_BUFF_OBJ_NUM);
	return EN_TRUE;
}


static void frame_buff_list_destory()
{
	if(pFrameBuffObjs){
		free(pFrameBuffObjs);
		pFrameBuffObjs = NULL;
	}

	if(pFrameBuffList){
		free(pFrameBuffList);
		pFrameBuffList = NULL;
	}
	return;
}

int get_frame_buff(void **ppFrameBuff)
{
	int ret = 0;
	SAFETY_MUTEX_LOCK(s_frame_buff_list_mng.mutex);
	CHECK_POINTER_NULL(ppFrameBuff, -1);
	printf("s_frame_buff_list_mng.freeCnt = %d\n", s_frame_buff_list_mng.freeCnt);
	if(s_frame_buff_list_mng.freeCnt <= 0){
		printf("All the msg buffer has been used!!!\n");
		ret = -1;
	}
	else{
		*ppFrameBuff = s_frame_buff_list_mng.pFreeList->pAddr;
		stFrameBuffListNode *pUsed = s_frame_buff_list_mng.pFreeList;
		s_frame_buff_list_mng.pFreeList = s_frame_buff_list_mng.pFreeList->pNext;
		s_frame_buff_list_mng.freeCnt--;
		if(!s_frame_buff_list_mng.pUsedList){
			pUsed->pNext = NULL;
			pUsed->pAddr = NULL;
			s_frame_buff_list_mng.pUsedList = pUsed;
		}
		else{
			pUsed->pAddr = NULL;
			pUsed->pNext = s_frame_buff_list_mng.pUsedList->pNext;
			s_frame_buff_list_mng.pUsedList->pNext = pUsed;
		}
	}
	SAFETY_MUTEX_UNLOCK(s_frame_buff_list_mng.mutex);
	printf("Get Buffer : 0x%x\n", *ppFrameBuff);
	return ret;
}

void free_frame_buff(void *pFrameBuff)
{
	SAFETY_MUTEX_LOCK(s_frame_buff_list_mng.mutex);
	stFrameBuffListNode *pFree = s_frame_buff_list_mng.pUsedList;
	s_frame_buff_list_mng.pUsedList = s_frame_buff_list_mng.pUsedList->pNext;
	pFree->pAddr = pFrameBuff;
	pFree->pNext = NULL;
	if(!s_frame_buff_list_mng.pFreeList){
		s_frame_buff_list_mng.pFreeList = pFree;
		s_frame_buff_list_mng.pFreeListTail = s_frame_buff_list_mng.pFreeList;
		s_frame_buff_list_mng.pFreeListTail->pNext = NULL;
	}
	else{
		#if 1
		/* add to the free list tail */
		s_frame_buff_list_mng.pFreeListTail->pNext = pFree;
		s_frame_buff_list_mng.pFreeListTail = pFree;
		#else
		/* add to the free list head */
		pFree->pNext = s_frame_buff_list_mng.pFreeList->pNext;
		s_frame_buff_list_mng.pFreeList->pNext = pFree;
		#endif
	}
	s_frame_buff_list_mng.freeCnt++;
	SAFETY_MUTEX_UNLOCK(s_frame_buff_list_mng.mutex);
	printf("Free Buffer : 0x%x\n", pFrameBuff);
	return;
}

int queue_all_init()
{
	if(frame_buff_list_create() == EN_FALSE) { 
		printf("frame_buff_list_create fail\n");
		return -1;
	}
	int i = 0;
	for(i = 0; i < MAX_FRAME_QUEUE_NUM; i++){
		s_frame_buff_queue_mngs[i].state = AVAIL_STATUS_FREE;
		s_frame_buff_queue_mngs[i].pLoopQueue = NULL;
	}
	printf("queue init success, queue max num: %d\n", MAX_FRAME_QUEUE_NUM);
	return 0;
}

void queue_all_destory()
{
	frame_buff_list_destory();

	int i = 0;
	for(i = 0; i < MAX_FRAME_QUEUE_NUM; i++) {
		if(s_frame_buff_queue_mngs[i].state != AVAIL_STATUS_FREE) {
			s_frame_buff_queue_mngs[i].state = AVAIL_STATUS_FREE;
			DeleteLoopQueue(s_frame_buff_queue_mngs[i].pLoopQueue);
			pthread_cond_destroy(&s_frame_buff_queue_mngs[i].pushCond);
			pthread_cond_destroy(&s_frame_buff_queue_mngs[i].popCond);
		}
	}
}

int queue_create(int *pQueueId, int queueMaxCnt)
{
	int ret = 0;
	int queueId = 0;
	SAFETY_MUTEX_LOCK(s_frame_buff_queue_mutex);
	for(; queueId < MAX_FRAME_QUEUE_NUM; queueId++) {
		if(s_frame_buff_queue_mngs[queueId].state == AVAIL_STATUS_FREE) break;
	}
	
	if(queueId >= MAX_FRAME_QUEUE_NUM) {
		printf("Sorry all queue has been used, create queue FAIL!!!\n");
		*pQueueId = -1;
		ret = -1;
	}
	else{
		pthread_condattr_t attrCond;
		s_frame_buff_queue_mngs[queueId].state = AVAIL_STATUS_USED;
		pthread_mutex_init(&s_frame_buff_queue_mngs[queueId].mutex, NULL);
		pthread_condattr_init(&attrCond);
		pthread_condattr_setclock(&attrCond, CLOCK_MONOTONIC);
		pthread_cond_init(&s_frame_buff_queue_mngs[queueId].pushCond, &attrCond);
		pthread_cond_init(&s_frame_buff_queue_mngs[queueId].popCond, &attrCond);
		queueMaxCnt = (queueMaxCnt > TOTAL_FRAME_BUFF_OBJ_NUM) ? TOTAL_FRAME_BUFF_OBJ_NUM : queueMaxCnt;
		s_frame_buff_queue_mngs[queueId].pLoopQueue = CreateLoopQueue(queueMaxCnt);
		*pQueueId = queueId;
	}
	SAFETY_MUTEX_UNLOCK(s_frame_buff_queue_mutex);
	return ((ret != -1) && s_frame_buff_queue_mngs[queueId].pLoopQueue) ? 0 : -1;
}

int queue_destory(int queueId)
{
	int ret = 0;
	SAFETY_MUTEX_LOCK(s_frame_buff_queue_mutex);
	if(queueId < 0 || queueId >= MAX_FRAME_QUEUE_NUM || 
		s_frame_buff_queue_mngs[queueId].state != AVAIL_STATUS_USED){
		ret = -1;
	}
	else{
		s_frame_buff_queue_mngs[queueId].state = AVAIL_STATUS_FREE;
		DeleteLoopQueue(s_frame_buff_queue_mngs[queueId].pLoopQueue);
		pthread_cond_destroy(&s_frame_buff_queue_mngs[queueId].pushCond);
		pthread_cond_destroy(&s_frame_buff_queue_mngs[queueId].popCond);
	}
	SAFETY_MUTEX_UNLOCK(s_frame_buff_queue_mutex);
	return ret;
}

/* 	function: push frame buff objest pointer into queue
	timeOutMs :   -1  waitforever
			     >0  wait timeOutMs ms
*/
int queue_push_frame_buff(int queueId, void *pFbObj, int timeOutMs)
{
	CHECK_POINTER_NULL(pFbObj, -1);
	int ret = 0;
	if((queueId < 0) || (queueId >= MAX_FRAME_QUEUE_NUM)) {
		printf("queue ID must in the range: [%d -  %d], current ID: %d", 0, MAX_FRAME_QUEUE_NUM-1, queueId);
		ret = -1;
	}
	else if(s_frame_buff_queue_mngs[queueId].state == AVAIL_STATUS_FREE) {
		printf("current queue is not be created\n");
		ret = -1;	
	}
	else {
		int os_result = 0;
		SAFETY_MUTEX_LOCK(s_frame_buff_queue_mngs[queueId].mutex);
		while(WriteLoopQueue(s_frame_buff_queue_mngs[queueId].pLoopQueue, &pFbObj) == EN_FALSE){
			/* if loop queue is full */
			if(timeOutMs < 0){
				os_result = pthread_cond_wait(&s_frame_buff_queue_mngs[queueId].pushCond, &s_frame_buff_queue_mngs[queueId].mutex);
			}
			else{
				struct timespec tv;
				clock_gettime(CLOCK_MONOTONIC, &tv);
				tv.tv_nsec += (timeOutMs%1000)*1000*1000;
				tv.tv_sec += tv.tv_nsec/(1000*1000*1000) + timeOutMs/1000;
				tv.tv_nsec = tv.tv_nsec%(1000*1000*1000);
				os_result = pthread_cond_timedwait(&s_frame_buff_queue_mngs[queueId].pushCond, &s_frame_buff_queue_mngs[queueId].mutex, &tv);
			}

			if(os_result == ETIMEDOUT){
				printf("queue_push_frame_buff time out!\n");
				ret = -1;
				break;
			}
		}
		SAFETY_MUTEX_UNLOCK(s_frame_buff_queue_mngs[queueId].mutex);
		pthread_cond_signal(&s_frame_buff_queue_mngs[queueId].popCond);
	}
	return ret;
}

/* timeOutMs :   -1  waitforever
			     >0  wait timeOutMs ms
*/
int queue_pop_frame_buff(int queueId, void **ppFbMem, int timeOutMs)
{
	int ret = 0;
	if((queueId < 0) || (queueId >= MAX_FRAME_QUEUE_NUM)) {
		printf("queue ID must in the range: [%d -  %d], current ID: %d", 0, MAX_FRAME_QUEUE_NUM-1, queueId);
		ret = -1;
	}
	else if(s_frame_buff_queue_mngs[queueId].state == AVAIL_STATUS_FREE) {
		printf("current queue is not be created\n");
		ret = -1;	
	}
	else {
		int os_result = 0;
		SAFETY_MUTEX_LOCK(s_frame_buff_queue_mngs[queueId].mutex);
		while(ReadLoopQueue(s_frame_buff_queue_mngs[queueId].pLoopQueue, ppFbMem) == EN_FALSE){
			/* if loop queue is empty */
			//MAILBOX_LOG_WARNING("Receive from Du FB Queue waiting.....");
			if(timeOutMs == -1){
				os_result = pthread_cond_wait(&s_frame_buff_queue_mngs[queueId].popCond, &s_frame_buff_queue_mngs[queueId].mutex);
			}
			else{
				struct timespec tv;
				clock_gettime(CLOCK_MONOTONIC, &tv);
				tv.tv_nsec += (timeOutMs%1000)*1000*1000;
				tv.tv_sec += tv.tv_nsec/(1000*1000*1000) + timeOutMs/1000;
				tv.tv_nsec = tv.tv_nsec%(1000*1000*1000);
				os_result = pthread_cond_timedwait(&s_frame_buff_queue_mngs[queueId].popCond, &s_frame_buff_queue_mngs[queueId].mutex, &tv);
			}
			//MAILBOX_LOG_WARNING("Receive from Du FB Queue wait Over.....");

			if(os_result == ETIMEDOUT){
				printf("queue_pop_frame_buff time out!\n");
				ret = -1;
				break;
			}
		}
		SAFETY_MUTEX_UNLOCK(s_frame_buff_queue_mngs[queueId].mutex);
		pthread_cond_signal(&s_frame_buff_queue_mngs[queueId].pushCond);
	}
	return ret;
}


/////////////////////////////////////////////////////////////


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

int sdl2_show(stSDL2* sdl_info, stFrameBuffObj *frame)
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
	if(pAvDecStream->decode_start == EN_FALSE) return 0;
	stFrameBuffObj *pFbObj = NULL;
	if(get_frame_buff((void**)&pFbObj) != -1) {
		
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


			pFbObj->Width = pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->width;
			pFbObj->Height = pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->height;
			pFbObj->YUV_Data[0] = pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->data[0];
			pFbObj->YUV_Data[1] = pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->data[1];
			pFbObj->YUV_Data[2] = pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->data[2];
			pFbObj->YUV_LineSize[0] = pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->linesize[0];
			pFbObj->YUV_LineSize[1] = pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->linesize[1];
			pFbObj->YUV_LineSize[2] = pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->linesize[2];
			pFbObj->Frame_Num = pAvDecStream->codec_ctx->frame_number;
			queue_push_frame_buff(pAvDecStream->queue_id, (void*)pFbObj, -1);
			if(pAvDecStream->frame_handle_cb) {
				pAvDecStream->frame_handle_cb(pAvDecStream->usr_data);
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

			pFbObj->Width = pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->width;
			pFbObj->Height = pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->height;
			pFbObj->YUV_Data[0] = pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->data[0];
			pFbObj->YUV_Data[1] = pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->data[1];
			pFbObj->YUV_Data[2] = pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->data[2];
			pFbObj->YUV_LineSize[0] = pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->linesize[0];
			pFbObj->YUV_LineSize[1] = pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->linesize[1];
			pFbObj->YUV_LineSize[2] = pAvDecStream->decoding_frame[pAvDecStream->current_decodeing_frame]->linesize[2];
			pFbObj->Frame_Num = pAvDecStream->codec_ctx->frame_number;
			queue_push_frame_buff(pAvDecStream->queue_id, (void*)pFbObj, -1);
			if(pAvDecStream->frame_handle_cb) {
				pAvDecStream->frame_handle_cb(pAvDecStream->usr_data);
			}
			pAvDecStream->current_decodeing_frame ^= 1;
	    }
		#endif
	}
	else {
		printf("get_frame_buff Fail, so this packet be discarded\n");
		return -1;
	}
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

	 queue_all_init();
	 // 队列可以存放两个元素
	 queue_create(&pAvDecStream->queue_id, 2);
}

void av_dec_deinit(stAvDecStream *pAvDecStream)
{
	av_parser_close(pAvDecStream->parser);
	avcodec_free_context(&pAvDecStream->codec_ctx);
    av_frame_free(&pAvDecStream->decoding_frame[0]);
	av_frame_free(&pAvDecStream->decoding_frame[1]);
	queue_all_destory();
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

int sdl_event_send(void* user_data)
{
	static SDL_Event new_frame_event = {
        .type = EVENT_NEW_FRAME,
    };

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
	while(pAvDecStream->decode_start) {
    	SDL_WaitEvent(&event);
    	switch (event.type) {
        	case SDL_QUIT://退出事件
    		{
            	SDL_Log("quit");
           		pAvDecStream->decode_start = EN_FALSE;
				stFrameBuffObj *pFbObj = NULL;
				if(queue_pop_frame_buff(pAvDecStream->queue_id, (void**)&pFbObj, -1) != -1) {
					if(pFbObj) {
						free_frame_buff((void*)pFbObj);
					}
				}
            }break;
			case EVENT_NEW_FRAME:
			{
				stFrameBuffObj *pFbObj = NULL;
				if(queue_pop_frame_buff(pAvDecStream->queue_id, (void**)&pFbObj, -1) != -1) {
					if(pFbObj) {
						sdl2_show(sdl_info, pFbObj);
						free_frame_buff((void*)pFbObj);
					}
				}
			}break;
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
	stream.decode_start = EN_TRUE;

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
	stream.usr_data = NULL;
	
	av_loop_dec_file(&stream);

SDL_FIN:
	sdl2_deinit(&sdl2_obj);

AV_DEC_FIN:
	av_dec_deinit(&stream);
	return 0;
}
