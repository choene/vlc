/*****************************************************************************
 * sofalizer.c : SOFAlizer plugin for virtual binaural acoustics
 *****************************************************************************
 * Copyright (C) 2013-2015 Andreas Fuchs, Wolfgang Hrauda,
 *                         Acoustics Research Institute (ARI), Vienna, Austria
 *
 * Authors: Andreas Fuchs <andi.fuchs.mail@gmail.com>
 *          Wolfgang Hrauda <wolfgang.hrauda@gmx.at>
 *          Christian Hoene <christian.hoene@symonics.com>
 *
 * SOFAlizer project coordinator at ARI, main developer of SOFA:
 *          Piotr Majdak <piotr@majdak.at>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
******************************************************************************/

/*****************************************************************************
 * Commonly used abbreviations in the comments:
 *   IR ... impulse response
 *  no. ... number of
 *  ch. ... channels
 * pos. ... position
 *  L/R ... left/right
******************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_aout.h>
#include <vlc_filter.h>
#include <vlc_block.h>
#include <vlc_modules.h>
#include <samplerate.h>
#include <math.h>


// Rely (if naively) on OpenAL's header for the types used for serialization.
#define AL_ALEXT_PROTOTYPES
#include "AL/al.h"
#include "AL/alext.h"

#undef NDEBUG
#include <assert.h>

#define N_SOFA 3 /* no. SOFA files loaded (for instant comparison) */
#define N_POSITIONS 4 /* no. virtual source positions (advanced settings) */
/* possible values of i_processing_type: */
#define PROC_TIME_DOM 0
#define PROC_FREQ_DOM 1
#define PROC_OPENAL   2

#ifdef __GNUC__
#  define UNUSED(x) UNUSED_ ## x __attribute__((__unused__))
#else
#  define UNUSED(x) UNUSED_ ## x
#endif

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/

#define AL_SOURCES (10)
#define AL_BUFFERS (200)

struct filter_sys_t /* field of struct filter_t, which describes the filter */
{
    /*  mutually exclusive lock */
    vlc_mutex_t lock; /* avoid interference by simultaneous threads */

    float pf_speaker_pos[10]; /* positions of the virtual loudspekaers */
    int i_n_conv; /* number of channels to convolute */
    bool b_lfe; /* whether or not the LFE channel is used */

	int samplingrate;

    /* control variables */
    /* - from GUI: */
    float axes[3]; /* filter gain (in dB) */
    float f_radius; /* distance virtual loudspeakers to listener (in metres) */
    bool b_mute; /* mutes audio output if set to true */

    /* OpenAL */
    ALCdevice *device;
    ALCcontext *context;

	ALuint sources[AL_SOURCES];
	ALuint buffers[AL_BUFFERS];

    size_t frames;
};

#if 0
struct thread_data_s /* data for audio processing of one output channel */
{
    filter_sys_t *p_sys; /* contains the filter data (see above) */
    block_t *p_in_buf; /* contains input samples buffer and information */
    int *pi_input_nb; /* points to i_input_nb (no. input ch. incl. LFE) */
    int *pi_delay; /* broadband delay for each IR to be convolved */
    int i_write; /* current write position to ringbuffer */
    int *pi_n_clippings; /* points to clippings counter (output samples >= 1) */
    float *pf_ringbuffer; /* buffers input samples, see struct filter_sys_t */
    float *pf_dest; /* points to output samples buffer (p_out_buf->p_buffer) */
                   /* (samples of L and R channel are alternating in memory) */
    float *pf_data; /* IRs/HRTFs for all channels to be convolved (excl. LFE) */
    float f_gain_lfe; /* LFE gain: gain (GUI) -6dB -3dB/ch., linear, not dB */
    int i_samplingrate;
};
#endif

static int  Open ( vlc_object_t *p_this ); /* opens the filter module */
static void Close( vlc_object_t * ); /* closes filter module, frees memory */
static block_t *DoWork( filter_t *, block_t * ); /* audio processing */

