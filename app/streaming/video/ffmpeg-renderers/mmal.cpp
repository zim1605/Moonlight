#include "mmal.h"

#include "streaming/streamutils.h"
#include "streaming/session.h"

#include <Limelight.h>

#include <SDL_syswm.h>

MmalRenderer::MmalRenderer()
    : m_Isp(nullptr),
      m_YuvFrames(nullptr),
      m_OutputPool(nullptr),
      m_OutputBuffer({.fd = -1, .vcsmHandle = 0}),
      m_OutputImage(0),
      m_EGLDisplay(EGL_NO_DISPLAY),
      m_ColorSpace(AVCOL_SPC_UNSPECIFIED),
      m_ColorRange(AVCOL_RANGE_UNSPECIFIED),
      m_VideoWidth(0),
      m_VideoHeight(0),
      m_EGLExtDmaBuf(false),
      m_eglCreateImage(nullptr),
      m_eglDestroyImage(nullptr),
      m_eglCreateImageKHR(nullptr),
      m_eglDestroyImageKHR(nullptr)
{
}

MmalRenderer::~MmalRenderer()
{
    freeEglImage();

    if (m_Isp != nullptr) {
        mmal_port_disable(m_Isp->input[0]);
        mmal_port_disable(m_Isp->output[0]);

        if (m_OutputPool != nullptr) {
            mmal_port_pool_destroy(m_Isp->output[0], m_OutputPool);
        }

        mmal_component_destroy(m_Isp);
    }

    if (m_YuvFrames != nullptr) {
        mmal_queue_destroy(m_YuvFrames);
    }

    if (m_OutputBuffer.fd >= 0) {
        close(m_OutputBuffer.fd);
    }

    if (m_OutputBuffer.vcsmHandle != 0) {
        vcsm_free(m_OutputBuffer.vcsmHandle);
    }
}

bool MmalRenderer::prepareDecoderContext(AVCodecContext* context, AVDictionary** options)
{
    // FFmpeg defaults this to 10 which is too large to fit in the default 64 MB VRAM split.
    // Reducing to 2 seems to work fine for our bitstreams (max of 1 buffered frame needed).
    av_dict_set_int(options, "extra_buffers", 2, 0);

    // MMAL seems to dislike certain initial width and height values, but it seems okay
    // with getting zero for the width and height. We'll zero them all the time to be safe.
    context->width = 0;
    context->height = 0;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Using MMAL renderer");

    return true;
}


