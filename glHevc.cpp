///-----------------------------------------------///
/// Example program for setting up a hardware     ///
/// path for hevc and h264 over v4l2 to           ///
/// OpenGL texture.                               ///
///-----------------------------------------------///
//
// sudo apt install libglfw3-dev
// This needs a new Mesa probably higher than v21.
// And it needs rpi-ffmpeg.

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <linux/videodev2.h>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include "include/LEDSwap.h"

#define GLFW_INCLUDE_ES2
extern "C" {
#include <GLFW/glfw3.h>
#include "glhelp.h"
}

#include <drm_fourcc.h>

extern "C" {
/// video file decode
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/avassert.h>
#include <libavutil/imgutils.h>
//
#include "libavutil/hwcontext_drm.h"
}

#include "user_input.h"

static AVBufferRef *hw_device_ctx = NULL;
static enum AVPixelFormat hw_pix_fmt;

static EGLint texgen_attrs[] = {
	EGL_DMA_BUF_PLANE0_FD_EXT,
	EGL_DMA_BUF_PLANE0_OFFSET_EXT,
	EGL_DMA_BUF_PLANE0_PITCH_EXT,
	EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
	EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
	EGL_DMA_BUF_PLANE1_FD_EXT,
	EGL_DMA_BUF_PLANE1_OFFSET_EXT,
	EGL_DMA_BUF_PLANE1_PITCH_EXT,
	EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
	EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
	EGL_DMA_BUF_PLANE2_FD_EXT,
	EGL_DMA_BUF_PLANE2_OFFSET_EXT,
	EGL_DMA_BUF_PLANE2_PITCH_EXT,
	EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
	EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
};

typedef struct egl_aux_s {
  int fd;
  GLuint texture;
} egl_aux_t;

static int hw_decoder_init(AVCodecContext *ctx, const enum AVHWDeviceType type)
{
  int err = 0;

  if ((err = av_hwdevice_ctx_create(&hw_device_ctx, type,
									NULL, NULL, 0)) < 0) {
	fprintf(stderr, "Failed to create specified HW device.\n");
	return err;
  }
  ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);

  return err;
}

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
										const enum AVPixelFormat *pix_fmts)
{
  const enum AVPixelFormat *p;

  for (p = pix_fmts; *p != -1; p++) {
	if (*p == hw_pix_fmt) {
	  return *p;
	}
  }

  fprintf(stderr, "Failed to get HW surface format.\n");
  return AV_PIX_FMT_NONE;
}

// Not completely sure yet how this works
static int write_texture(egl_aux_t *da_out,EGLDisplay *egl_display,AVFrame *frame){
  auto before=std::chrono::steady_clock::now();
  const AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor*)frame->data[0];
  da_out->fd = desc->objects[0].fd;

  /// runs every frame
  if (da_out->texture == 0) {
	//std::cout<<"Generating texture\n";

	EGLint attribs[50];
	EGLint * a = attribs;
	const EGLint * b = texgen_attrs;

	*a++ = EGL_WIDTH;
	*a++ = av_frame_cropped_width(frame);
	*a++ = EGL_HEIGHT;
	*a++ = av_frame_cropped_height(frame);
	*a++ = EGL_LINUX_DRM_FOURCC_EXT;
	*a++ = desc->layers[0].format;

	int i, j;
	for (i = 0; i < desc->nb_layers; ++i) {
	  for (j = 0; j < desc->layers[i].nb_planes; ++j) {
		const AVDRMPlaneDescriptor * const p = desc->layers[i].planes + j;
		const AVDRMObjectDescriptor * const obj = desc->objects + p->object_index;
		*a++ = *b++;
		*a++ = obj->fd;
		*a++ = *b++;
		*a++ = p->offset;
		*a++ = *b++;
		*a++ = p->pitch;
		if (obj->format_modifier == 0) {
		  b += 2;
		}
		else {
		  *a++ = *b++;
		  *a++ = (EGLint)(obj->format_modifier & 0xFFFFFFFF);
		  *a++ = *b++;
		  *a++ = (EGLint)(obj->format_modifier >> 32);
		}
	  }
	}

	*a = EGL_NONE;

	const EGLImage image = eglCreateImageKHR(*egl_display,
											 EGL_NO_CONTEXT,
											 EGL_LINUX_DMA_BUF_EXT,
											 NULL, attribs);
	if (!image) {
	  printf("Failed to create EGLImage\n");
	  return -1;
	}

	/// his
	glGenTextures(1, &da_out->texture);
	glEnable(GL_TEXTURE_EXTERNAL_OES);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, da_out->texture);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, image);

	eglDestroyImageKHR(*egl_display, image);
	auto delta=std::chrono::steady_clock::now()-before;
	std::cout<<"Creating texture took:"<<std::chrono::duration_cast<std::chrono::milliseconds>(delta).count()<<"ms\n";
  }
  return 0;
}