static void setSourcePositions(filter_sys_t *p_sys);

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/

#define HELP_TEXT N_( "3D Audio is using head-related transfer functions (HRTFs) to create virtual loudspeakers around the user for binaural listening via headphones (audio formats up to 8.1 supported).\nThe HRTFs are can changed (see www.sofacoustics.org for a database or mysofa.symonics.com for individualized HRTFs).\nThis modules has been developed at the Acoustics Research Institute (ARI) of the Austrian Academy of Sciences and Symonics GmbH, Germany." )

#define RADIUS_VALUE_TEXT N_( "Radius (in m)")
#define RADIUS_VALUE_LONGTEXT N_( "Varies the distance between the loudspeakers and the listener with near-field HRTFs." )

vlc_module_begin ()
    set_description( "3D Audio" )
    set_shortname( "3Daudio" )
    set_capability( "audio filter", 0)
    set_help( HELP_TEXT )
    add_float_with_range( "3D-audio-radius", 1, 0, 2.1,  RADIUS_VALUE_TEXT, RADIUS_VALUE_LONGTEXT, false )
    add_shortcut( "3D Audio" )
    set_category( CAT_AUDIO )
    set_subcategory( SUBCAT_AUDIO_AFILTER )
    set_callbacks( Open, Close )
vlc_module_end ()


/*****************************************************************************
* Callbacks
******************************************************************************/

static int HeadRotationCallback(vlc_object_t* UNUSED(p_this), char const *psz_var,
                          vlc_value_t oldval, vlc_value_t newval, void *pf_data)
{
    VLC_UNUSED(psz_var); 
    VLC_UNUSED(oldval);

    filter_t *p_filter = (filter_t *)pf_data;
    filter_sys_t *p_sys = p_filter->p_sys;
    double *array = (double*)newval.p_address;

    vlc_mutex_lock( &p_sys->lock );
    p_sys->axes[0] = array[0];
    p_sys->axes[1] = array[1];
    p_sys->axes[2] = array[2];
    setSourcePositions(p_sys);
    vlc_mutex_unlock( &p_sys->lock );

    msg_Info( p_filter, "New head_rotation %f %f %f", array[0], array[1], array[2]  );

    return VLC_SUCCESS;
}

static int RadiusCallback( vlc_object_t *UNUSED(p_this), char const *psz_var,
                          vlc_value_t oldval, vlc_value_t newval, void *pf_data )
{
    VLC_UNUSED(psz_var); 
    VLC_UNUSED(oldval);

    filter_t *p_filter = (filter_t *)pf_data;
    filter_sys_t *p_sys = p_filter->p_sys;

    vlc_mutex_lock( &p_sys->lock );
    p_sys->f_radius = newval.f_float ;
    setSourcePositions(p_sys);
    vlc_mutex_unlock( &p_sys->lock );

    msg_Info( p_filter, "New radius-value: %f", newval.f_float );
    return VLC_SUCCESS;
}



/* tait-bryan */
 
static void rotate(float *pos, float *axes)
{
	float x = pos[2];
	float y = pos[0];
	float z = pos[1];

	float yaw = axes[0] * (M_PI / 180.);			// z axis
	float pitch = axes[1] * (M_PI / 180.);			// y axis
	float roll = axes[2] * (M_PI / 180.);			// x axis

	float sin, cos;
	float nx, ny, nz;

	// rotate yaw
	sincosf(yaw, &sin, &cos);
	nx = x * cos - y * sin;
	ny = x * sin + y * cos;
	nz = z;

	// rotate pitch
	sincosf(pitch, &sin, &cos);
	x = nx * cos + nz * sin;
	y = ny;
	z = - nx * sin + nz * cos;

	// rotate roll
	sincosf(roll, &sin, &cos);
	nx = x;					
	ny = y * cos - z * sin;
	nz = y * sin + z * cos;

	printf("rotate %f %f %f with %f %f %f to %f %f %f\n", pos[2], pos[0], pos[1], axes[0], axes[1], axes[2], nx, ny, nz);
	pos[2] = nx;
	pos[0] = ny;
	pos[1] = nz;
}