bool MmalRenderer::initialize(PDECODER_PARAMETERS params)
{
    MMAL_STATUS_T status;

    m_VideoWidth = params->width;
    m_VideoHeight = params->height;

    m_YuvFrames = mmal_queue_create();
    if (m_YuvFrames == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "mmal_queue_create() failed");
        return false;
    }

    status = mmal_component_create("vc.ril.isp", &m_Isp);
    if (status != MMAL_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "mmal_component_create() failed: %x (%s)",
                     status, mmal_status_to_string(status));
        return false;
    }

    //status = mmal_port_parameter_set_boolean(m_Isp->output[0], MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE);
    if (status != MMAL_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "mmal_port_parameter_set_boolean(MMAL_PARAMETER_ZERO_COPY, SDL_TRUE) failed: %x (%s)",
                     status, mmal_status_to_string(status));
        return false;
    }

    status = mmal_component_enable(m_Isp);
    if (status != MMAL_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "mmal_component_enable() failed: %x (%s)",
                     status, mmal_status_to_string(status));
        return false;
    }

    m_Isp->input[0]->userdata = (struct MMAL_PORT_USERDATA_T*)this;
    m_Isp->input[0]->format->encoding = MMAL_ENCODING_OPAQUE;
    m_Isp->input[0]->format->es->video.width = params->width;
    m_Isp->input[0]->format->es->video.height = params->height;
    m_Isp->input[0]->format->es->video.crop.x = 0;
    m_Isp->input[0]->format->es->video.crop.y = 0;
    m_Isp->input[0]->format->es->video.crop.width = params->width;
    m_Isp->input[0]->format->es->video.crop.height = params->height;
    mmal_format_full_copy(m_Isp->output[0]->format, m_Isp->input[0]->format);
    m_Isp->output[0]->userdata = (struct MMAL_PORT_USERDATA_T*)this;
    m_Isp->output[0]->format->encoding = MMAL_ENCODING_NV12;
    m_Isp->output[0]->buffer_num = 1;

    status = mmal_port_format_commit(m_Isp->output[0]);
    if (status != MMAL_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "mmal_port_format_commit(output) failed: %x (%s)",
                     status, mmal_status_to_string(status));
        return false;
    }

    status = mmal_port_enable(m_Isp->input[0], InputPortCallback);
    if (status != MMAL_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "mmal_port_enable(input) failed: %x (%s)",
                     status, mmal_status_to_string(status));
        return false;
    }

    // Pass 0 for size to handle allocating the buffers ourselves
    m_OutputPool = mmal_port_pool_create(m_Isp->output[0], m_Isp->output[0]->buffer_num, 0);
    if (m_OutputPool == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "mmal_port_pool_create() failed");
        return false;
    }

    m_OutputBuffer.vcsmHandle = vcsm_malloc(m_Isp->output[0]->buffer_size, "DRM Buf");
    if (m_OutputBuffer.vcsmHandle == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "vcsm_malloc() failed");
        return false;
    }

    m_OutputBuffer.fd = vcsm_export_dmabuf(m_OutputBuffer.vcsmHandle);
    if (m_OutputBuffer.fd < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "vcsm_export_dmabuf() failed");
        return false;
    }

    m_OutputPool->header[0]->data = (uint8_t*)vcsm_vc_hdl_from_hdl(m_OutputBuffer.vcsmHandle);
    m_OutputPool->header[0]->alloc_size = m_Isp->output[0]->buffer_size;
    m_OutputPool->header[0]->length = 0;

    status = mmal_port_enable(m_Isp->output[0], OutputPortCallback);
    if (status != MMAL_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "mmal_port_enable(output) failed: %x (%s)",
                     status, mmal_status_to_string(status));
        return false;
    }

    m_OutputFrameDescriptor.nb_objects = 1;
    m_OutputFrameDescriptor.objects[0].fd = m_OutputBuffer.fd;
    m_OutputFrameDescriptor.objects[0].format_modifier = DRM_FORMAT_MOD_INVALID;
    m_OutputFrameDescriptor.objects[0].size = m_Isp->output[0]->buffer_size;
    m_OutputFrameDescriptor.nb_layers = 1;
    m_OutputFrameDescriptor.layers[0].format = DRM_FORMAT_NV12;
    m_OutputFrameDescriptor.layers[0].nb_planes = 2;
    m_OutputFrameDescriptor.layers[0].planes[0].object_index = 0;
    m_OutputFrameDescriptor.layers[0].planes[0].offset = 0;
    m_OutputFrameDescriptor.layers[0].planes[0].pitch = mmal_encoding_width_to_stride(m_Isp->output[0]->format->encoding,
                                                                                      m_Isp->output[0]->format->es->video.width);
    m_OutputFrameDescriptor.layers[0].planes[1].object_index = 0;
    m_OutputFrameDescriptor.layers[0].planes[1].offset = m_OutputFrameDescriptor.layers[0].planes[0].pitch * m_Isp->output[0]->format->es->video.height;
    m_OutputFrameDescriptor.layers[0].planes[1].pitch = m_OutputFrameDescriptor.layers[0].planes[0].pitch;

    return true;
}

void MmalRenderer::InputPortCallback(MMAL_PORT_T*, MMAL_BUFFER_HEADER_T*)
{
    // Do nothing - FFmpeg owns the buffer reference
}

