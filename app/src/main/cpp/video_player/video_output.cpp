#include "video_output.h"

#define LOG_TAG "VideoOutput"

VideoOutput::VideoOutput() {
	renderer = NULL;
	handler = NULL;
	queue = NULL;
	surfaceWindow = NULL;
	forceGetFrame = false;
	surfaceExists = false;
	eglCore = NULL;
	isANativeWindowValid = false;
	renderTexSurface = EGL_NO_SURFACE;

	eglHasDestroyed = false;
}

VideoOutput::~VideoOutput() {
}

/** 初始化Output **/
bool VideoOutput::initOutput(ANativeWindow* window, int screenWidth, int screenHeight, getTextureCallback produceDataCallback, void* ctx) {
	LOGI("VideoOutput::initOutput");
	this->ctx = ctx;
	this->produceDataCallback = produceDataCallback;
	this->screenWidth = screenWidth;
	this->screenHeight = screenHeight;
	if(NULL != window){
		isANativeWindowValid = true;
	}

	queue = new MessageQueue("video output message queue");
	handler = new VideoOuputHandler(this, queue);

	handler->postMessage(new Message(VIDEO_OUTPUT_MESSAGE_CREATE_EGL_CONTEXT, window));
	pthread_create(&_threadId, 0, threadStartCallback, this);
	return true;
}
bool VideoOutput::createEGLContext(ANativeWindow* window) {
	LOGI("enter VideoOutput::createEGLContext");
	eglCore = new EGLCore();
	LOGI("enter VideoOutput use sharecontext");
	bool ret = eglCore->initWithSharedContext();
	if(!ret){
		LOGI("create EGL Context failed...");
		return false;
	}
	this->createWindowSurface(window);
	eglCore->doneCurrent();	// must do this before share context in Huawei p6, or will crash
	return ret;
}

void* VideoOutput::threadStartCallback(void *myself) {
	VideoOutput *output = (VideoOutput*) myself;
	output->processMessage();
	pthread_exit(0);
	return 0;
}

void VideoOutput::processMessage() {
	bool renderingEnabled = true;
	while (renderingEnabled) {
		Message* msg = NULL;
		if(queue->dequeueMessage(&msg, true) > 0){
//			LOGI("msg what is %d", msg->getWhat());
		    if(MESSAGE_QUEUE_LOOP_QUIT_FLAG == msg->execute()){
		    		renderingEnabled = false;
		    }
		    delete msg;
		}
	}
}

/** 当surface创建的时候的调用 **/
void VideoOutput::onSurfaceCreated(ANativeWindow* window) {
	LOGI("enter VideoOutput::onSurfaceCreated");
	if (handler) {
		isANativeWindowValid = true;
		handler->postMessage(new Message(VIDEO_OUTPUT_MESSAGE_CREATE_WINDOW_SURFACE, window));
		handler->postMessage(new Message(VIDEO_OUTPUT_MESSAGE_RENDER_FRAME));
	}
}
void VideoOutput::createWindowSurface(ANativeWindow* window) {
	LOGI("enter VideoOutput::createWindowSurface");
	this->surfaceWindow = window;
	renderTexSurface = eglCore->createWindowSurface(window);
	if (renderTexSurface != NULL){
		eglCore->makeCurrent(renderTexSurface);
		// must after makeCurrent
		renderer = new VideoGLSurfaceRender();
		bool isGLViewInitialized = renderer->init(screenWidth, screenHeight);// there must be right：1080, 810 for 4:3
		if (!isGLViewInitialized) {
			LOGI("GL View failed on initialized...");
		} else {
			surfaceExists = true;
			forceGetFrame = true;
		}
	}
	LOGI("Leave VideoOutput::createWindowSurface");
}

/** 重置视频绘制区域的大小-不需要在EGLThread中,直接调用即可 **/
void VideoOutput::resetRenderSize(int left, int top, int width, int height) {
	if (width > 0 && height > 0){
		screenWidth = width;
		screenHeight = height;
		if (NULL != renderer) {
			renderer->resetRenderSize(left, top, width, height);
		}
	}
}

