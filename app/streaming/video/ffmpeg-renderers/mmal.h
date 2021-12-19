#pragma once

#include "renderer.h"

#include <interface/mmal/mmal.h>
#include <interface/mmal/util/mmal_util.h>
#include <interface/mmal/util/mmal_util_params.h>
#include <interface/mmal/util/mmal_default_components.h>
#include <interface/vcsm/user-vcsm.h>

extern "C" {
    #include <libavutil/hwcontext_drm.h>
}

#include <libdrm/drm_fourcc.h>

class MmalRenderer : public IFFmpegRenderer {
public:
    MmalRenderer();
    virtual ~MmalRenderer() override;
    virtual bool initialize(PDECODER_PARAMETERS params) override;
    virtual bool prepareDecoderContext(AVCodecContext* context, AVDictionary** options) override;
    virtual void renderFrame(AVFrame* frame) override;
    virtual enum AVPixelFormat getPreferredPixelFormat(int videoFormat) override;
    virtual bool needsTestFrame() override;
    virtual int getRendererAttributes() override;
    virtual bool canExportEGL() override;
    virtual AVPixelFormat getEGLImagePixelFormat() override;
    virtual bool initializeEGL(EGLDisplay dpy, const EGLExtensions &ext) override;
    virtual ssize_t exportEGLImages(AVFrame *frame, EGLDisplay dpy, EGLImage images[EGL_MAX_PLANES]) override;
    virtual void freeEGLImages(EGLDisplay dpy, EGLImage[EGL_MAX_PLANES]) override;

private:
    static void InputPortCallback(MMAL_PORT_T* port, MMAL_BUFFER_HEADER_T* buffer);
    static void OutputPortCallback(MMAL_PORT_T* port, MMAL_BUFFER_HEADER_T* buffer);

    bool createEglImage(AVFrame* frame);
    void freeEglImage();

    MMAL_COMPONENT_T* m_Isp;
    MMAL_QUEUE_T* m_YuvFrames;
    MMAL_POOL_T* m_OutputPool;

    struct {
        int fd;
        unsigned int vcsmHandle;
    } m_OutputBuffer;
    AVDRMFrameDescriptor m_OutputFrameDescriptor;
    EGLImage m_OutputImage;
    EGLDisplay m_EGLDisplay;
    AVColorSpace m_ColorSpace;
    AVColorRange m_ColorRange;

    int m_VideoWidth, m_VideoHeight;

    bool m_EGLExtDmaBuf;
    PFNEGLCREATEIMAGEPROC m_eglCreateImage;
    PFNEGLDESTROYIMAGEPROC m_eglDestroyImage;
    PFNEGLCREATEIMAGEKHRPROC m_eglCreateImageKHR;
    PFNEGLDESTROYIMAGEKHRPROC m_eglDestroyImageKHR;
};