/*****************************************************************************
* Set source positions
******************************************************************************/

static void setSourcePositions(filter_sys_t *p_sys)
{
	int i_n_conv = p_sys -> i_n_conv;

	if(p_sys->b_lfe) {
		alSourcei(p_sys->sources[i_n_conv], AL_SOURCE_RELATIVE, AL_TRUE);
	//	alSourcei(p_sys->sources[i_n_conv], AL_SOURCE_TYPE, AL_STREAMING);
    		alSource3f(p_sys->sources[i_n_conv], AL_POSITION, 0.0f, 0.0f, -1.0f);
    		assert(alGetError()==AL_NO_ERROR && "Failed to setup sound source2");
	}
	printf("setSourcePositions\n");

	for(int i=0;i<i_n_conv;i++) {
		alSourcei(p_sys->sources[i], AL_SOURCE_RELATIVE, AL_TRUE);
  		assert(alGetError()==AL_NO_ERROR && "Failed to setup sound source");
	//	alSourcei(p_sys->sources[i], AL_SOURCE_TYPE, AL_STREAMING);
   		assert(alGetError()==AL_NO_ERROR && "Failed to setup sound source");

		double angle = p_sys->pf_speaker_pos[i] * (M_PI / 180.);
		printf("speaker %d %f %u\n",i,angle,p_sys->sources[i]);
		float pos[3];
		pos[0] = sin(angle) * p_sys->f_radius;
		pos[1] = 0.0f;
		pos[2] = -cos(angle) * p_sys->f_radius;
		rotate(pos, p_sys->axes);
	        alSource3f(p_sys->sources[i], AL_POSITION, pos[0], pos[1], pos[2]);
    		assert(alGetError()==AL_NO_ERROR && "Failed to setup sound source");
	}
	
}

/*****************************************************************************
* Open:
******************************************************************************/