// This function is in the main loop.
static int decode_write(egl_aux_t *da_out, EGLDisplay *egl_display, AVCodecContext * const avctx, AVPacket *packet)
{
  AVFrame *frame = NULL, *sw_frame = NULL;
  int ret = 0;
  ret = avcodec_send_packet(avctx, packet);
  if (ret < 0) {
	fprintf(stderr, "Error during decoding\n");
	return ret;
  }

  while (true) {

	if (!(frame = av_frame_alloc()) || !(sw_frame = av_frame_alloc())) {
	  fprintf(stderr, "Can not alloc frame\n");
	  ret = AVERROR(ENOMEM);
	  av_frame_free(&frame);
	  av_frame_free(&sw_frame);
	  if (ret < 0)
		return ret;
	}

	ret = avcodec_receive_frame(avctx, frame);
	if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
	  av_frame_free(&frame);
	  av_frame_free(&sw_frame);
	  return 0; /// goes sends/gets another packet.
	} else if (ret < 0) {
	  fprintf(stderr, "Error while decoding\n");
	  av_frame_free(&frame);
	  av_frame_free(&sw_frame);
	  if (ret < 0)
		return ret;
	}

	int ret2= write_texture(da_out,egl_display,frame);
	av_frame_free(&frame);
	av_frame_free(&sw_frame);
	return ret2;
  }
}
//Sends one frame to the decoder, then waits for the output frame to become available
static int decode_and_wait_for_frame(AVCodecContext * const avctx,AVPacket *packet,egl_aux_t *da_out, EGLDisplay *egl_display){
  AVFrame *frame = nullptr;
  // testing
  //check_single_nalu(packet->data,packet->size);
  //std::cout<<"Decode packet:"<<packet->pos<<" size:"<<packet->size<<" B\n";
  packet->pts=getTimeUs();
  const auto before=std::chrono::steady_clock::now();
  int ret = avcodec_send_packet(avctx, packet);
  if (ret < 0) {
	fprintf(stderr, "Error during decoding\n");
	return ret;
  }
  // alloc output frame(s)
  if (!(frame = av_frame_alloc())) {
	fprintf(stderr, "Can not alloc frame\n");
	ret = AVERROR(ENOMEM);
	av_frame_free(&frame);
	return ret;
  }
  // Poll until we get the frame out
  // XX first few frames ?!
  const auto beginReceiveFrame=std::chrono::steady_clock::now();
  bool gotFrame=false;
  while (!gotFrame){
	ret = avcodec_receive_frame(avctx, frame);
	if(ret == AVERROR_EOF){
	  std::cout<<"Got EOF\n";
	  break;
	}else if(ret==0){
	  // we got a new frame
	  const auto x_delay=std::chrono::steady_clock::now()-before;
	  std::cout<<"(True) decode delay:"<<((float)std::chrono::duration_cast<std::chrono::microseconds>(x_delay).count()/1000.0f)<<" ms\n";
	  //avgDecodeTime.add(x_delay);
	  //avgDecodeTime.printInIntervals(CALCULATOR_LOG_INTERVAL);
	  gotFrame=true;
	  //const auto now=getTimeUs();
	  //MLOGD<<"Frame pts:"<<frame->pts<<" Set to:"<<now<<"\n";
	  //frame->pts=now;
	  //frame->pts=beforeUs;
	  // display frame
	  auto reported_delay_us=getTimeUs()-frame->pts;
	  std::cout<<"Reported decode:"<<((float)reported_delay_us/1000.0f)<<"ms\n";
	  write_texture(da_out,egl_display,frame);
	}else{
	  //std::cout<<"avcodec_receive_frame returned:"<<ret<<"\n";
	}
	if(std::chrono::steady_clock::now()-beginReceiveFrame>std::chrono::seconds(3)){
	  // Now the decode latency measurement is not correct anymore !
	  std::cout<<"No frame after 3 seconds\n";
	  av_frame_free(&frame);
	  return 0; /// goes sends/gets another packet.
	}
  }
  av_frame_free(&frame);
  return 0;
}