void MmalRenderer::OutputPortCallback(MMAL_PORT_T* port, MMAL_BUFFER_HEADER_T* buffer)
{
    auto me = (MmalRenderer*)port->userdata;

    // exportEGLImages() will free the buffer reference
    mmal_queue_put(me->m_YuvFrames, buffer);
}

bool MmalRenderer::createEglImage(AVFrame *frame)
{
    // Max 30 attributes (1 key + 1 value for each)
    const int MAX_ATTRIB_COUNT = 30 * 2;
    EGLAttrib attribs[MAX_ATTRIB_COUNT] = {
        EGL_LINUX_DRM_FOURCC_EXT, (EGLAttrib)m_OutputFrameDescriptor.layers[0].format,
        EGL_WIDTH, frame->width,
        EGL_HEIGHT, frame->height,
    };
    int attribIndex = 6;

    freeEglImage();

    for (int i = 0; i < m_OutputFrameDescriptor.layers[0].nb_planes; ++i) {
        const auto &plane = m_OutputFrameDescriptor.layers[0].planes[i];
        const auto &object = m_OutputFrameDescriptor.objects[plane.object_index];

        switch (i) {
        case 0:
            attribs[attribIndex++] = EGL_DMA_BUF_PLANE0_FD_EXT;
            attribs[attribIndex++] = object.fd;
            attribs[attribIndex++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
            attribs[attribIndex++] = plane.offset;
            attribs[attribIndex++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
            attribs[attribIndex++] = plane.pitch;
            if (m_EGLExtDmaBuf && object.format_modifier != DRM_FORMAT_MOD_INVALID) {
                attribs[attribIndex++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
                attribs[attribIndex++] = (EGLint)(object.format_modifier & 0xFFFFFFFF);
                attribs[attribIndex++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
                attribs[attribIndex++] = (EGLint)(object.format_modifier >> 32);
            }
            break;

        case 1:
            attribs[attribIndex++] = EGL_DMA_BUF_PLANE1_FD_EXT;
            attribs[attribIndex++] = object.fd;
            attribs[attribIndex++] = EGL_DMA_BUF_PLANE1_OFFSET_EXT;
            attribs[attribIndex++] = plane.offset;
            attribs[attribIndex++] = EGL_DMA_BUF_PLANE1_PITCH_EXT;
            attribs[attribIndex++] = plane.pitch;
            if (m_EGLExtDmaBuf && object.format_modifier != DRM_FORMAT_MOD_INVALID) {
                attribs[attribIndex++] = EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT;
                attribs[attribIndex++] = (EGLint)(object.format_modifier & 0xFFFFFFFF);
                attribs[attribIndex++] = EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT;
                attribs[attribIndex++] = (EGLint)(object.format_modifier >> 32);
            }
            break;

        case 2:
            attribs[attribIndex++] = EGL_DMA_BUF_PLANE2_FD_EXT;
            attribs[attribIndex++] = object.fd;
            attribs[attribIndex++] = EGL_DMA_BUF_PLANE2_OFFSET_EXT;
            attribs[attribIndex++] = plane.offset;
            attribs[attribIndex++] = EGL_DMA_BUF_PLANE2_PITCH_EXT;
            attribs[attribIndex++] = plane.pitch;
            if (m_EGLExtDmaBuf && object.format_modifier != DRM_FORMAT_MOD_INVALID) {
                attribs[attribIndex++] = EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT;
                attribs[attribIndex++] = (EGLint)(object.format_modifier & 0xFFFFFFFF);
                attribs[attribIndex++] = EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT;
                attribs[attribIndex++] = (EGLint)(object.format_modifier >> 32);
            }
            break;

        case 3:
            attribs[attribIndex++] = EGL_DMA_BUF_PLANE3_FD_EXT;
            attribs[attribIndex++] = object.fd;
            attribs[attribIndex++] = EGL_DMA_BUF_PLANE3_OFFSET_EXT;
            attribs[attribIndex++] = plane.offset;
            attribs[attribIndex++] = EGL_DMA_BUF_PLANE3_PITCH_EXT;
            attribs[attribIndex++] = plane.pitch;
            if (m_EGLExtDmaBuf && object.format_modifier != DRM_FORMAT_MOD_INVALID) {
                attribs[attribIndex++] = EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT;
                attribs[attribIndex++] = (EGLint)(object.format_modifier & 0xFFFFFFFF);
                attribs[attribIndex++] = EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT;
                attribs[attribIndex++] = (EGLint)(object.format_modifier >> 32);
            }
            break;

        default:
            Q_UNREACHABLE();
        }
    }

    // Add colorspace data if present
    switch (frame->colorspace) {
    case AVCOL_SPC_BT2020_CL:
    case AVCOL_SPC_BT2020_NCL:
        attribs[attribIndex++] = EGL_YUV_COLOR_SPACE_HINT_EXT;
        attribs[attribIndex++] = EGL_ITU_REC2020_EXT;
        break;
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_FCC:
        attribs[attribIndex++] = EGL_YUV_COLOR_SPACE_HINT_EXT;
        attribs[attribIndex++] = EGL_ITU_REC601_EXT;
        break;
    case AVCOL_SPC_BT709:
        attribs[attribIndex++] = EGL_YUV_COLOR_SPACE_HINT_EXT;
        attribs[attribIndex++] = EGL_ITU_REC709_EXT;
        break;
    default:
        break;
    }

    // Add color range data if present
    switch (frame->color_range) {
    case AVCOL_RANGE_JPEG:
        attribs[attribIndex++] = EGL_SAMPLE_RANGE_HINT_EXT;
        attribs[attribIndex++] = EGL_YUV_FULL_RANGE_EXT;
        break;
    case AVCOL_RANGE_MPEG:
        attribs[attribIndex++] = EGL_SAMPLE_RANGE_HINT_EXT;
        attribs[attribIndex++] = EGL_YUV_NARROW_RANGE_EXT;
        break;
    default:
        break;
    }

    // Terminate the attribute list
    attribs[attribIndex++] = EGL_NONE;
    SDL_assert(attribIndex <= MAX_ATTRIB_COUNT);

    // Our EGLImages are non-planar, so we only populate the first entry
    if (m_eglCreateImage) {
        m_OutputImage = m_eglCreateImage(m_EGLDisplay, EGL_NO_CONTEXT,
                                         EGL_LINUX_DMA_BUF_EXT,
                                         nullptr, attribs);
        if (!m_OutputImage) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "eglCreateImage() Failed: %d", eglGetError());
            return false;
        }
    }
    else {
        // Cast the EGLAttrib array elements to EGLint for the KHR extension
        EGLint intAttribs[MAX_ATTRIB_COUNT];
        for (int i = 0; i < MAX_ATTRIB_COUNT; i++) {
            intAttribs[i] = (EGLint)attribs[i];
        }

        m_OutputImage = m_eglCreateImageKHR(m_EGLDisplay, EGL_NO_CONTEXT,
                                            EGL_LINUX_DMA_BUF_EXT,
                                            nullptr, intAttribs);
        if (!m_OutputImage) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "eglCreateImageKHR() Failed: %d", eglGetError());
            return false;
        }
    }

    m_ColorSpace = frame->colorspace;
    m_ColorRange = frame->color_range;

    return true;
}

