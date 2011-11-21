#include <QMutexLocker>

#include "mythcorecontext.h"
#include "mythlogging.h"
#include "packetqueue.h"
#include "resultslist.h"
#include "videoconsumer.h"
#include "mythxdisplay.h"
#include "videoouttypes.h"
#include "mythcodecid.h"
#include "videopacket.h"
#include "videoprocessor.h"

extern "C" {
#include "libavutil/avutil.h"
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
}

#include "vdpauvideodecoder.h"
#include "ffmpegvideodecoder.h"

VideoConsumer::VideoConsumer(PacketQueue *inQ, ResultsList *outL,
                             OpenCLDevice *dev) : 
    QueueConsumer(inQ, outL, dev, "VideoConsumer"), m_useX(false),
    m_decoder(NULL)
{
    InitVideoProcessors();
    if (m_dev)
        m_proclist = openCLVideoProcessorList;
    else
        m_proclist = softwareVideoProcessorList;

    m_specialDecode = (AVSpecialDecode)(kAVSpecialDecode_LowRes         |
                                        kAVSpecialDecode_SingleThreaded |
                                        kAVSpecialDecode_NoLoopFilter);

    int x = gCoreContext->GetNumSetting("CommFlagFast", 0);
    LOG(VB_GENERAL, LOG_INFO, QString("CommFlagFast: %1").arg(x));
    if (x == 0)
        m_specialDecode = kAVSpecialDecode_None;
    LOG(VB_COMMFLAG, LOG_INFO, QString("Special Decode Flags: 0x%1")
        .arg(m_specialDecode, 0, 16));

    m_context = avcodec_alloc_context();
}

bool VideoConsumer::Initialize(void)
{
    m_decoder = NULL;

    if (m_useX)
    {
        if (m_dev)
        {
            if (m_dev->m_opengl)
            {
                cl_device_id devid = m_dev->m_deviceId;
                uint64_t hash = m_dev->GetHash();

                for (OpenCLDeviceMap::iterator it = devMap->begin();
                     it != devMap->end(); ++it)
                {
                    if (it.value() == m_dev)
                    {
                        devMap->erase(it);
                        break;
                    }
                }
                delete m_dev;
                m_dev = new OpenCLDevice(devid, true);
                devMap->insertMulti(hash, m_dev);
                if (!m_dev->Initialize())
                {
                    m_dev = NULL;
                }
            }
        }

        VDPAUVideoDecoder *d = new VDPAUVideoDecoder(m_dev);
        if (!d->Initialize())
        {
            LOG(VB_GENERAL, LOG_NOTICE,
                "No VDPAU support, using ffmpeg software video decoding.");
            delete d;
            m_useX = false;
        }
        else
        {
            m_decoder = d;
        }
    }

    if (!m_decoder)
    {
        FFMpegVideoDecoder *d = new FFMpegVideoDecoder(m_dev);
        if (!d->Initialize())
        {
            LOG(VB_GENERAL, LOG_ERR,
                "Can't initialize ffmpeg software decoding");
            delete d;
            return false;
        }
        m_decoder = d;
    }

    return true;
}


void VideoConsumer::ProcessPacket(Packet *packet)
{
    LOG(VB_GENERAL, LOG_INFO, "Video Frame");

    AVStream *curstream = packet->m_stream;
    AVPacket *pkt = packet->m_pkt;
    QMutex   *avcodeclock = packet->m_mutex;
    AVCodecContext *ac = curstream->codec;

    if (!m_opened)
    {
        QMutexLocker lock(avcodeclock);

        m_decoder->SetSize(ac->width, ac->height);
        m_decoder->SetCodec(ac->codec);

        if (m_decoder->m_useCPU && !m_decoder->m_isCPU)
        {
            LOG(VB_GENERAL, LOG_NOTICE, 
                QString("%1: Switching to ffmpeg software decoding")
                .arg(m_decoder->Name()));
            delete m_decoder;

            FFMpegVideoDecoder *d = new FFMpegVideoDecoder(m_dev);
            if (!d->Initialize())
            {
                LOG(VB_GENERAL, LOG_ERR,
                    "Can't initialize ffmpeg software decoding");
                delete d;
                done();
                return;
            }
            m_decoder = d;

            m_decoder->SetSize(ac->width, ac->height);
            m_decoder->SetCodec(ac->codec);

        }
        m_codec = m_decoder->m_codec;

        if (avcodec_open(m_context, m_codec) < 0)
        {
            LOG(VB_GENERAL, LOG_ERR, "Can't open video codec!");
            return;
        }

        InitVideoCodec();

        LOG(VB_GENERAL, LOG_INFO,
            QString("Codec %1  Width: %2  Height: %3  PixFmt: %4")
            .arg(m_decoder->m_codec->name)
            .arg(m_decoder->m_width) .arg(m_decoder->m_height)
            .arg(avcodec_get_pix_fmt_name(ac->pix_fmt)));

        m_opened = true;
    }

    // Decode the packet to YUV (using HW if possible)
    AVFrame mpa_pic;
    AVFrame *wavelet = NULL;
    avcodec_get_frame_defaults(&mpa_pic);
    int gotpicture = 0;
    int ret = avcodec_decode_video2(m_context, &mpa_pic, &gotpicture, pkt);
    if (ret < 0)
    {
        LOG(VB_GENERAL, LOG_ERR, "Video: Unknown decoding error");
        return;
    }

    if (!gotpicture)
        return;

    VideoPacket *videoFrame = NULL;
    static uint64_t count = 0;

    // Push YUV frame to GPU/CPU Processing memory
    if (m_dev)
    {
        // Push frame to the GPU via OpenGL/OpenCL
        count++;
        videoFrame = new VideoPacket(m_decoder, &mpa_pic, count);
        if ((count <= 100) && videoFrame)
        {
            // videoFrame->m_frameRaw->Dump("frame", count);
            // videoFrame->m_frameYUVSNORM->Dump("yuv", count);
            // videoFrame->m_wavelet->Dump("wavelet", count);
            VideoSurface rgb(m_dev, kSurfaceRGB,
                             videoFrame->m_frameRaw->m_width,
                             videoFrame->m_frameRaw->m_height);
            OpenCLYUVToRGB(m_dev, videoFrame->m_frameYUV, &rgb);
            rgb.Dump("rgb", count);

            VideoSurface yuv(m_dev, kSurfaceYUV2,
                             videoFrame->m_frameRaw->m_width,
                             videoFrame->m_frameRaw->m_height);
            OpenCLWaveletInverse(m_dev, videoFrame->m_wavelet, &yuv);
            // yuv.Dump("unwaveletYUV", count);
            VideoSurface yuv2(m_dev, kSurfaceYUV,
                              videoFrame->m_frameRaw->m_width,
                              videoFrame->m_frameRaw->m_height);
            OpenCLYUVFromSNORM(m_dev, &yuv, &yuv2);
            OpenCLYUVToRGB(m_dev, &yuv2, &rgb);
            rgb.Dump("unwaveletRGB", count);
        }
    }

    // Get wavelet from the frame (one level)
    if (!m_dev)
    {
        wavelet = new AVFrame;
        SoftwareWavelet(&mpa_pic, wavelet);
    }

    // Loop through the list of detection routines
    for (VideoProcessorList::iterator it = m_proclist->begin();
         it != m_proclist->end(); ++it)
    {
        VideoProcessor *proc = *it;

        // Run the routine in GPU/CPU & pull results
        FlagResults *result = proc->m_func(m_dev, &mpa_pic, wavelet);

        // Toss the results onto the results list
        if (result)
        {
            LOG(VB_GENERAL, LOG_INFO, "Video Finding found");
            result->m_pts = pkt->pts;
            result->m_duration = pkt->duration;
            m_outL->append(result);
        }
    }

    if (m_dev)
    {
        // Free the frame in GPU/CPU memory if not needed
        delete videoFrame;
    }
}