static int Open( vlc_object_t *p_this )
{
    	filter_t *p_filter = (filter_t *)p_this;

	/**********************************************
	 * alloc and init p_sys */
    	filter_sys_t *p_sys = p_filter->p_sys = malloc( sizeof( *p_sys ) );
    	if( unlikely( p_sys == NULL ) )
        	return VLC_ENOMEM;
    	bzero(p_sys, sizeof( *p_sys ));

    	p_sys->axes[0] = p_sys->axes[1] = p_sys->axes[2] = 0;
    	p_sys->f_radius = 1; 
    	p_sys->b_mute = false;
	vlc_mutex_init( &p_sys->lock );


	/**********************************************
	 * get input parameter */

	/* get sampling rate of audio file/stream: */
 	p_sys->samplingrate = p_filter->fmt_in.audio.i_rate;
	p_filter->fmt_in.audio.i_format = VLC_CODEC_FL32;

    	int i_input_nb = aout_FormatNbChannels( &p_filter->fmt_in.audio ); /* no. input channels */
    	if ( p_filter->fmt_in.audio.i_physical_channels & AOUT_CHAN_LFE ) /* if LFE is used */
    	{
        	p_sys->b_lfe = true;
        	p_sys->i_n_conv = i_input_nb - 1 ; /* LFE is an input channel but requires no convolution */
    	}
    	else /* if LFE is not used */
    	{
        	p_sys->b_lfe = false;
        	p_sys->i_n_conv = i_input_nb ;
    	}
    /* set speaker positions according to input channel configuration: */
    	switch (  p_filter->fmt_in.audio.i_physical_channels )
    	{
    	case AOUT_CHAN_CENTER:  
		p_sys->pf_speaker_pos[0] = 0;
 		break;
    	case AOUT_CHANS_8_1:    
		p_sys->pf_speaker_pos[6] = 180;
    	case AOUT_CHANS_7_0:
    	case AOUT_CHANS_7_1:
    	case AOUT_CHANS_6_0: 	   
		p_sys->pf_speaker_pos[5] = 210;
		p_sys->pf_speaker_pos[4] = 150;
		p_sys->pf_speaker_pos[3] = 270;
		p_sys->pf_speaker_pos[2] = 90;
		p_sys->pf_speaker_pos[1] = 330;
		p_sys->pf_speaker_pos[0] = 30;
		break;
    	case AOUT_CHANS_5_0:
    	case AOUT_CHANS_5_1:
    	case ( AOUT_CHANS_5_0_MIDDLE | AOUT_CHAN_LFE ):
    	case AOUT_CHANS_5_0_MIDDLE: 
    	case AOUT_CHANS_4_0:
    	case AOUT_CHANS_4_1:
		p_sys->pf_speaker_pos[3] = 240;
		p_sys->pf_speaker_pos[2] = 120;
    	case AOUT_CHANS_3_0:
    	case AOUT_CHANS_3_1:
    	case AOUT_CHANS_STEREO:
    	case AOUT_CHANS_2_1:    
		p_sys->pf_speaker_pos[1] = 330;
		p_sys->pf_speaker_pos[0] = 30;
                break;
    	default: 
	        msg_Err (p_filter, "Couldn't get speaker positions. Input channel configuration not supported. ");
		Close(p_this);
		return VLC_EGENERIC;
    	}

	/**********************************************
	 * set output format */

	/* set filter settings and calculate speaker positions */
    	p_filter->fmt_out.audio = p_filter->fmt_in.audio;

    	/* set physical channels to stereo, required for filter output set to stereo */
    	p_filter->fmt_out.audio.i_physical_channels = AOUT_CHANS_STEREO;
    	p_filter->fmt_out.audio.i_original_channels = AOUT_CHANS_STEREO;

	/**********************************************
	 * use OpenAL */

	if(!alcIsExtensionPresent(NULL, "ALC_SOFT_loopback")) {
        	msg_Err (p_filter, "OpenAL does not support loopback device. ");
		Close(p_this);
        	return VLC_EGENERIC;
	}

    	p_sys->device = alcLoopbackOpenDeviceSOFT(NULL);
    	if(p_sys->device == NULL) {
        	msg_Err (p_filter, "Couldn't get OpenAL device. ");
		Close(p_this);
        	return VLC_EGENERIC;
    	}

	ALCint attrs[7];
	attrs[0] = ALC_FORMAT_CHANNELS_SOFT;
	attrs[1] = ALC_STEREO_SOFT;
	attrs[2] = ALC_FORMAT_TYPE_SOFT;
	attrs[3] = ALC_FLOAT_SOFT;
	attrs[4] = ALC_FREQUENCY;
	attrs[5] = p_sys->samplingrate;
	attrs[6] = 0;

	if(alcIsRenderFormatSupportedSOFT(p_sys->device, attrs[5], attrs[1], attrs[3]) == ALC_FALSE) {
        	msg_Err (p_filter, "Render format not supported %04X %04X %dHz\n",attrs[1],attrs[3],attrs[5]);
		Close(p_this);
	        return VLC_EGENERIC;
	}

    	p_sys->context = alcCreateContext(p_sys->device, attrs);
    	if(p_sys->context == NULL || alcMakeContextCurrent(p_sys->context) == ALC_FALSE)
    	{
        	msg_Err (p_filter, "Could not set OpenAL context. ");
		Close(p_this);
	        return VLC_EGENERIC;
 	}

#if 0
    	if(!alcIsExtensionPresent(p_sys->device, "ALC_SOFT_HRTF"))
    	{
        	msg_Err (p_filter, "Error: ALC_SOFT_HRTF not supported. ");
		Close(p_this);
	        return VLC_EGENERIC;
	}


    /* Enumerate available HRTFs, and reset the device using one. */
    	alcGetIntegerv(p_sys->device, ALC_NUM_HRTF_SPECIFIERS_SOFT, 1, &num_hrtf);
    	if(!num_hrtf)
        	printf("No HRTFs found\n");
    	else
    	{
        	ALCint attr[5];
        	ALCint index = -1;
        	ALCint i;

        	printf("Available HRTFs:\n");
        	for(i = 0;i < num_hrtf;i++)
        	{
            		const ALCchar *name = alcGetStringiSOFT(p_sys_>device, ALC_HRTF_SPECIFIER_SOFT, i);
	            	printf("    %d: %s\n", i, name);

	            /* Check if this is the HRTF the user requested. */
	            if(hrtfname && strcmp(name, hrtfname) == 0)
	                index = i;
	        }

	        i = 0;
	        attr[i++] = ALC_HRTF_SOFT;
	        attr[i++] = ALC_TRUE;
	        if(index == -1)
	        {
 	           if(hrtfname)
 	               printf("HRTF \"%s\" not found\n", hrtfname);
 	           printf("Using default HRTF...\n");
 	       }
 	       else
 	       {
        	    printf("Selecting HRTF %d...\n", index);
	            attr[i++] = ALC_HRTF_ID_SOFT;
	            attr[i++] = index;
	        }
	        attr[i] = 0;

	        if(!alcResetDeviceSOFT(p_sys->device, attr))
	            printf("Failed to reset device: %s\n", alcGetString(p_sys->device, alcGetError(device)));
    }

    /* Check if HRTF is enabled, and show which is being used. */
    alcGetIntegerv(p_sys->device, ALC_HRTF_SOFT, 1, &hrtf_state);
    if(!hrtf_state)
        printf("HRTF not enabled!\n");
    else
    {
        const ALchar *name = alcGetString(p_sys->device, ALC_HRTF_SPECIFIER_SOFT);
        printf("HRTF enabled, using %s\n", name);
    }
#endif

    /* Create the sources and buffers to play the sound with. */
    	alGenSources(AL_SOURCES, p_sys->sources);
	alGenBuffers(AL_BUFFERS, p_sys->buffers);

 	assert(alGetError()==AL_NO_ERROR && "Failed to gen sources and buffers");



    /* get user settings */
    p_sys->f_radius     = var_CreateGetFloat( p_filter->obj.parent, "sofalizer-radius");

    /* Callbacks can call function LoadData */
    var_AddCallback( p_filter->obj.parent, "sofalizer-radius", RadiusCallback, p_filter );
    var_AddCallback( p_filter->obj.libvlc, "head-rotation", HeadRotationCallback, p_filter );

	setSourcePositions(p_sys);

 	assert(alGetError()==AL_NO_ERROR && "Failed to gen sources and buffers");
	p_filter->pf_audio_filter = DoWork; /* DoWork does the audio processing */


    return VLC_SUCCESS;
}