void MmalRenderer::freeEglImage()
{
    if (m_OutputImage) {
        if (m_eglDestroyImage) {
            m_eglDestroyImage(m_EGLDisplay, m_OutputImage);
        }
        else {
            m_eglDestroyImageKHR(m_EGLDisplay, m_OutputImage);
        }
        m_OutputImage = 0;
    }
}

enum AVPixelFormat MmalRenderer::getPreferredPixelFormat(int)
{
    // Opaque MMAL buffers
    return AV_PIX_FMT_MMAL;
}

int MmalRenderer::getRendererAttributes()
{
    // This renderer maxes out at 1080p
    return RENDERER_ATTRIBUTE_1080P_MAX;
}

bool MmalRenderer::canExportEGL()
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "MMAL backend supports exporting EGLImage");
    return true;
}

AVPixelFormat MmalRenderer::getEGLImagePixelFormat()
{
    // This tells EGLRenderer to treat the EGLImage as a single opaque texture
    return AV_PIX_FMT_DRM_PRIME;
}

bool MmalRenderer::initializeEGL(EGLDisplay dpy, const EGLExtensions &ext)
{
    if (!ext.isSupported("EGL_EXT_image_dma_buf_import")) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "MMAL-EGL: DMABUF unsupported");
        return false;
    }

    m_EGLDisplay = dpy;
    m_EGLExtDmaBuf = ext.isSupported("EGL_EXT_image_dma_buf_import_modifiers");

    // NB: eglCreateImage() and eglCreateImageKHR() have slightly different definitions
    m_eglCreateImage = (typeof(m_eglCreateImage))eglGetProcAddress("eglCreateImage");
    m_eglCreateImageKHR = (typeof(m_eglCreateImageKHR))eglGetProcAddress("eglCreateImageKHR");
    m_eglDestroyImage = (typeof(m_eglDestroyImage))eglGetProcAddress("eglDestroyImage");
    m_eglDestroyImageKHR = (typeof(m_eglDestroyImageKHR))eglGetProcAddress("eglDestroyImageKHR");

    if (!(m_eglCreateImage && m_eglDestroyImage) &&
            !(m_eglCreateImageKHR && m_eglDestroyImageKHR)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Missing eglCreateImage()/eglDestroyImage() in EGL driver");
        return false;
    }

    return true;
}