static const GLchar* vertex_shader_source =
	"#version 300 es\n"
	"in vec3 position;\n"
	"in vec2 tx_coords;\n"
	"out vec2 v_texCoord;\n"
	"void main() {  \n"
	"	gl_Position = vec4(position, 1.0);\n"
	"	v_texCoord = tx_coords;\n"
	"}\n";

static const GLchar* fragment_shader_source =
	"#version 300 es\n"
	"#extension GL_OES_EGL_image_external : require\n"
	"precision mediump float;\n"
	"uniform samplerExternalOES texture;\n"
	"in vec2 v_texCoord;\n"
	"out vec4 out_color;\n"
	"void main() {	\n"
	"	out_color = texture2D( texture, v_texCoord );\n"
	"}\n";

/// negative x,y is bottom left and first vertex
//Consti10: Video was flipped horizontally (at least big buck bunny)
static const GLfloat vertices[][4][3] =
	{
		{ {-1.0, -1.0, 0.0}, { 1.0, -1.0, 0.0}, {-1.0, 1.0, 0.0}, {1.0, 1.0, 0.0} }
	};
static const GLfloat uv_coords[][4][2] =
	{
		//{ {0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}, {1.0, 1.0} }
		{ {1.0, 1.0}, {0.0, 1.0}, {1.0, 0.0}, {0.0, 0.0} }
	};

GLint common_get_shader_program(const char *vertex_shader_source, const char *fragment_shader_source) {
  enum Consts {INFOLOG_LEN = 512};
  GLchar infoLog[INFOLOG_LEN];
  GLint fragment_shader;
  GLint shader_program;
  GLint success;
  GLint vertex_shader;

  /* Vertex shader */
  vertex_shader = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(vertex_shader, 1, &vertex_shader_source, NULL);
  glCompileShader(vertex_shader);
  glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &success);
  if (!success) {
	glGetShaderInfoLog(vertex_shader, INFOLOG_LEN, NULL, infoLog);
	printf("ERROR::SHADER::VERTEX::COMPILATION_FAILED\n%s\n", infoLog);
  }

  /* Fragment shader */
  fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(fragment_shader, 1, &fragment_shader_source, NULL);
  glCompileShader(fragment_shader);
  glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &success);
  if (!success) {
	glGetShaderInfoLog(fragment_shader, INFOLOG_LEN, NULL, infoLog);
	printf("ERROR::SHADER::FRAGMENT::COMPILATION_FAILED\n%s\n", infoLog);
  }

  /* Link shaders */
  shader_program = glCreateProgram();
  glAttachShader(shader_program, vertex_shader);
  glAttachShader(shader_program, fragment_shader);
  glLinkProgram(shader_program);
  glGetProgramiv(shader_program, GL_LINK_STATUS, &success);
  if (!success) {
	glGetProgramInfoLog(shader_program, INFOLOG_LEN, NULL, infoLog);
	printf("ERROR::SHADER::PROGRAM::LINKING_FAILED\n%s\n", infoLog);
  }

  glDeleteShader(vertex_shader);
  glDeleteShader(fragment_shader);
  return shader_program;
}




/////////////  M A I N  ////////////////////////////////////////////////