/** 绘制视频帧 **/
void VideoOutput::signalFrameAvailable() {
//	LOGI("enter VideoOutput::signalFrameAvailable surfaceExists is %d", surfaceExists);

	if(surfaceExists){
		if (handler)
			handler->postMessage(new Message(VIDEO_OUTPUT_MESSAGE_RENDER_FRAME));
	}
}
bool VideoOutput::renderVideo() {
	FrameTexture* texture = NULL;
	//去视频帧队列里面获取 FrameTexture，里面会判断时间，如果视频的速度大于音频，则播放返回nul，这样子就不会渲染新的帧画面
	//如果视频的速度慢于音频，则会做跳帧处理
	//对于forceGetFrame的理解是第一帧渲染才会需要，当获取画面成功后，可能就一直为false，专注于控制视频的时间。为true时候，队列有数据就会立刻返回，不会控制时间
	produceDataCallback(&texture, ctx, forceGetFrame);
	if (NULL != texture && NULL != renderer) {
//		LOGI("VideoOutput::renderVideo() ");
		//这里绑定的 surface 并非离线的，所以将会渲染到屏幕上
		eglCore->makeCurrent(renderTexSurface);
		//渲染到屏幕
		renderer->renderToViewWithAutoFill(texture->texId, screenWidth, screenHeight, texture->width, texture->height);
		if (!eglCore->swapBuffers(renderTexSurface)) {
			LOGE("eglSwapBuffers(renderTexSurface) returned error %d", eglGetError());
		}
		//给 Projector 传递纹理
		if(renderCallback != NULL && ctx != NULL){
			renderCallback(texture, ctx);
		}
	}
	if(forceGetFrame){
		forceGetFrame = false;
	}
	return true;
}

/** 当surface销毁的时候调用 **/
void VideoOutput::onSurfaceDestroyed() {
	LOGI("enter VideoOutput::onSurfaceDestroyed");
	isANativeWindowValid = false;
	if (handler){
		handler->postMessage(new Message(VIDEO_OUTPUT_MESSAGE_DESTROY_WINDOW_SURFACE));
	}
}
void VideoOutput::destroyWindowSurface() {
	LOGI("enter VideoOutput::destroyWindowSurface");
	if (EGL_NO_SURFACE != renderTexSurface){
		if (renderer) {
			renderer->dealloc();
			delete renderer;
			renderer = NULL;
		}

		if (eglCore){
			eglCore->releaseSurface(renderTexSurface);
		}

		renderTexSurface = EGL_NO_SURFACE;
		surfaceExists = false;
		if(NULL != surfaceWindow){
			LOGI("VideoOutput Releasing surfaceWindow");
			ANativeWindow_release(surfaceWindow);
			surfaceWindow = NULL;
		}
	}
}

/** 销毁Output **/
void VideoOutput::stopOutput() {
	LOGI("enter VideoOutput::stopOutput");
	if (handler) {
		handler->postMessage(
				new Message(VIDEO_OUTPUT_MESSAGE_DESTROY_EGL_CONTEXT));
		handler->postMessage(new Message(MESSAGE_QUEUE_LOOP_QUIT_FLAG));
		pthread_join(_threadId, 0);
		if (queue) {
			queue->abort();
			delete queue;
			queue = NULL;
		}

		delete handler;
		handler = NULL;
	}
	LOGI("leave VideoOutput::stopOutput");
}
void VideoOutput::destroyEGLContext() {
	LOGI("enter VideoOutput::destroyEGLContext");
	if (EGL_NO_SURFACE != renderTexSurface){
		eglCore->makeCurrent(renderTexSurface);
	}
	this->destroyWindowSurface();

	if (NULL != eglCore){
		eglCore->release();
		delete eglCore;
		eglCore = NULL;
	}

	eglHasDestroyed = true;

	LOGI("leave VideoOutput::destroyEGLContext");
}

void VideoOutput::setRenderTexCallback(renderTextureCallback callback) {
	this->renderCallback = callback;
}