ssize_t MmalRenderer::exportEGLImages(AVFrame *frame, EGLDisplay, EGLImage images[])
{
    MMAL_BUFFER_HEADER_T* opaqueBuffer = (MMAL_BUFFER_HEADER_T*)frame->data[3];
    MMAL_BUFFER_HEADER_T* yuvBuffer;
    MMAL_STATUS_T status;

    memset(images, 0, sizeof(EGLImage) * EGL_MAX_PLANES);

    // Requeue output buffers from the pool so the ISP has something to write into
    while ((yuvBuffer = mmal_queue_get(m_OutputPool->queue)) != nullptr) {
        mmal_port_send_buffer(m_Isp->output[0], yuvBuffer);
    }

    // Send opaque MMAL frame to ISP for conversion to YUV
    status = mmal_port_send_buffer(m_Isp->input[0], opaqueBuffer);
    if (status != MMAL_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "mmal_port_send_buffer() failed: %x (%s)",
                     status, mmal_status_to_string(status));
        return -1;
    }

    // Wait for the frame to come back from the ISP and release it.
    // This is gross, but it works for our case because we only have 1 buffer
    // and we are guaranteed to never queue any input outside of this function.
    mmal_buffer_header_release(mmal_queue_wait(m_YuvFrames));

#if 1
    {
        uint8_t* buf = (uint8_t*)vcsm_lock(m_OutputBuffer.vcsmHandle);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Data: %x %x %x %x %x",
                buf[0], buf[1], buf[2], buf[3], buf[4]);
        vcsm_unlock_ptr((void*)buf);
    }
#endif

    // Check if we need to recreate our EGLImage
    if (!m_OutputImage || frame->colorspace != m_ColorSpace || frame->color_range != m_ColorRange) {
        if (!createEglImage(frame)) {
            // Logging happens in createEglImage()
            return -1;
        }
    }

    images[0] = m_OutputImage;
    return 1;
}

void MmalRenderer::freeEGLImages(EGLDisplay, EGLImage[])
{
    // We manage the lifetime of our own EGLImage, so nothing to do
}

bool MmalRenderer::needsTestFrame()
{
    // We won't be able to decode if the GPU memory is 64 MB or lower,
    // so we must test before allowing the decoder to be used.
    return true;
}

void MmalRenderer::renderFrame(AVFrame*)
{
    // We don't support direct rendering
    SDL_assert(false);
}