int main(int argc, char *argv[]) {

  auto options= parse_run_parameters(argc,argv);
  std::cout<<"Disable VSYNC:"<<(options.disable_vsync?"Y":"N")<<"\n";

  GLuint shader_program, vbo;
  GLint pos;
  GLint uvs;
  GLFWwindow* window;

  glfwInit();
  glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
  glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
  glfwWindowHint(GLFW_RESIZABLE, GL_FALSE);
  if(options.disable_vsync){
	//glfwWindowHint( GLFW_DOUBLEBUFFER,GL_FALSE );
  }
  if(options.disable_vsync){
	window = glfwCreateWindow(options.width, options.height, __FILE__, glfwGetPrimaryMonitor(), NULL);
  }else{
	window = glfwCreateWindow(options.width, options.height, __FILE__,NULL, NULL);
  }
  glfwMakeContextCurrent(window);
  if(options.disable_vsync){
	// Doesn't work
	//std::cout<<"Disabling VSYNC\n";
	glfwSwapInterval( 0 );
  }

  //EGLDisplay egl_display = glfwGetEGLDisplay();
  EGLDisplay egl_display = eglGetCurrentDisplay();
  if(egl_display == EGL_NO_DISPLAY) {
	printf("error: glfwGetEGLDisplay no EGLDisplay returned\n");
  }

  printf("GL_VERSION  : %s\n", glGetString(GL_VERSION) );
  printf("GL_RENDERER : %s\n", glGetString(GL_RENDERER) );

  shader_program = common_get_shader_program(vertex_shader_source, fragment_shader_source);
  pos = glGetAttribLocation(shader_program, "position");
  uvs = glGetAttribLocation(shader_program, "tx_coords");

  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glViewport(0, 0, options.width, options.height);

  glGenBuffers(1, &vbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(vertices)+sizeof(uv_coords), 0, GL_STATIC_DRAW);
  glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
  glBufferSubData(GL_ARRAY_BUFFER, sizeof(vertices), sizeof(uv_coords), uv_coords);
  glEnableVertexAttribArray(pos);
  glEnableVertexAttribArray(uvs);
  glVertexAttribPointer(pos, 3, GL_FLOAT, GL_FALSE, 0, (GLvoid*)0);
  glVertexAttribPointer(uvs, 2, GL_FLOAT, GL_FALSE, 0, (GLvoid*)sizeof(vertices)); /// last is offset to loc in buf memory
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  /// END OpenGL setup ------------------------------------
  /// BEGIN avcodec setup ---------------------------------

  AVFormatContext *input_ctx = NULL;
  int video_stream, ret;
  AVStream *video = NULL;
  AVCodecContext *decoder_ctx = NULL;
  AVCodec *softwareDecoder = NULL;
  AVCodec *decoder = NULL;
  AVPacket packet;
  enum AVHWDeviceType type;

  type = av_hwdevice_find_type_by_name("drm");

  if (type == AV_HWDEVICE_TYPE_NONE) {
	fprintf(stderr, "Device type is not supported.\n");
	fprintf(stderr, "Available device types:");
	while((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE)
	  fprintf(stderr, " %s", av_hwdevice_get_type_name(type));

	fprintf(stderr, "\n");
	return -1;
  }
  // These options are needed for using the foo.sdp (rtp streaming)
  AVDictionary* av_dictionary=NULL;
  av_dict_set(&av_dictionary, "protocol_whitelist", "file,udp,rtp", 0);
  av_dict_set_int(&av_dictionary, "stimeout", 1000000, 0);
  av_dict_set_int(&av_dictionary, "rw_timeout", 1000000, 0);
  av_dict_set_int(&av_dictionary, "reorder_queue_size", 1, 0);

  /// open the input file
  if (avformat_open_input(&input_ctx, options.filename.c_str(), NULL, &av_dictionary) != 0) {
	fprintf(stderr, "Cannot open input file '%s'\n", options.filename.c_str());
	return -1;
  }

  if (avformat_find_stream_info(input_ctx, NULL) < 0) {
	fprintf(stderr, "Cannot find input stream information.\n");
	return -1;
  }

  /// find the video stream information */
  ret = av_find_best_stream(input_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &softwareDecoder, 0);
  if (ret < 0) {
	fprintf(stderr, "Cannot find a video stream in the input file\n");
	return -1;
  }
  video_stream = ret;

  if(softwareDecoder -> id == AV_CODEC_ID_H264) {
	decoder = avcodec_find_decoder_by_name("h264_v4l2m2m");
  }

  if(softwareDecoder -> id == AV_CODEC_ID_HEVC) {
	decoder = avcodec_find_decoder_by_name("hevc");
  }

  if(!decoder) {
	printf("decoder not found");
	return -1;
  }

  /// Pixel format must be AV_PIX_FMT_DRM_PRIME!  (DRM DMA buffers)
  hw_pix_fmt = AV_PIX_FMT_DRM_PRIME;

  if (!(decoder_ctx = avcodec_alloc_context3(decoder)))
	return AVERROR(ENOMEM);

  video = input_ctx->streams[video_stream];
  if (avcodec_parameters_to_context(decoder_ctx, video->codecpar) < 0)
	return -1;

  decoder_ctx->get_format  = get_hw_format;

  printf("Codec Format: %s\n", decoder_ctx->codec->name);

  if (hw_decoder_init(decoder_ctx, type) < 0) {
	printf("Failed hw_decoder_init\n");
	return -1;
  }
  // this reduced latency a lot in hello_drmprime, still needed ?!
  decoder_ctx->thread_count = 1;

  if ((ret = avcodec_open2(decoder_ctx, decoder, NULL)) < 0) {
	fprintf(stderr, "Failed to open codec for stream #%u\n", video_stream);
	return -1;
  }


  /// --- MAIN Loop ------------------------------------------------------

  //egl_aux_t* da = malloc(sizeof *da);
  egl_aux_t* da = (egl_aux_t*) malloc(sizeof *da);
  da->fd=-1;
  da->texture=0;

  uint64_t lastFrameUs=0;

  while (ret>=0 && !glfwWindowShouldClose(window)) {

	glfwPollEvents();  /// for mouse window closing

	if ((ret = av_read_frame(input_ctx, &packet)) < 0) {
	  printf("fail reading a packet...\n");
	  break;
	}

	if (video_stream == packet.stream_index) {
	  if(options.led_swap_enable) {
		// wait for a keyboard input
		printf("Press ENTER key to Feed new frame\n");
		auto tmp = getchar();
		// change LED, feed one new frame
		switch_led_on_off();
	  }

	  const auto before=std::chrono::steady_clock::now();

	  //ret = decode_write(da, &egl_display, decoder_ctx, &packet);
	  ret= decode_and_wait_for_frame(decoder_ctx,&packet,da,&egl_display);

	  if(da->fd > -1 && da->texture >= 0) {
		const auto delta=std::chrono::steady_clock::now()-before;
		//std::cout<<"Decode_write took:"<<std::chrono::duration_cast<std::chrono::milliseconds>(delta).count()<<"ms\n";

		///printf("Fd: %d	Texture: %d\n", da->fd, da->texture);
		const auto beforeRendering=std::chrono::steady_clock::now();

		///glBindTexture(GL_TEXTURE_EXTERNAL_OES, da->texture);
		///glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);  //"u_blitter:600: Caught recursion. This is a driver bug."
		glClearColor(0.0, 0.0, 0.0, 1.0);
		//glClear(GL_COLOR_BUFFER_BIT);
		glClear(GL_COLOR_BUFFER_BIT |GL_DEPTH_BUFFER_BIT| GL_STENCIL_BUFFER_BIT);
		glUseProgram(shader_program);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		const auto deltaRender=std::chrono::steady_clock::now()-beforeRendering;
		if(deltaRender>std::chrono::milliseconds(2)){
		  std::cout<<"gl send commands took:"<<((float)std::chrono::duration_cast<std::chrono::microseconds>(deltaRender).count()/1000.0f)<<" ms\n";
		}
		const auto beforeSwap=std::chrono::steady_clock::now();
		/*if(options.disable_vsync){
		  glFlush();
		}else{
		  glfwSwapBuffers(window);
		}*/
		glfwSwapBuffers(window);
		const auto deltaSwap=std::chrono::steady_clock::now()-beforeSwap;
		std::cout<<"swap took:"<<((float)std::chrono::duration_cast<std::chrono::microseconds>(deltaSwap).count()/1000.0f)<<" ms\n";

		/// flush
		glDeleteTextures(1, &da->texture);
		da->texture = 0;
		da->fd = -1;
		if(lastFrameUs==0){
		  lastFrameUs=getTimeUs();
		}else{
		  const auto deltaUs=getTimeUs()-lastFrameUs;
		  std::cout<<"Frame time:"<<(deltaUs/1000.0)<<"ms\n";
		  lastFrameUs=getTimeUs();
		}
	  }

	  //usleep(3000); // hacky backy
	}

	av_packet_unref(&packet);
  }

  printf("END avcodec test\n");
  glDeleteBuffers(1, &vbo);
  glfwTerminate();
  return 0;
}