/*****************************************************************************
* DoWork: Prepares the data structures for the threads and starts them
* sofalizer_Convolute: Writes the samples of the input buffer to the ringbuffer
* and convolutes with the impulse response
* sofalizer_FastConvolution: Writes the samples of the input buffer to the ring
* buffer, transforms it to frequency domain using FFT, multiplies with filter
* frequency response and transforms result back to time domain
******************************************************************************/

static block_t *DoWork( filter_t *p_filter, block_t *p_in_buf )
{
    struct filter_sys_t *p_sys = p_filter->p_sys; /* get pointer to filter_t struct */

    /* get number of input and output channels*/
    int i_input_nb = aout_FormatNbChannels( &p_filter->fmt_in.audio );
    int i_output_nb = aout_FormatNbChannels( &p_filter->fmt_out.audio );

    /* prepare output buffer: output buffer size is input buffer size */
    /*                        scaled according to number of in/out channels */
    size_t i_out_size = p_in_buf->i_buffer * i_output_nb / i_input_nb;
    block_t *p_out_buf = block_Alloc( i_out_size ); /* get output buffer memory */
    if ( unlikely( !p_out_buf ) )
    {
        msg_Warn( p_filter, "Can't get output buffer." );
        goto out;
    }
    /* set output buffer parameters */
    p_out_buf->i_nb_samples = p_in_buf->i_nb_samples;
    p_out_buf->i_dts        = p_in_buf->i_dts;
    p_out_buf->i_pts        = p_in_buf->i_pts;
    p_out_buf->i_length     = p_in_buf->i_length;

    		assert(alGetError()==AL_NO_ERROR && "prestate");

    if ( unlikely( p_sys->b_mute ) ) /* mutes output (e.g. invalid SOFA file selected) */
    {
        memset( p_out_buf->p_buffer, 0 , sizeof( float ) * p_in_buf->i_nb_samples * 2 );
    }
    else {
     	short data[p_in_buf->i_nb_samples];

     	for ( int l = 0 ; l < i_input_nb ; l++ ) {
    	
	    float *p = (float*)p_in_buf->p_buffer;
	    p += l;
	    for ( size_t i = 0 ; i < p_in_buf->i_nb_samples ; i++) {
		data[i] = p[i*i_input_nb] * 32767.f;
	    }
 	    	ALuint buffer = p_sys->buffers[p_sys->frames % AL_BUFFERS];
		if(p_sys->frames >= AL_BUFFERS) {
	    		alSourceUnqueueBuffers(p_sys->sources[l], 1, &buffer);
	 		assert(alGetError()==AL_NO_ERROR && "Failed to unqueue buffer");
		}
		p_sys->frames++;
		
		alBufferData(buffer, AL_FORMAT_MONO16, data, p_in_buf->i_nb_samples * sizeof(short), p_sys->samplingrate);
    		assert(alGetError()==AL_NO_ERROR && "Failed to buffer data");

    		alSourceQueueBuffers(p_sys->sources[l], 1, &buffer);
 		assert(alGetError()==AL_NO_ERROR && "Failed to queue buffer");
  
	}
	if(p_sys->frames==i_input_nb) {
        	memset( p_out_buf->p_buffer, 0 , sizeof( float ) * p_in_buf->i_nb_samples * 2 );
		for ( int l = 0 ; l < i_input_nb ; l++ ) {
			alSourcePlay(p_sys->sources[l]);
 			assert(alGetError()==AL_NO_ERROR && "Failed to start playing");
		} 
	}
	else {
	    	alcRenderSamplesSOFT(p_sys->device, (void*)p_out_buf->p_buffer,
                             p_in_buf->i_nb_samples);

   		assert(alGetError()==AL_NO_ERROR && "Failed to render");
	}
    }
out:
    block_Release( p_in_buf );
    return p_out_buf; /* DoWork returns the modified output buffer */
}

/*****************************************************************************
* Close:
******************************************************************************/

static void Close( vlc_object_t *p_this )
{
    filter_t *p_filter = ( filter_t* )p_this;
    filter_sys_t *p_sys = p_filter->p_sys;
    vlc_object_t *p_out = p_filter->obj.parent;

    	alDeleteSources(AL_SOURCES, p_sys->sources);
    	alDeleteBuffers(AL_BUFFERS, p_sys->buffers);


    alcMakeContextCurrent(NULL);
    if(p_sys->context != NULL)
    	alcDestroyContext(p_sys->context);
    if(p_sys->device != NULL)
    	alcCloseDevice(p_sys->device);

    /* delete GUI callbacks */
    var_DelCallback( p_out, "sofalizer-radius", RadiusCallback, p_filter );
    var_DelCallback( p_filter->obj.libvlc, "head-rotation", HeadRotationCallback, p_filter );

    vlc_mutex_destroy( &p_sys->lock ); /* get rid of mutex lock */

    free( p_sys );
}

