/**
 * @file oggvorbis.c
 * Ogg Vorbis codec support via libvorbisenc.
 * @author Mark Hills <mark@pogo.org.uk>
 */

#include <vorbis/vorbisenc.h>

#include "avcodec.h"
#include "oggvorbis.h"

//#define OGGVORBIS_FRAME_SIZE 1024
#define OGGVORBIS_FRAME_SIZE 64

#define BUFFER_SIZE (1024*64)

typedef struct OggVorbisContext {
    vorbis_info vi ;
    vorbis_dsp_state vd ;
    vorbis_block vb ;
    uint8_t buffer[BUFFER_SIZE];
    int buffer_index;

    /* decoder */
    vorbis_comment vc ;
    ogg_packet op;
} OggVorbisContext ;


int oggvorbis_init_encoder(vorbis_info *vi, AVCodecContext *avccontext) {

#ifdef OGGVORBIS_VBR_BY_ESTIMATE
    /* variable bitrate by estimate */

    return (vorbis_encode_setup_managed(vi, avccontext->channels,
              avccontext->sample_rate, -1, avccontext->bit_rate, -1) ||
	    vorbis_encode_ctl(vi, OV_ECTL_RATEMANAGE_AVG, NULL) ||
	    vorbis_encode_setup_init(vi)) ;
#else
    /* constant bitrate */

    return vorbis_encode_init(vi, avccontext->channels,
	          avccontext->sample_rate, -1, avccontext->bit_rate, -1) ;
#endif
}


static int oggvorbis_encode_init(AVCodecContext *avccontext) {
    OggVorbisContext *context = avccontext->priv_data ;

    vorbis_info_init(&context->vi) ;
    if(oggvorbis_init_encoder(&context->vi, avccontext) < 0) {
	av_log(avccontext, AV_LOG_ERROR, "oggvorbis_encode_init: init_encoder failed") ;
	return -1 ;
    }
    vorbis_analysis_init(&context->vd, &context->vi) ;
    vorbis_block_init(&context->vd, &context->vb) ;

    avccontext->frame_size = OGGVORBIS_FRAME_SIZE ;
 
    avccontext->coded_frame= avcodec_alloc_frame();
    avccontext->coded_frame->key_frame= 1;
    
    return 0 ;
}


static int oggvorbis_encode_frame(AVCodecContext *avccontext,
				  unsigned char *packets,
			   int buf_size, void *data)
{
    OggVorbisContext *context = avccontext->priv_data ;
    float **buffer ;
    ogg_packet op ;
    signed char *audio = data ;
    int l, samples = OGGVORBIS_FRAME_SIZE ;

    buffer = vorbis_analysis_buffer(&context->vd, samples) ;

    if(context->vi.channels == 1) {
	for(l = 0 ; l < samples ; l++)
	    buffer[0][l]=((audio[l*2+1]<<8)|(0x00ff&(int)audio[l*2]))/32768.f;
    } else {
	for(l = 0 ; l < samples ; l++){
	    buffer[0][l]=((audio[l*4+1]<<8)|(0x00ff&(int)audio[l*4]))/32768.f;
	    buffer[1][l]=((audio[l*4+3]<<8)|(0x00ff&(int)audio[l*4+2]))/32768.f;
	}
    }
    
    vorbis_analysis_wrote(&context->vd, samples) ; 

    while(vorbis_analysis_blockout(&context->vd, &context->vb) == 1) {
	vorbis_analysis(&context->vb, NULL);
	vorbis_bitrate_addblock(&context->vb) ;

	while(vorbis_bitrate_flushpacket(&context->vd, &op)) {
            memcpy(context->buffer + context->buffer_index, &op, sizeof(ogg_packet));
            context->buffer_index += sizeof(ogg_packet);
            memcpy(context->buffer + context->buffer_index, op.packet, op.bytes);
            context->buffer_index += op.bytes;
//            av_log(avccontext, AV_LOG_DEBUG, "e%d / %d\n", context->buffer_index, op.bytes);
	}
    }

    if(context->buffer_index){
        ogg_packet *op2= context->buffer;
        op2->packet = context->buffer + sizeof(ogg_packet);
        l=  op2->bytes;
        
        memcpy(packets, op2->packet, l);
        context->buffer_index -= l + sizeof(ogg_packet);
        memcpy(context->buffer, context->buffer + l + sizeof(ogg_packet), context->buffer_index);
        
//        av_log(avccontext, AV_LOG_DEBUG, "E%d\n", l);
        return l;
    }

    return 0;
}