void VideoConsumer::InitVideoCodec(void)
{
    m_context->opaque = (void *)m_decoder;
    m_context->get_buffer = get_avf_buffer;
    m_context->release_buffer = release_avf_buffer;
    m_context->draw_horiz_band = NULL;
    m_context->slice_flags = 0;

    m_context->error_recognition = FF_ER_COMPLIANT;
    m_context->workaround_bugs = FF_BUG_AUTODETECT;
    m_context->error_concealment = FF_EC_GUESS_MVS | FF_EC_DEBLOCK;
    m_context->idct_algo = FF_IDCT_AUTO;
    m_context->debug = 0;
    m_context->rate_emu = 0;
    m_context->error_rate = 0;

    if (CODEC_IS_VDPAU(m_codec))
    {
        m_context->get_buffer      = get_avf_buffer_vdpau;
        m_context->get_format      = get_format_vdpau;
        m_context->release_buffer  = release_avf_buffer_vdpau;
        m_context->draw_horiz_band = render_slice_vdpau;
        m_context->slice_flags     = SLICE_FLAG_CODED_ORDER |
                                     SLICE_FLAG_ALLOW_FIELD;
    }
#if 0
    else if (CODEC_IS_DXVA2(m_codec, m_context))
    {
        m_context->get_buffer      = get_avf_buffer_dxva2;
        m_context->get_format      = get_format_dxva2;
        m_context->release_buffer  = release_avf_buffer;
    }
    else if (CODEC_IS_VAAPI(m_codec, m_context))
    {
        m_context->get_buffer      = get_avf_buffer_vaapi;
        m_context->get_format      = get_format_vaapi;
        m_context->release_buffer  = release_avf_buffer;
        m_context->slice_flags     = SLICE_FLAG_CODED_ORDER |
                                     SLICE_FLAG_ALLOW_FIELD;
    }
#endif
    else if (m_codec && m_codec->capabilities & CODEC_CAP_DR1)
    {
        m_context->flags          |= CODEC_FLAG_EMU_EDGE;
    }
    else
    {
        LOG(VB_PLAYBACK, LOG_INFO,
            QString("Using software scaling to convert pixel format %1 for "
                    "codec %2")
            .arg(m_context->pix_fmt)
            .arg(ff_codec_id_string(m_context->codec_id)));
    }

    if (m_specialDecode)
    {
        m_context->flags2 |= CODEC_FLAG2_FAST;

        if (m_codec->id == CODEC_ID_MPEG2VIDEO ||
            m_codec->id == CODEC_ID_MPEG1VIDEO)
        {
            if (m_specialDecode & kAVSpecialDecode_FewBlocks)
            {
                uint total_blocks = (m_context->height+15) / 16;
                m_context->skip_top     = (total_blocks+3) / 4;
                m_context->skip_bottom  = (total_blocks+3) / 4;
            }

            if (m_specialDecode & kAVSpecialDecode_LowRes)
                m_context->lowres = 2; // 1 = 1/2 size, 2 = 1/4 size
        }
        else if (m_codec->id == CODEC_ID_H264)
        {
            if (m_specialDecode & kAVSpecialDecode_NoLoopFilter)
            {
                m_context->flags &= ~CODEC_FLAG_LOOP_FILTER;
                m_context->skip_loop_filter = AVDISCARD_ALL;
            }
        }

        if (m_specialDecode & kAVSpecialDecode_NoDecode)
        {
            m_context->skip_idct = AVDISCARD_ALL;
        }
    }
}


/*
 * vim:ts=4:sw=4:ai:et:si:sts=4
 */