static int oggvorbis_encode_close(AVCodecContext *avccontext) {
    OggVorbisContext *context = avccontext->priv_data ;
/*  ogg_packet op ; */
    
    vorbis_analysis_wrote(&context->vd, 0) ; /* notify vorbisenc this is EOF */

    /* We need to write all the remaining packets into the stream
     * on closing */
    
    av_log(avccontext, AV_LOG_ERROR, "fixme: not all packets written on oggvorbis_encode_close()\n") ;

/*
    while(vorbis_bitrate_flushpacket(&context->vd, &op)) {
	memcpy(packets + l, &op, sizeof(ogg_packet)) ;
	memcpy(packets + l + sizeof(ogg_packet), op.packet, op.bytes) ;
	l += sizeof(ogg_packet) + op.bytes ;	
    }
*/

    vorbis_block_clear(&context->vb);
    vorbis_dsp_clear(&context->vd);
    vorbis_info_clear(&context->vi);

    av_freep(&avccontext->coded_frame);
  
    return 0 ;
}


AVCodec oggvorbis_encoder = {
    "vorbis",
    CODEC_TYPE_AUDIO,
    CODEC_ID_VORBIS,
    sizeof(OggVorbisContext),
    oggvorbis_encode_init,
    oggvorbis_encode_frame,
    oggvorbis_encode_close
} ;


static int oggvorbis_decode_init(AVCodecContext *avccontext) {
    OggVorbisContext *context = avccontext->priv_data ;

    vorbis_info_init(&context->vi) ;
    vorbis_comment_init(&context->vc) ;
    context->op.packetno= 0;

    return 0 ;
}


static inline int conv(int samples, float **pcm, char *buf, int channels) {
    int i, j, val ;
    ogg_int16_t *ptr, *data = (ogg_int16_t*)buf ;
    float *mono ;
 
    for(i = 0 ; i < channels ; i++){
	ptr = &data[i];
	mono = pcm[i] ;
	
	for(j = 0 ; j < samples ; j++) {
	    
	    val = mono[j] * 32767.f;
	    
	    if(val > 32767) val = 32767 ;
	    if(val < -32768) val = -32768 ;
	   	    
	    *ptr = val ;
	    ptr += channels;
	}
    }
    
    return 0 ;
}
	   
	
static int oggvorbis_decode_frame(AVCodecContext *avccontext,
                        void *data, int *data_size,
                        uint8_t *buf, int buf_size)
{
    OggVorbisContext *context = avccontext->priv_data ;
    float **pcm ;
    ogg_packet *op= &context->op;    
    int samples, total_samples, total_bytes,i;
 
    if(!buf_size){
    //FIXME flush
        *data_size=0;
        return 0;
    }
    
    op->packet = buf;
    op->bytes  = buf_size;
    op->b_o_s  = op->packetno == 0;

//    av_log(avccontext, AV_LOG_DEBUG, "%d %d %d %lld %lld %d %d\n", op->bytes, op->b_o_s, op->e_o_s, op->granulepos, op->packetno, buf_size, context->vi.rate);
    
/*    for(i=0; i<op->bytes; i++)
      av_log(avccontext, AV_LOG_DEBUG, "%02X ", op->packet[i]);
    av_log(avccontext, AV_LOG_DEBUG, "\n");*/
    if(op->packetno < 3) {
	if(vorbis_synthesis_headerin(&context->vi, &context->vc, op)<0){
            av_log(avccontext, AV_LOG_ERROR, "%lld. vorbis header damaged\n", op->packetno+1);
            return -1;
        }
	avccontext->channels = context->vi.channels ;
	avccontext->sample_rate = context->vi.rate ;
        op->packetno++;
	return buf_size ;
    }

    if(op->packetno == 3) {
//	av_log(avccontext, AV_LOG_INFO, "vorbis_decode: %d channel, %ldHz, encoder `%s'\n",
//		context->vi.channels, context->vi.rate, context->vc.vendor);

	vorbis_synthesis_init(&context->vd, &context->vi) ;
	vorbis_block_init(&context->vd, &context->vb); 
    }

    if(vorbis_synthesis(&context->vb, op) == 0)
	vorbis_synthesis_blockin(&context->vd, &context->vb) ;
    
    total_samples = 0 ;
    total_bytes = 0 ;

    while((samples = vorbis_synthesis_pcmout(&context->vd, &pcm)) > 0) {
	conv(samples, pcm, (char*)data + total_bytes, context->vi.channels) ;
	total_bytes += samples * 2 * context->vi.channels ;
	total_samples += samples ;
        vorbis_synthesis_read(&context->vd, samples) ;
    }

    op->packetno++;
    *data_size = total_bytes ;   
    return buf_size ;
}


static int oggvorbis_decode_close(AVCodecContext *avccontext) {
    OggVorbisContext *context = avccontext->priv_data ;
   
    vorbis_info_clear(&context->vi) ;
    vorbis_comment_clear(&context->vc) ;

    return 0 ;
}


AVCodec oggvorbis_decoder = {
    "vorbis",
    CODEC_TYPE_AUDIO,
    CODEC_ID_VORBIS,
    sizeof(OggVorbisContext),
    oggvorbis_decode_init,
    NULL,
    oggvorbis_decode_close,
    oggvorbis_decode_frame,
} ;
