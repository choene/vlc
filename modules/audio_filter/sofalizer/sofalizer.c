﻿/*****************************************************************************
 * sofalizer.c : SOFAlizer plugin for virtual binaural acoustics
 *****************************************************************************
 * Copyright (C) 2013-2015 Andreas Fuchs, Wolfgang Hrauda,
 *                         Acoustics Research Institute (ARI), Vienna, Austria
 *
 * Authors: Andreas Fuchs <andi.fuchs.mail@gmail.com>
 *          Wolfgang Hrauda <wolfgang.hrauda@gmx.at>
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
#include <modules/audio_filter/sofalizer/resampling/samplerate.h>
#include <modules/audio_filter/sofalizer/fft/kiss_fft.h>

#include <math.h>
#include <netcdf.h>

#define N_SOFA 3 /* no. SOFA files loaded (for instant comparison) */
#define N_POSITIONS 4 /* no. virtual source positions (advanced settings) */
/* possible values of i_processing_type: */
#define PROC_TIME_DOM 0
#define PROC_FREQ_DOM 1

#ifdef __GNUC__
#  define UNUSED(x) UNUSED_ ## x __attribute__((__unused__))
#else
#  define UNUSED(x) UNUSED_ ## x
#endif


/*****************************************************************************
 * Local prototypes
 *****************************************************************************/

struct nc_sofa_s /* contains data of one SOFA file */
{
   int i_ncid; /* netCDF ID of the opened SOFA file */
   int i_n_samples; /* length of one impulse response (IR) */
   int i_m_dim; /* number of measurement positions */
   int *pi_data_delay; /* broadband delay of each IR */
   /* all measurement positions for each receiver (i.e. ear): */
   float *pf_sp_a; /* azimuth angles */
   float *pf_sp_e; /* elevation angles */
   float *pf_sp_r; /* radii */
   /* dataat at each measurement position for each receiver: */
   float *pf_data_ir; /* IRs (time-domain) */
};

struct filter_sys_t /* field of struct filter_t, which describes the filter */
{
    struct nc_sofa_s sofa[N_SOFA]; /* contains data of the SOFA files */

    /*  mutually exclusive lock */
    vlc_mutex_t lock; /* avoid interference by simultaneous threads */

    float *pf_speaker_pos; /* positions of the virtual loudspekaers */

    int i_n_conv; /* number of channels to convolute */

    /* buffer variables (for convolution) */
    float *pf_ringbuffer_l; /* buffers input samples, length of one buffer: */
    float *pf_ringbuffer_r; /* no. input ch. (incl. LFE) x i_buffer_length */
    int i_write; /* current write position to ringbuffer */
    int i_buffer_length; /* is: longest IR plus max. delay in all SOFA files */
                         /* then choose next power of 2 */
    int i_n_longest_filter; /* longest IR + max. delay in all SOFA files */
    int i_output_buffer_length; /* remember no. samples in output buffer */
    int i_n_fft; /* no. samples in one FFT block */

    /* KissFFT configuration variables */
    kiss_fft_cfg p_fft_cfg;
    kiss_fft_cfg p_ifft_cfg;

    /* netCDF variables */
    int i_sofa_id;  /* selected SOFA file (zero-based; unlike GUI "Select"!) */
    int *pi_delay_l; /* broadband delay for each channel/IR to be convolved */
    int *pi_delay_r;
    float *pf_data_ir_l; /* IRs for all channels to be convolved */
    float *pf_data_ir_r; /* (this excludes the LFE) */
    kiss_fft_cpx *p_data_hrtf_l; /* HRTFs for all channels to be convolved */
    kiss_fft_cpx *p_data_hrtf_r; /* (this excludes the LFE) */

    /* control variables */
    /* - from GUI: */
    float f_gain; /* filter gain (in dB) */
    float f_rotation; /* rotation of virtual loudspeakers (in degrees)  */
    float f_elevation; /* elevation of virtual loudspeakers (in deg.) */
    float f_radius; /* distance virtual loudspeakers to listener (in metres) */
    int i_switch; /* 0: source positions according to input format plus */
                  /* user's rotation and elevation settings on GUI, */
                  /* 1-4: virtual source pos. defined in advanced settings */
    /* - from advanced settings: virtual source positions: */
    int pi_azimuth_array[N_POSITIONS]; /* azimuth angles (in deg.) */
    int pi_elevation_array[N_POSITIONS]; /* elevation angles (in deg.) */
    int i_processing_type; /* type of audio processing algorithm used */
    int i_resampling_type; /* quality of libsamplerate conversion */

    bool b_mute; /* mutes audio output if set to true */
    bool b_lfe; /* whether or not the LFE channel is used */
};

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
};

/* constants for audio processing type menu (in advanced settings) */
static const int pi_processing_type_values[] =
{
    PROC_TIME_DOM, PROC_FREQ_DOM,
};
static const char *const psz_processing_type_texts[] =
{
    N_("Time-domain convolution"), N_("Fast Convolution"),
};

/* constants for resampling quality menu (in advanced settings) */
static const int pi_resampling_type_values[] =
{
    SRC_SINC_BEST_QUALITY, SRC_SINC_MEDIUM_QUALITY, SRC_SINC_FASTEST,
    SRC_ZERO_ORDER_HOLD, SRC_LINEAR,
};
static const char *const psz_resampling_type_texts[] =
    {
    N_("Sinc function (best quality)"), N_("Sinc function (medium quality)"),
    N_("Sinc function (fast)"), N_("Zero Order Hold (fastest)"),
    N_("Linear (fastest)"),
};

static int  Open ( vlc_object_t *p_this ); /* opens the filter module */
static void Close( vlc_object_t * ); /* closes filter module, frees memory */
static block_t *DoWork( filter_t *, block_t * ); /* audio processing */

static int LoadData ( filter_t *p_filter, int i_azim, int i_elev, float f_radius);
void sofalizer_Convolute ( void *data );
int sofalizer_FastConvolution( void *data_l, void *data_r );
static int FindM ( filter_sys_t *p_sys, int i_azim, int i_elev, float f_radius );

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/

#define HELP_TEXT N_( "SOFAlizer uses head-related transfer functions (HRTFs) to create virtual loudspeakers around the user for binaural listening via headphones (audio formats up to 8.1 supported).\nThe HRTFs are stored in SOFA files (see www.sofacoustics.org for a database).\nSOFAlizer is developed at the Acoustics Research Institute (ARI) of the Austrian Academy of Sciences." )

#define GAIN_VALUE_TEXT N_( "Gain [dB]" )
#define GAIN_VALUE_LONGTEXT NULL

#define FILE1_NAME_TEXT N_( "SOFA file 1" )
#define FILE2_NAME_TEXT N_( "SOFA file 2" )
#define FILE3_NAME_TEXT N_( "SOFA file 3" )

#define FILE_NAME_LONGTEXT N_( "Three different SOFA files can be specified and used for instant comparison during playback." )

#define SELECT_VALUE_TEXT N_( "Select SOFA file used for rendering:" )
#define SELECT_VALUE_LONGTEXT N_( "SOFAlizer allows to load 3 different SOFA files and easily switch between them for better comparison." )

#define ROTATION_VALUE_TEXT N_( "Rotation (in deg)" )
#define ROTATION_VALUE_LONGTEXT N_( "Rotates virtual loudspeakers." )

#define ELEVATION_VALUE_TEXT N_( "Elevation (in deg)" )
#define ELEVATION_VALUE_LONGTEXT N_( "Elevates the virtual loudspeakers." )

#define RADIUS_VALUE_TEXT N_( "Radius (in m)")
#define RADIUS_VALUE_LONGTEXT N_( "Varies the distance between the loudspeakers and the listener with near-field HRTFs." )

#define SWITCH_VALUE_TEXT N_( "Switch" )
#define SWITCH_VALUE_LONGTEXT N_( "Presents all audio channels from one of four pre-defined virtual positions. Position 0 activates Rotation and Elevation." )

#define POS_VALUE_LONGTEXT N_( "Only active for Switch 1-4." )

#define POS1_AZIMUTH_VALUE_TEXT N_( "Azimuth Position 1 ")
#define POS1_ELEVATION_VALUE_TEXT N_( "Elevation Position 1 ")
#define POS2_AZIMUTH_VALUE_TEXT N_( "Azimuth Position 2 ")
#define POS2_ELEVATION_VALUE_TEXT N_( "Elevation Position 2 ")
#define POS3_AZIMUTH_VALUE_TEXT N_( "Azimuth Position 3 ")
#define POS3_ELEVATION_VALUE_TEXT N_( "Elevation Position 3 ")
#define POS4_AZIMUTH_VALUE_TEXT N_( "Azimuth Position 4 ")
#define POS4_ELEVATION_VALUE_TEXT N_( "Elevation Position 4 ")

#define PROCESSING_TYPE_VALUE_TEXT N_( "Audio Processing Type ")
#define PROCESSING_TYPE_VALUE_LONGTEXT NULL
#define RESAMPLING_TYPE_VALUE_TEXT N_( "HRTF Resampling Algorithm Type ")
#define RESAMPLING_TYPE_VALUE_LONGTEXT N_( "High quality will result in slower conversion times. Fast conversion means medicore quality.")

vlc_module_begin ()
    set_description( "SOFAlizer" )
    set_shortname( "SOFAlizer" )
    set_capability( "audio filter", 0)
    set_help( HELP_TEXT )
    add_loadfile( "sofalizer-filename1", "", FILE1_NAME_TEXT, FILE_NAME_LONGTEXT, false)
    add_loadfile( "sofalizer-filename2", "", FILE2_NAME_TEXT, FILE_NAME_LONGTEXT, false)
    add_loadfile( "sofalizer-filename3", "", FILE3_NAME_TEXT, FILE_NAME_LONGTEXT, false)
    add_float_with_range( "sofalizer-select", 1 , 1 , 3,  SELECT_VALUE_TEXT, SELECT_VALUE_LONGTEXT, false)
    add_float_with_range( "sofalizer-gain", 0.0, -20, 40,  GAIN_VALUE_TEXT, GAIN_VALUE_LONGTEXT, false )
    add_float_with_range( "sofalizer-rotation", 0, -360, 360, ROTATION_VALUE_TEXT, ROTATION_VALUE_LONGTEXT, false )
    add_float_with_range( "sofalizer-elevation", 0, -90, 90, ELEVATION_VALUE_TEXT, ELEVATION_VALUE_LONGTEXT, false )
    add_float_with_range( "sofalizer-radius", 1, 0, 2.1,  RADIUS_VALUE_TEXT, RADIUS_VALUE_LONGTEXT, false )
    add_float_with_range( "sofalizer-switch", 0, 0, 4, SWITCH_VALUE_TEXT, SWITCH_VALUE_LONGTEXT, false )
    add_integer_with_range( "sofalizer-pos1-azi", 90, -180, 180,POS1_AZIMUTH_VALUE_TEXT, POS_VALUE_LONGTEXT, false )
    add_integer_with_range( "sofalizer-pos1-ele", 0, -90, 90, POS1_ELEVATION_VALUE_TEXT, POS_VALUE_LONGTEXT, false )
    add_integer_with_range( "sofalizer-pos2-azi", 180, -180, 180, POS2_AZIMUTH_VALUE_TEXT, POS_VALUE_LONGTEXT, false )
    add_integer_with_range( "sofalizer-pos2-ele", 0, -90, 90, POS2_ELEVATION_VALUE_TEXT, POS_VALUE_LONGTEXT, false )
    add_integer_with_range( "sofalizer-pos3-azi", -90, -180, 180, POS3_AZIMUTH_VALUE_TEXT, POS_VALUE_LONGTEXT, false )
    add_integer_with_range( "sofalizer-pos3-ele", 0, -90, 90, POS3_ELEVATION_VALUE_TEXT, POS_VALUE_LONGTEXT, false )
    add_integer_with_range( "sofalizer-pos4-azi", 0, -180, 180, POS4_AZIMUTH_VALUE_TEXT, POS_VALUE_LONGTEXT, false )
    add_integer_with_range( "sofalizer-pos4-ele", 90, -90, 90, POS4_ELEVATION_VALUE_TEXT, POS_VALUE_LONGTEXT, false )
    add_integer ("sofalizer-processing-type", 1,
        PROCESSING_TYPE_VALUE_TEXT, PROCESSING_TYPE_VALUE_LONGTEXT, true)
    change_integer_list (pi_processing_type_values, psz_processing_type_texts)
    add_integer ("sofalizer-resampling-type", SRC_SINC_FASTEST,
        RESAMPLING_TYPE_VALUE_TEXT, RESAMPLING_TYPE_VALUE_LONGTEXT, true)
    change_integer_list (pi_resampling_type_values, psz_resampling_type_texts)
    add_shortcut( "sofalizer" )
    set_category( CAT_AUDIO )
    set_subcategory( SUBCAT_AUDIO_AFILTER )
    set_callbacks( Open, Close )
vlc_module_end ()

/*****************************************************************************
* CloseSofa: Closes the given SOFA file and frees its allocated memory.
* LoadSofa: Loads the given SOFA file, check for the most important SOFAconventions
*     and load the whole IR Data, Source-Positions and Delays
* GetSpeakerPos: Get the Speaker Positions for current input.
* MaxDelay: Find the Maximum Delay in the Sofa File
* CompensateVolume: Compensate the Volume of the Sofa file. The Energy of the
*     IR closest to ( 0°, 0°, 1m ) to the left ear is calculated.
* FreeAllSofa: Frees Memory allocated in LoadSofa of all Sofa files
* FreeFilter: Frees Memory allocated in Open
******************************************************************************/

static int CloseSofa ( struct nc_sofa_s *sofa )
{   /* close given SOFA file and free associated memory: */
    free( sofa->pi_data_delay );
    free( sofa->pf_sp_a );
    free( sofa->pf_sp_e );
    free( sofa->pf_sp_r );
    free( sofa->pf_data_ir );
    nc_close( sofa->i_ncid );
    sofa->i_ncid = 0;
    return VLC_SUCCESS;
}

static int LoadSofa ( filter_t *p_filter, char *psz_filename, /* loads one file*/
                      int i_sofa_id , int *p_samplingrate)
{
    struct filter_sys_t *p_sys = p_filter->p_sys;
    /* variables associated with content of SOFA file: */
    int i_ncid, i_n_dims, i_n_vars, i_n_gatts, i_n_unlim_dim_id, i_status;
    unsigned int i_samplingrate;
    int i_n_samples = 0;
    int i_m_dim = 0;
    p_sys->sofa[i_sofa_id].i_ncid = 0;
    i_status = nc_open( psz_filename , NC_NOWRITE, &i_ncid); /* open SOFA file read-only */
    if (i_status != NC_NOERR)
    {
        msg_Err(p_filter, "Can't find SOFA-file '%s'", psz_filename);
        return VLC_EGENERIC;
    }
    /* get number of dimensions, vars, global attributes and Id of unlimited dimensions: */
    nc_inq(i_ncid, &i_n_dims, &i_n_vars, &i_n_gatts, &i_n_unlim_dim_id);

    /* -- get number of measurements ("M") and length of one IR ("N") -- */
    char psz_dim_names[i_n_dims][NC_MAX_NAME];   /* names of netCDF dimensions */
    size_t pi_dim_length[i_n_dims]; /* lengths of netCDF dimensions */
    int i_m_dim_id = -1;
    int i_n_dim_id = -1;
    for( int i = 0; i < i_n_dims; i++ ) /* go through all dimensions of file */
    {
        nc_inq_dim( i_ncid, i, psz_dim_names[i], &pi_dim_length[i] ); /* get dimensions */
        if ( !strncmp("M", psz_dim_names[i], 1 ) ) /* get ID of dimension "M" */
            i_m_dim_id = i;
        if ( !strncmp("N", psz_dim_names[i], 1 ) ) /* get ID of dimension "N" */
            i_n_dim_id = i;
    }
    if( ( i_m_dim_id == -1 ) || ( i_n_dim_id == -1 ) ) /* dimension "M" or "N" couldn't be found */
    {
        msg_Err(p_filter, "Can't find required dimensions in SOFA file.");
        nc_close(i_ncid);
        return VLC_EGENERIC;
    }
    i_n_samples = pi_dim_length[i_n_dim_id]; /* get number of measurements */
    i_m_dim =  pi_dim_length[i_m_dim_id]; /* get length of one IR */

    /* -- check file type -- */
    size_t i_att_len; /* get length of attritube "Conventions" */
    i_status = nc_inq_attlen(i_ncid, NC_GLOBAL, "Conventions", &i_att_len);
    if (i_status != NC_NOERR)
    {
        msg_Err(p_filter, "Can't get length of attribute \"Conventions\".");
        nc_close(i_ncid);
        return VLC_EGENERIC;
    }
    char psz_conventions[i_att_len + 1]; /* check whether file is SOFA file */
    nc_get_att_text( i_ncid , NC_GLOBAL, "Conventions", psz_conventions);
    *( psz_conventions + i_att_len ) = 0;
    if ( strncmp( "SOFA" , psz_conventions, 4 ) )
    {
        msg_Err(p_filter, "Not a SOFA file!");
        nc_close(i_ncid);
        return VLC_EGENERIC;
    }

    /* -- check if attribute "SOFAConventions" is "SimpleFreeFieldHRIR": -- */
    nc_inq_attlen (i_ncid, NC_GLOBAL, "SOFAConventions", &i_att_len );
    char psz_sofa_conventions[i_att_len + 1];
    nc_get_att_text(i_ncid, NC_GLOBAL, "SOFAConventions", psz_sofa_conventions);
    *( psz_sofa_conventions + i_att_len ) = 0;
    if ( strncmp( "SimpleFreeFieldHRIR" , psz_sofa_conventions, i_att_len ) )
    {
       msg_Err(p_filter, "Not a SimpleFreeFieldHRIR file!");
       nc_close(i_ncid);
       return VLC_EGENERIC;
    }

    /* -- get sampling rate of HRTFs -- */
    int i_samplingrate_id; /* read ID, then value */
    i_status = nc_inq_varid( i_ncid, "Data.SamplingRate", &i_samplingrate_id);
    i_status += nc_get_var_uint( i_ncid, i_samplingrate_id, &i_samplingrate );
    if (i_status != NC_NOERR)
    {
        msg_Err(p_filter, "Couldn't read Data.SamplingRate.");
        nc_close(i_ncid);
        return VLC_EGENERIC;
    }
    *p_samplingrate = i_samplingrate; /* remember sampling rate */

    /* -- allocate memory for one value for each measurement position: -- */
    float *pf_sp_a = p_sys->sofa[i_sofa_id].pf_sp_a = malloc( sizeof(float) * i_m_dim);
    float *pf_sp_e = p_sys->sofa[i_sofa_id].pf_sp_e = malloc( sizeof(float) * i_m_dim);
    float *pf_sp_r = p_sys->sofa[i_sofa_id].pf_sp_r = malloc( sizeof(float) * i_m_dim);
    /* delay and IR values required for each ear and measurement position: */
    int *pi_data_delay = p_sys->sofa[i_sofa_id].pi_data_delay =
        calloc ( i_m_dim * 2, sizeof( int ) );
    float *pf_data_ir = p_sys->sofa[i_sofa_id].pf_data_ir =
        malloc( sizeof( float ) * 2 * i_m_dim * i_n_samples );

    if ( !pi_data_delay || !pf_sp_a || !pf_sp_e || !pf_sp_r || !pf_data_ir )
    {   /* if memory could not be allocated */
        CloseSofa( &p_sys->sofa[i_sofa_id] );
        return VLC_ENOMEM;
    }

    /* get impulse responses (HRTFs): */
    int i_data_ir_id; /* get corresponding ID */
    i_status = nc_inq_varid( i_ncid, "Data.IR", &i_data_ir_id);
    i_status += nc_get_var_float( i_ncid, i_data_ir_id, pf_data_ir ); /* read and store IRs */
    if ( i_status != NC_NOERR )
    {
        msg_Err( p_filter, "Couldn't read Data.IR!" );
        goto error;
    }

    /* get source positions of the HRTFs in the SOFA file: */
    int i_sp_id;
    i_status = nc_inq_varid(i_ncid, "SourcePosition", &i_sp_id); /* get corresponding ID */
    i_status += nc_get_vara_float (i_ncid, i_sp_id, (size_t[2]){ 0 , 0 } ,
                (size_t[2]){ i_m_dim , 1 } , pf_sp_a ); /* read & store azimuth angles */
    i_status += nc_get_vara_float (i_ncid, i_sp_id, (size_t[2]){ 0 , 1 } ,
                (size_t[2]){ i_m_dim , 1 } , pf_sp_e ); /* read & store elevation angles */
    i_status += nc_get_vara_float (i_ncid, i_sp_id, (size_t[2]){ 0 , 2 } ,
                (size_t[2]){ i_m_dim , 1 } , pf_sp_r ); /* read & store radii */
    if (i_status != NC_NOERR) /* if any source position variable coudn't be read */
    {
        msg_Err(p_filter, "Couldn't read SourcePosition.");
        goto error;
    }

    /* read Data.Delay, check for errors and fit it to pi_data_delay */
    int i_data_delay_id;
    int pi_data_delay_dim_id[2];
    char psz_data_delay_dim_name[NC_MAX_NAME];

    i_status = nc_inq_varid(i_ncid, "Data.Delay", &i_data_delay_id);
    i_status += nc_inq_vardimid ( i_ncid, i_data_delay_id, &pi_data_delay_dim_id[0]);
    i_status += nc_inq_dimname ( i_ncid, pi_data_delay_dim_id[0], psz_data_delay_dim_name );
    if (i_status != NC_NOERR)
    {
        msg_Err(p_filter, "Couldn't read Data.Delay." );
        goto error;
    }

    /* Data.Delay dimension check */
       /* dimension of Data.Delay is [I R]: */
    if ( !strncmp ( psz_data_delay_dim_name, "I", 2 ) )
    { /* check 2 characters to assure string is 0-terminated after "I" */
        msg_Dbg ( p_filter, "Data.Delay has dimension [I R]" );
        int pi_Delay[2]; /* delays get from SOFA file: */
        i_status = nc_get_var_int( i_ncid, i_data_delay_id, &pi_Delay[0] );
        if ( i_status != NC_NOERR )
        {
            msg_Err(p_filter, "Couldn't read Data.Delay");
            goto error;
        }
        int *pi_data_delay_r = pi_data_delay + i_m_dim;
        for ( int i = 0 ; i < i_m_dim ; i++ ) /* extend given dimension [I R] to [M R] */
        { /* assign constant delay value for all measurements to data_delay fields */
            *( pi_data_delay + i ) = pi_Delay[0];
            *( pi_data_delay_r + i ) = pi_Delay[1];
        }
    }
      /* dimension of Data.Delay is [M R] */
    else if ( !strncmp ( psz_data_delay_dim_name, "M", 2 ) )
    {
        msg_Dbg( p_filter, "Data.Delay in dimension [M R]");
        /* get delays from SOFA file: */
        i_status = nc_get_var_int( i_ncid, i_data_delay_id, pi_data_delay );
        if (i_status != NC_NOERR)
        {
            msg_Err(p_filter, "Couldn't read Data.Delay");
            goto error;
        }
    }
    else /* dimension of Data.Delay is neither [I R] nor [M R] */
    {
        msg_Err ( p_filter,
        "Data.Delay does not have the required dimensions [I R] or [M R].");
        goto error;
    }

    /* save information in SOFA struct: */
    p_sys->sofa[i_sofa_id].i_m_dim = i_m_dim; /* no. measurement positions */
    p_sys->sofa[i_sofa_id].i_n_samples = i_n_samples; /* length on one IR */
    p_sys->sofa[i_sofa_id].i_ncid = i_ncid; /* netCDF ID of SOFA file */
    nc_close(i_ncid); /* close SOFA file */

    return VLC_SUCCESS;

error:
    CloseSofa( &p_sys->sofa[i_sofa_id] );
    return VLC_EGENERIC;
}

static int GetSpeakerPos ( filter_t *p_filter, float *pf_speaker_pos )
{
    /* get input channel configuration: */
    uint16_t i_physical_channels = p_filter->fmt_in.audio.i_physical_channels;
    float *pf_pos_temp;
    int i_input_nb = aout_FormatNbChannels( &p_filter->fmt_in.audio ); /* get no. input channels */
    int i_n_conv = i_input_nb;
    if ( i_physical_channels & AOUT_CHAN_LFE ) /* if LFE is used */
    {   /* decrease number of channels to be convolved: */
        i_n_conv = i_input_nb - 1;
    }

    /* set speaker positions according to input channel configuration: */
    switch ( i_physical_channels )
    {
    case AOUT_CHAN_CENTER:  pf_pos_temp = (float[1]){ 0 };
                            break;
    case AOUT_CHANS_STEREO:
    case AOUT_CHANS_2_1:    pf_pos_temp = (float[2]){ 30 , 330 };
                            break;
    case AOUT_CHANS_3_0:
    case AOUT_CHANS_3_1:    pf_pos_temp = (float[3]){ 30 , 330 , 0 };
                            break;
    case AOUT_CHANS_4_0:
    case AOUT_CHANS_4_1:    pf_pos_temp = (float[4]){ 30 , 330 , 120 , 240 };
                            break;
    case AOUT_CHANS_5_0:
    case AOUT_CHANS_5_1:
    case ( AOUT_CHANS_5_0_MIDDLE | AOUT_CHAN_LFE ):
    case AOUT_CHANS_5_0_MIDDLE: pf_pos_temp = (float[5]){ 30 , 330 , 120 , 240 , 0 };
                                break;
    case AOUT_CHANS_6_0:    pf_pos_temp = (float[6]){ 30 , 330 , 90 , 270 , 150 , 210 };
                            break;
    case AOUT_CHANS_7_0:
    case AOUT_CHANS_7_1:    pf_pos_temp = (float[7]){ 30 , 330 , 90 , 270 , 150 , 210 , 0 };
                            break;
    case AOUT_CHANS_8_1:    pf_pos_temp = (float[8]){ 30 , 330 , 90 , 270 , 150 , 210 , 180 , 0 };
                            break;
    default: return VLC_EGENERIC;
             break;
    }
    memcpy( pf_speaker_pos , pf_pos_temp , i_n_conv * sizeof( float ) );
    return VLC_SUCCESS;

}

static int MaxDelay ( struct nc_sofa_s *sofa )
{
    int i_max = 0;
    for ( int  i = 0; i < ( sofa->i_m_dim * 2 ) ; i++ )
    {   /* search maximum delay in given SOFA file */
        if ( *( sofa->pi_data_delay + i ) > i_max )
            i_max = *( sofa->pi_data_delay + i) ;
    }
    return i_max;
}

static int CompensateVolume( filter_t *p_filter)
{
    struct filter_sys_t *p_sys = p_filter->p_sys;
    float f_energy = 0;
    int i_m;
    int i_sofa_id_backup = p_sys->i_sofa_id;
    float *pf_ir;
    float f_compensate;
    /* compensate volume for each SOFA file */
    for ( int i = 0 ; i < N_SOFA ; i++ ) /* go through all SOFA files */
    {
        if( p_sys->sofa[i].i_ncid )
        {
            /* find IR at front center position in i-th SOFA file (IR closest to 0°,0°,1m) */
            struct nc_sofa_s *p_sofa = &p_sys->sofa[i];
            p_sys->i_sofa_id = i;
            i_m = FindM( p_sys, 0, 0, 1 );
            /* get energy of that IR and compensate volume */
            pf_ir = p_sofa->pf_data_ir + 2 * i_m * p_sofa->i_n_samples;
            for ( int j = 0 ; j < p_sofa->i_n_samples ; j ++ )
            {
                f_energy += *( pf_ir + j ) * *(pf_ir + j );
            }
            f_compensate = 256 / ( p_sofa->i_n_samples * sqrt( f_energy ) );
            msg_Dbg( p_filter, "Compensate-factor: %f", f_compensate );
            pf_ir = p_sofa->pf_data_ir;
            for ( int j = 0 ; j < ( p_sofa->i_n_samples * p_sofa->i_m_dim * 2 ) ; j++ )
            {
                *( pf_ir + j ) *= f_compensate; /* apply volume compensation to IRs */
            }
        }
    }
    p_sys->i_sofa_id = i_sofa_id_backup;
    return VLC_SUCCESS;
}

static void FreeAllSofa ( filter_t *p_filter )
{
    filter_sys_t *p_sys = p_filter->p_sys;
    /* go through all SOFA files and free associated memory: */
    for ( int i = 0 ; i < N_SOFA ; i++)
    {
        if ( p_sys->sofa[i].i_ncid )
        {
            free( p_sys->sofa[i].pf_sp_a );
            free( p_sys->sofa[i].pf_sp_e );
            free( p_sys->sofa[i].pf_sp_r );
            free( p_sys->sofa[i].pi_data_delay );
            free( p_sys->sofa[i].pf_data_ir );
        }
    }
}

static void FreeFilter( filter_t *p_filter )
{
    filter_sys_t *p_sys = p_filter->p_sys;
    free( p_sys->pi_delay_l );
    free( p_sys->pi_delay_r );
    free( p_sys->pf_data_ir_l );
    free( p_sys->pf_data_ir_r );
    free( p_sys->p_data_hrtf_l );
    free( p_sys->p_data_hrtf_r );
    free( p_sys->pf_ringbuffer_l );
    free( p_sys->pf_ringbuffer_r );
    free( p_sys->pf_speaker_pos );
    free( p_sys->p_fft_cfg );
    free( p_sys->p_ifft_cfg );
    free( p_sys );
}

/*****************************************************************************
* Callbacks
******************************************************************************/

static int GainCallback( vlc_object_t *p_this, char const *psz_var,
                          vlc_value_t oldval, vlc_value_t newval, void *pf_data )
{
    VLC_UNUSED(psz_var); VLC_UNUSED(oldval);
    filter_t *p_filter = (filter_t *)pf_data;
    filter_sys_t *p_sys = p_filter->p_sys;
    vlc_mutex_lock( &p_sys->lock );
    p_sys->f_gain = newval.f_float;
    vlc_mutex_unlock( &p_sys->lock );
    /* re-load IRs based on new GUI settings: */
    LoadData( p_filter, p_sys->f_rotation, p_sys->f_elevation, p_sys->f_radius );
    msg_Dbg( p_this , "New Gain-value: %f", newval.f_float );
    return VLC_SUCCESS;
}

static int RotationCallback(vlc_object_t* UNUSED(p_this), char const *psz_var,
                          vlc_value_t oldval, vlc_value_t newval, void *pf_data)
{
    VLC_UNUSED(psz_var); VLC_UNUSED(oldval);
    filter_t *p_filter = (filter_t *)pf_data;
    filter_sys_t *p_sys = p_filter->p_sys;
    float f_temp= (int) (- newval.f_float + 720 ) % 360  ;
    vlc_mutex_lock( &p_sys->lock );
    p_sys->f_rotation = f_temp ;
    vlc_mutex_unlock( &p_sys->lock );
    /* re-load IRs based on new GUI settings: */
    LoadData( p_filter, f_temp, p_sys->f_elevation, p_sys->f_radius );
    msg_Dbg( p_filter, "New azimuth-value: %f", f_temp  );
    return VLC_SUCCESS;
}

static int ElevationCallback( vlc_object_t *UNUSED(p_this), char const *psz_var,
                          vlc_value_t oldval, vlc_value_t newval, void *pf_data )
{
    VLC_UNUSED(psz_var); VLC_UNUSED(oldval);
    filter_t *p_filter = (filter_t *)pf_data;
    filter_sys_t *p_sys = p_filter->p_sys;
    vlc_mutex_lock( &p_sys->lock );
    p_sys->f_elevation = newval.f_float ;
    vlc_mutex_unlock( &p_sys->lock );
    /* re-load IRs based on new GUI settings: */
    LoadData( p_filter, p_sys->f_rotation, newval.f_float, p_sys->f_radius );
    msg_Dbg( p_filter, "New elevation-value: %f", newval.f_float );
    return VLC_SUCCESS;
}

static int HeadRotationCallback(vlc_object_t* UNUSED(p_this), char const *psz_var,
                          vlc_value_t oldval, vlc_value_t newval, void *pf_data)
{
    VLC_UNUSED(psz_var); VLC_UNUSED(oldval);
    filter_t *p_filter = (filter_t *)pf_data;
#if 0
    filter_sys_t *p_sys = p_filter->p_sys;
    float f_temp= (int) (- newval.f_float + 720 ) % 360  ;
    vlc_mutex_lock( &p_sys->lock );
    p_sys->f_rotation = f_temp ;
    vlc_mutex_unlock( &p_sys->lock );
    /* re-load IRs based on new GUI settings: */
    LoadData( p_filter, f_temp, p_sys->f_elevation, p_sys->f_radius );
#endif
    msg_Info( p_filter, "New head_rotation"  );
    return VLC_SUCCESS;
}

static int RadiusCallback( vlc_object_t *UNUSED(p_this), char const *psz_var,
                          vlc_value_t oldval, vlc_value_t newval, void *pf_data )
{
    VLC_UNUSED(psz_var); VLC_UNUSED(oldval);
    filter_t *p_filter = (filter_t *)pf_data;
    filter_sys_t *p_sys = p_filter->p_sys;
    vlc_mutex_lock( &p_sys->lock );
    p_sys->f_radius = newval.f_float ;
    vlc_mutex_unlock( &p_sys->lock );
    /* re-load IRs based on new GUI settings: */
    LoadData( p_filter, p_sys->f_rotation, p_sys->f_elevation,  newval.f_float );
    msg_Dbg( p_filter, "New radius-value: %f", newval.f_float );
    return VLC_SUCCESS;
}

/* new virtual source position selected */
static int SwitchCallback( vlc_object_t *UNUSED(p_this), char const *psz_var,
                           vlc_value_t oldval, vlc_value_t newval, void *pf_data )
{
    VLC_UNUSED(psz_var); VLC_UNUSED(oldval);
    filter_t *p_filter = (filter_t *)pf_data;
    filter_sys_t *p_sys = p_filter->p_sys;
    vlc_mutex_lock( &p_sys->lock );
    p_sys->i_switch = (int) newval.f_float ;
    if ( p_sys->i_switch )
    {   /* if switch not equal 0, pre-defined virtual source positions are used: */
        for ( int i = 0 ; i < p_sys->i_n_conv ; i++ ) *(p_sys->pf_speaker_pos + i ) = 0;
    }
    else /* if switch is zero */
    {   /* get speaker positions depending on current input format */
        GetSpeakerPos ( p_filter, p_sys->pf_speaker_pos );
    }
    vlc_mutex_unlock( &p_sys->lock );
    /* re-load IRs based on new GUI settings: */
    LoadData ( p_filter, p_sys->f_rotation, p_sys->f_elevation, p_sys->f_radius );
    msg_Dbg( p_filter, "New Switch-Position: %d", (int) newval.f_float );
    return VLC_SUCCESS;
}

/* new SOFA file selected */
static int SelectCallback( vlc_object_t *UNUSED(p_this), char const *psz_var,
                           vlc_value_t oldval, vlc_value_t newval, void *pf_data )
{
    VLC_UNUSED(psz_var); VLC_UNUSED(oldval);
    filter_t *p_filter = (filter_t *)pf_data;
    filter_sys_t *p_sys = p_filter->p_sys;
    vlc_mutex_lock( &p_sys->lock );
    if ( p_sys->sofa[((int)newval.f_float + 5 - 1 ) % 5].i_ncid )
    {
        p_sys->i_sofa_id = ( (int) newval.f_float + 5 - 1) % 5 ;
        p_sys->b_mute = false;
        vlc_mutex_unlock( &p_sys->lock );
        /* re-load IRs based on new GUI settings: */
        LoadData ( p_filter, p_sys->f_rotation, p_sys->f_elevation , p_sys->f_radius );
        msg_Dbg( p_filter, "New Sofa-Select: %f", newval.f_float );
    }
    else
    {
        msg_Err( p_filter, "Selected SOFA file is invalid. Please select valid SOFA file." );
        p_sys->b_mute = true;
        vlc_mutex_unlock( &p_sys->lock );
    }
    return VLC_SUCCESS;
}

/*****************************************************************************
* Open:
******************************************************************************/

static int Open( vlc_object_t *p_this )
{
    filter_t *p_filter = (filter_t *)p_this;
    filter_sys_t *p_sys = p_filter->p_sys = malloc( sizeof( *p_sys ) );
    if( unlikely( p_sys == NULL ) )
        return VLC_ENOMEM;

    vlc_object_t *p_out = p_filter->obj.parent; /* assign filter output */

    char *psz_filename[N_SOFA];
    const char *psz_var_names_filename[N_SOFA] =
        { "sofalizer-filename1", "sofalizer-filename2", "sofalizer-filename3" };
    for ( int i = 0 ; i < N_SOFA ; i++ )
    {   /* get SOFA file names from advanced settings */
        psz_filename[i] = var_CreateGetStringCommand( p_filter, psz_var_names_filename[i] );
    }
    /* get user settings */
    p_sys->f_rotation   = fabs ( fmod( -round(var_CreateGetFloat ( p_out, "sofalizer-rotation" ) )+ 720,360) );
    p_sys->i_sofa_id     = round( var_CreateGetFloat ( p_out, "sofalizer-select" ) ) - 1;
    p_sys->i_switch     = round( var_CreateGetFloat ( p_out, "sofalizer-switch" ) );
    p_sys->f_gain       = var_CreateGetFloat( p_out, "sofalizer-gain" );
    p_sys->f_elevation  = var_CreateGetFloat( p_out, "sofalizer-elevation" );
    p_sys->f_radius     = var_CreateGetFloat( p_out, "sofalizer-radius");

    const char *psz_var_names_azimuth_array[N_POSITIONS] =
        { "sofalizer-pos1-azi", "sofalizer-pos2-azi", "sofalizer-pos3-azi", "sofalizer-pos4-azi" };
    for ( int i = 0 ; i < N_POSITIONS ; i++ )
    {   /* get azimuth angles of virtual source positions from advanced settings */
        p_sys->pi_azimuth_array[i] = ( var_InheritInteger ( p_out, psz_var_names_azimuth_array[i] ) + 720 ) % 360 ;
    }

    const char *psz_var_names_elevation_array[N_POSITIONS] =
        { "sofalizer-pos1-ele", "sofalizer-pos2-ele", "sofalizer-pos3-ele", "sofalizer-pos4-ele" };
    for ( int i = 0 ; i < N_POSITIONS ; i++ )
    {   /* get elevation angles of virtual source positions from advanced settings */
        p_sys->pi_elevation_array[i] = var_InheritInteger( p_out, psz_var_names_elevation_array[i] ) ;
    }

    p_sys->i_processing_type = var_InheritInteger( p_out,
        "sofalizer-processing-type" ); /* get audio processing type */
    p_sys->i_resampling_type = var_InheritInteger( p_out,
        "sofalizer-resampling-type" ); /* get resampling quality */

    p_sys->b_mute = false;
    p_sys->i_write = 0;

    /* sampling rate and SRC resampling variables */
    int i_samplingrate = 0;
    /* get sampling rate of audio file/stream: */
    int i_samplingrate_stream = p_filter->fmt_in.audio.i_rate;
    int i_n_samples_new; /* length of one resampled IR */
    int i_n_samples_out; /* number of output samples*/

    int i_resampling_type = p_sys->i_resampling_type; /* resampling quality */
    int i_channels = 0; /* no. channels to resample  (here, 1 channel is 1 IR) */
    double d_ratio = 0; /* resampling conversion ratio */
    SRC_DATA src; /* contains settings for sample rate conversion using SRC */

    int i_err = -1; /* error code of SRC function */

    float *pf_ir_temp; /* temporary variable for resampled IRs */
    int i_status = 0; /* zero, if no file could be loaded */

    /* load SOFA files, resample if sampling rate different from audio file */
    for( int i = 0; i < N_SOFA; i++)
    {   /* initialize file IDs to 0 before attempting to load SOFA files,
         * this assures that in case of error, only the memory of already
         * loaded files is free'd ( e.g. in FreeAllSofa() ) */
        p_sys->sofa[i].i_ncid = 0;
    }
    for( int i = 0;  i < N_SOFA; i++ )
    {
        if ( LoadSofa( p_filter, psz_filename[i], i , &i_samplingrate) != VLC_SUCCESS )
        {   /* file loading error */
            msg_Err(p_filter, "Error while loading SOFA file %d: '%s'", i + 1, psz_filename[i] );
        }
        else if (i_samplingrate != i_samplingrate_stream)
        {   /* no file loading error, resampling required */
            msg_Dbg( p_filter, "File %d: '%s' loaded. Resampling impulse responses to %d Hz.",
                i + 1 , psz_filename[i], i_samplingrate_stream );

            /* -- sampling rate conversion using SRC -- */

            /* get conversion ratio, no. channels, no. output samples */
            d_ratio = ( double ) i_samplingrate_stream / i_samplingrate;
            i_channels = 2 * p_sys->sofa[i].i_m_dim; /* no. channels to resample */
            i_n_samples_new = /* length of one resampled IR: */
                ceil( ( double ) p_sys->sofa[i].i_n_samples * d_ratio );
            i_n_samples_out = /* total no. output samples */
                i_channels * i_n_samples_new;

            /* get temporary memory for resampled IRs */
            pf_ir_temp = malloc( sizeof(float) * i_n_samples_out );
            if( !pf_ir_temp ) /* memory allocation failed */
            {
                FreeAllSofa( p_filter );
                free( p_sys );
                return VLC_ENOMEM;
            }

            /* set src data */
            src.input_frames = p_sys->sofa[i].i_n_samples; /* length of 1 IR */
            src.output_frames = i_n_samples_new; /* new number of samples */
            src.src_ratio = d_ratio; /* sampling rate conversion ratio */

            /* call SRC resampling function and check for errors */
            //i_err = src_simple( &src, i_resampling_type, i_channels );
            for( int j = 0; j < i_channels; j++ )
            {
                src.data_in = p_sys->sofa[i].pf_data_ir + j * p_sys->sofa[i].i_n_samples; /* IRs of current file */
                src.data_out = pf_ir_temp + j * i_n_samples_new; /* write to temporary IR memory */
                i_err = src_simple( &src, i_resampling_type, 1 );
                if( unlikely( i_err ) ) /* if error occured during resampling */
                {
                    msg_Err( p_filter, "cannot resample: %s", src_strerror (i_err) );
                    FreeAllSofa( p_filter );
                    free( p_sys );
                    free( pf_ir_temp );
                    return VLC_EGENERIC;
                }
            }

            /* reallocate memory for resampled IRs */
            p_sys->sofa[i].pf_data_ir = realloc( p_sys->sofa[i].pf_data_ir,
                                       sizeof( float ) * i_n_samples_out );
            if( !p_sys->sofa[i].pf_data_ir ) /* memory allocation failed */
            {
                FreeAllSofa( p_filter );
                free( p_sys );
                free( pf_ir_temp );
                return VLC_ENOMEM;
            }

            /* copy resampled IRs from temporary to SOFA struct variables */
            memcpy( p_sys->sofa[i].pf_data_ir, pf_ir_temp,
                    sizeof( float ) * i_n_samples_out );

            /* update SOFA struct */
            p_sys->sofa[i].i_n_samples = i_n_samples_new;

            /* free temporary IR memory */
            free( pf_ir_temp );
            pf_ir_temp = NULL;

            msg_Dbg( p_filter,
                "Resampled %d impulses responses (each IR: %ld samples at %d Hz to %ld samples at %d Hz sampling rate, SOFA file %d).",
                i_channels, src.input_frames_used, i_samplingrate,
                src.output_frames_gen, i_samplingrate_stream, i + 1 );

            /* resample broadband delays by multiplication with the ratio to
             * maintain same delay time in sec with the new sampling rate */
            for( int j = 0; j < 2 * p_sys->sofa[i].i_m_dim; j++ )
            {
                *( p_sys->sofa[i].pi_data_delay + j ) *= round( d_ratio );
            }
            i_status++; /* increase status after successful file loading */
        }
        else /* no file loading error, resampling not required */
        {
            msg_Dbg( p_filter, "File %d: '%s' loaded", i + 1 , psz_filename[i] );
            i_status++; /* increase status after successful file loading */
        }
    }
    if( !i_status )
    {
        msg_Err( p_filter, "No valid SOFA file could be loaded. Please specify at least one valid SOFA file." );
        FreeAllSofa( p_filter );
        free( p_sys );
        return VLC_EGENERIC;
    }

    /* set filter settings and calculate speaker positions */
    p_filter->fmt_in.audio.i_format = VLC_CODEC_FL32;
    p_filter->fmt_out.audio = p_filter->fmt_in.audio;

    /* set physical channels to stereo, required for filter output set to stereo */
    p_filter->fmt_out.audio.i_physical_channels = AOUT_CHANS_STEREO;
    p_filter->fmt_out.audio.i_original_channels = AOUT_CHANS_STEREO;

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

    /* get size of ringbuffer (longest IR plus max. delay) */
    /* then choose next power of 2 for performance optimization */
    int i_n_max = 0;
    int i_n_current;
    int i_n_max_ir = 0;
    for ( int i = 0 ; i < N_SOFA ; i++ )
    {   /* go through all SOFA files and determine the longest IR */
        if ( p_sys->sofa[i].i_ncid != 0 )
        {
            i_n_current = p_sys->sofa[i].i_n_samples + MaxDelay ( &p_sys->sofa[i] );
            if ( i_n_current > i_n_max )
            {
                /* length of longest IR plus max. delay (in all SOFA files) */
                i_n_max = i_n_current;
                /* length of longest IR (without delay, in all SOFA files) */
                i_n_max_ir = p_sys->sofa[i].i_n_samples;
            }
        }
    }
    p_sys->i_n_longest_filter = i_n_max; /* longest IR plus max. delay */
    /* buffer length is longest IR plus max. delay -> next power of 2
       (32 - count leading zeros gives required exponent)  */
    p_sys->i_buffer_length = pow(2, 32 - clz( (uint32_t) i_n_max ) );

    p_sys->i_output_buffer_length = 0; /* initialization */
    p_sys->i_n_fft = 0; /* 0 until output buffer length was retrieved */
    p_sys->p_fft_cfg = NULL; /* don't leave FFT config uninitialized */
    p_sys->p_ifft_cfg = NULL; /* do this before FreeFilter might be called */

    p_sys->p_data_hrtf_l = NULL; /* initialization */
    p_sys->p_data_hrtf_r = NULL;

    /* Allocate memory for the impulse responses, delays and the ringbuffers */
    /* size: (longest IR) * (number of channels to convolute), without LFE */
    p_sys->pf_data_ir_l = malloc( sizeof(float) * i_n_max_ir * p_sys->i_n_conv );
    p_sys->pf_data_ir_r = malloc( sizeof(float) * i_n_max_ir * p_sys->i_n_conv );
    /* length:  number of channels to convolute */
    p_sys->pi_delay_l = malloc ( sizeof( int ) * p_sys->i_n_conv );
    p_sys->pi_delay_r = malloc ( sizeof( int ) * p_sys->i_n_conv );
    /* length: (buffer length) * (number of input channels),
     * OR: buffer length (if frequency domain processing)
     * calloc zero-initializes the buffer */
    if( p_sys->i_processing_type == PROC_TIME_DOM )
    {
        p_sys->pf_ringbuffer_l = calloc( p_sys->i_buffer_length * i_input_nb,
                                        sizeof( float ) );
        p_sys->pf_ringbuffer_r = calloc( p_sys->i_buffer_length * i_input_nb,
                                        sizeof( float ) );
    }
    else if( p_sys->i_processing_type == PROC_FREQ_DOM )
    {
        p_sys->pf_ringbuffer_l = calloc( p_sys->i_buffer_length,
                                        sizeof( float ) );
        p_sys->pf_ringbuffer_r = calloc( p_sys->i_buffer_length,
                                        sizeof( float ) );
    }
    /* length: number of channels to convolute */
    p_sys->pf_speaker_pos = malloc( sizeof( float) * p_sys->i_n_conv );

    /* memory allocation failed: */
    if ( !p_sys->pf_data_ir_l || !p_sys->pf_data_ir_r || !p_sys->pi_delay_l ||
         !p_sys->pi_delay_r || !p_sys->pf_ringbuffer_l || !p_sys->pf_ringbuffer_r
         || !p_sys->pf_speaker_pos )
    {
        FreeAllSofa( p_filter );
        FreeFilter( p_filter );
        return VLC_ENOMEM;
    }

    CompensateVolume ( p_filter );

    /* get speaker positions */
    if ( GetSpeakerPos ( p_filter, p_sys->pf_speaker_pos ) != VLC_SUCCESS )
    {
        msg_Err (p_filter, "Couldn't get speaker positions. Input channel configuration not supported. ");
        FreeAllSofa( p_filter );
        FreeFilter( p_filter );
        return VLC_EGENERIC;
    }
    vlc_mutex_init( &p_sys->lock );
    /* load IRs to pf_data_ir_l and pf_data_ir_r for required directions */
    if( p_sys->i_processing_type == PROC_TIME_DOM )
    {   /* only load IRs if time-domain convolution is used,
         * otherwise, data is loaded on FFT size change */
        if ( LoadData ( p_filter, p_sys->f_rotation, p_sys->f_elevation,
                        p_sys->f_radius ) != VLC_SUCCESS )
        {
            FreeAllSofa( p_filter );
            FreeFilter( p_filter );
            return VLC_ENOMEM;
        }
    }

    msg_Dbg( p_filter, "Samplerate: %d\n Channels to convolute: %d, Length of ringbuffer: %d x %d",
    p_filter->fmt_in.audio.i_rate, p_sys->i_n_conv, i_input_nb, (int )p_sys->i_buffer_length );

    p_filter->pf_audio_filter = DoWork; /* DoWork does the audio processing */

    /* Callbacks can call function LoadData */
    var_AddCallback( p_out, "sofalizer-gain", GainCallback, p_filter );
    var_AddCallback( p_out, "sofalizer-rotation", RotationCallback, p_filter );
    var_AddCallback( p_out, "sofalizer-elevation", ElevationCallback, p_filter );
    var_AddCallback( p_out, "sofalizer-switch", SwitchCallback, p_filter );
    var_AddCallback( p_out, "sofalizer-select", SelectCallback, p_filter );
    var_AddCallback( p_out, "sofalizer-radius", RadiusCallback, p_filter );

    var_AddCallback( p_filter->obj.libvlc, "head-rotation", HeadRotationCallback, p_filter );

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
    int i_n_clippings_l = 0; /* count output samples equal to or greather than 1 */
    int i_n_clippings_r = 0; /* (i.e. clipping occurs) */

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
        block_Release( p_in_buf );
        goto out;
    }
    /* set output buffer parameters */
    p_out_buf->i_nb_samples = p_in_buf->i_nb_samples;
    p_out_buf->i_dts        = p_in_buf->i_dts;
    p_out_buf->i_pts        = p_in_buf->i_pts;
    p_out_buf->i_length     = p_in_buf->i_length;

    /* -- if using fast convolution and output buffer length has changed:
     *    transform IRs to frequency domain (FFT of new size) -- */
    if( ( p_sys->i_processing_type == PROC_FREQ_DOM ) &&
        unlikely ( (size_t)p_out_buf->i_nb_samples != (size_t)p_sys->i_output_buffer_length ) )
    {
        /* update FFT size: longest filter plus length of output buffer,
         *                  next power of 2 */
        const int i_nb_samples_convolution =
            p_sys->i_n_longest_filter + p_out_buf->i_nb_samples;
        const int i_n_fft = p_sys->i_n_fft = /* next power of 2 */
            pow(2, 32 - clz( (uint32_t) i_nb_samples_convolution ) );

        /* update FFT configuration */
        free( p_sys->p_fft_cfg ); /* free previous FFT config */
        free( p_sys->p_ifft_cfg );
        p_sys->p_fft_cfg = kiss_fft_alloc( i_n_fft, 0, NULL, NULL );
        if( !p_sys->p_fft_cfg )
            goto out;
        p_sys->p_ifft_cfg = kiss_fft_alloc( i_n_fft, 1, NULL, NULL );
        if( !p_sys->p_ifft_cfg )
        {
            free( p_sys->p_fft_cfg );
            goto out;
        }

        /* get currently needed HRTFs (based on GUI settings) */
        if( LoadData ( p_filter, p_sys->f_rotation, p_sys->f_elevation,
                        p_sys->f_radius ) )
        {
            goto out;
        }

        /* save current output buffer length:
         * (only if updating to new length was successful) */
        p_sys->i_output_buffer_length = p_out_buf->i_nb_samples;

        msg_Dbg( p_filter, "New FFT size: %d samples.", p_sys->i_n_fft );
    }

    /* threads for simultaneous computation of left and right channel */
    vlc_thread_t left_thread, right_thread;
    struct thread_data_s t_data_l, t_data_r;

    /* GUI gain -3 dB per channel, -6 dB to get LFE on a similar level */
    float f_gain_lfe = expf( (p_sys->f_gain - 3 * i_input_nb - 6) / 20 * logf(10));

    /* prepare thread_data_s structs for L and R channel, respectively */
    t_data_l.p_sys = t_data_r.p_sys = p_sys;
    t_data_l.p_in_buf = t_data_r.p_in_buf = p_in_buf;
    t_data_l.pi_input_nb = t_data_r.pi_input_nb = &i_input_nb;
    t_data_l.f_gain_lfe = t_data_r.f_gain_lfe = f_gain_lfe;
    t_data_l.i_write = t_data_r.i_write = p_sys->i_write;
    t_data_l.pf_ringbuffer = p_sys->pf_ringbuffer_l;
    t_data_r.pf_ringbuffer = p_sys->pf_ringbuffer_r;
    t_data_l.pf_data = p_sys->pf_data_ir_l;
    t_data_r.pf_data = p_sys->pf_data_ir_r;
    t_data_l.pi_n_clippings = &i_n_clippings_l;
    t_data_r.pi_n_clippings = &i_n_clippings_r;
    t_data_l.pf_dest = (float *)p_out_buf->p_buffer;
    t_data_r.pf_dest = (float *)p_out_buf->p_buffer + 1;
    t_data_l.pi_delay = p_sys->pi_delay_l;
    t_data_r.pi_delay = p_sys->pi_delay_r;

    if ( unlikely( p_sys->b_mute ) ) /* mutes output (e.g. invalid SOFA file selected) */
    {
        memset( p_out_buf->p_buffer, 0 , sizeof( float ) * p_in_buf->i_nb_samples * 2 );
    }
    else if( p_sys->i_processing_type == PROC_TIME_DOM )
    {   /* compute convolution for left and right channel (time-domain) */
        if( vlc_clone( &left_thread, (void *)&sofalizer_Convolute,
        (void *)&t_data_l, VLC_THREAD_PRIORITY_AUDIO ) ) goto out;
        if( vlc_clone( &right_thread, (void *)&sofalizer_Convolute,
        (void *)&t_data_r, VLC_THREAD_PRIORITY_AUDIO ) )
        {
            vlc_join ( left_thread, NULL );
            goto out;
        }
        vlc_join ( left_thread, NULL );
        vlc_join ( right_thread, NULL );
        p_sys->i_write = t_data_l.i_write;
    }
    else if( p_sys->i_processing_type == PROC_FREQ_DOM )
    {
        if( sofalizer_FastConvolution( (void *)&t_data_l, (void *)&t_data_r ) )
            goto out;
        p_sys->i_write = t_data_l.i_write;
    }

    /* display error message if clipping occured */
    if ( ( i_n_clippings_l + i_n_clippings_r ) > 0 )
    {
        msg_Err(p_filter, "%d of %d Samples in the Outputbuffer clipped. Please reduce gain.",
                          i_n_clippings_l + i_n_clippings_r, p_out_buf->i_nb_samples * 2 );
    }

out: block_Release( p_in_buf );
    return p_out_buf; /* DoWork returns the modified output buffer */
}

void sofalizer_Convolute ( void *p_ptr )
{
    struct thread_data_s *t_data;
    t_data = (struct thread_data_s *)p_ptr;
    struct filter_sys_t *p_sys = t_data->p_sys;
    int i_n_samples = p_sys->sofa[p_sys->i_sofa_id].i_n_samples; /* length of one IR */
    float *pf_src = (float *)t_data->p_in_buf->p_buffer; /* get pointer to audio input buffer */
    float *pf_temp_ir;
    float *pf_dest = t_data->pf_dest; /* get pointer to audio output buffer */
    int i_read;
    int *pi_delay = t_data->pi_delay; /* broadband delay for each IR to be convolved */
    int i_input_nb = *t_data->pi_input_nb; /* number of input channels */
    /* ring buffer length is: longest IR plus max. delay -> next power of 2 */
    int i_buffer_length = p_sys->i_buffer_length;
    /* -1 for AND instead of MODULO (applied to powers of 2): */
    uint32_t i_modulo = (uint32_t) i_buffer_length - 1;
    float *pf_ringbuffer[i_input_nb]; /* holds ringbuffer for each input channel */
    for ( int l = 0 ; l < i_input_nb ; l++ )
    {   /* get starting address of ringbuffer for each input channel */
        pf_ringbuffer[l] = t_data->pf_ringbuffer + l * i_buffer_length ;
    }
    int i_write = t_data->i_write;
    float *pf_ir = t_data->pf_data;
    /* outer loop: go through all samples of current input buffer: */
    for ( int i = t_data->p_in_buf->i_nb_samples ; i-- ; )
    {   /* i is not used as an index in the loop! */
        *( pf_dest ) = 0;
        for ( int l = 0 ; l < i_input_nb ; l++ )
        {   /* write current input sample to ringbuffer (for each channel) */
            *( pf_ringbuffer[l] + i_write ) = *( pf_src++ );
        }
        pf_temp_ir = pf_ir; /* using same set of IRs for each sample */
        /* loop goes through all channels to be convolved (excl. LFE): */
        for ( int l = 0 ; l < p_sys->i_n_conv ; l++ )
        {
            /* current read position in ringbuffer: input sample write position
             * - delay for l-th ch. + diff. betw. IR length and buffer length
             * (mod buffer length) */
            i_read = ( i_write - *( pi_delay + l ) -
                       ( i_n_samples - 1 ) + i_buffer_length ) & i_modulo;

            for ( int j = i_n_samples ; j-- ; ) /* go through samples of IR */
            {
                /* multiply signal and IR, and add up the results */
                *( pf_dest ) += *( pf_ringbuffer[l] + ( ( i_read++ ) & i_modulo ) ) *
                               *( pf_temp_ir++ );
            }
        }
        if ( p_sys->b_lfe ) /* LFE */
        {
            /* apply gain to LFE signal and add to output buffer */
            *( pf_dest ) += *( pf_ringbuffer[p_sys->i_n_conv] + i_write ) * t_data->f_gain_lfe;
        }
        /* clippings counter */
        if ( fabs( *( pf_dest ) ) >= 1 ) /* if current output sample  >= 1 */
        {
            *t_data->pi_n_clippings = *t_data->pi_n_clippings + 1;
        }
        /* move output buffer pointer by +2 to get to next sample of processed channel: */
        pf_dest   += 2;
        i_write  = ( i_write + 1 ) & i_modulo; /* update ringbuffer write position */
    }
    t_data->i_write = i_write; /* remember write position in ringbuffer for next call */
    return;
}

int sofalizer_FastConvolution( void *p_ptr_l, void *p_ptr_r )
{
    struct thread_data_s *t_data_l, *t_data_r;
    t_data_l = (struct thread_data_s *)p_ptr_l;
    t_data_r = (struct thread_data_s *)p_ptr_r;
    struct filter_sys_t *p_sys = t_data_l->p_sys;

    /* get length of one IR */
    int i_n_samples = p_sys->sofa[p_sys->i_sofa_id].i_n_samples;
    /* no. input/output samples */
    int i_output_buffer_length = t_data_l->p_in_buf->i_nb_samples;
    int i_n_fft = p_sys->i_n_fft; /* length of one FFT */
    int i_n_conv = p_sys->i_n_conv; /* no. channels to convolve */

    /* get pointer to audio input buffer */
    float *pf_src = (float *)t_data_l->p_in_buf->p_buffer;
    float *pf_dest = t_data_l->pf_dest; /* get pointer to audio output buffer */
    int i_offset; /* helper variable for efficient loops */
    int i_input_nb = *t_data_l->pi_input_nb; /* number of input channels */
    /* ring buffer length is: longest IR plus max. delay -> next power of 2 */
    int i_buffer_length = p_sys->i_buffer_length;
    /* -1 for AND instead of MODULO (applied to powers of 2): */
    uint32_t i_modulo = (uint32_t) i_buffer_length - 1;
    int i_write = t_data_l->i_write; /* get saved read/write position from last call */

    /* get starting address of ringbuffer for usage as overflow buffer, *
     * no initialization necessary because calloc was used */
    float *pf_ringbuffer_l = t_data_l->pf_ringbuffer;
    float *pf_ringbuffer_r = t_data_r->pf_ringbuffer;

    /* get pointers to current HRTF data */
    kiss_fft_cpx *p_hrtf_l = p_sys->p_data_hrtf_l;
    kiss_fft_cpx *p_hrtf_r = p_sys->p_data_hrtf_r;

    /* temporary arrays for FFT input/output data */
    kiss_fft_cpx *p_fft_in = calloc( i_n_fft, sizeof( kiss_fft_cpx ) );
    kiss_fft_cpx *p_fft_out = calloc( i_n_fft, sizeof( kiss_fft_cpx ) );
    kiss_fft_cpx *p_fft_in_l = calloc( i_n_fft, sizeof( kiss_fft_cpx ) );
    kiss_fft_cpx *p_fft_in_r = calloc( i_n_fft, sizeof( kiss_fft_cpx ) );
    kiss_fft_cpx *p_fft_out_l = calloc( i_n_fft, sizeof( kiss_fft_cpx ) );
    kiss_fft_cpx *p_fft_out_r = calloc( i_n_fft, sizeof( kiss_fft_cpx ) );
    if( !p_fft_in || !p_fft_out || !p_fft_in_l || !p_fft_in_r ||
        !p_fft_out_l || !p_fft_out_r )
    {   /* free memory and return error if memory allocation failed */
        free( p_fft_in );
        free( p_fft_out);
        free( p_fft_in_l);
        free( p_fft_in_r);
        free( p_fft_out_l);
        free( p_fft_out_r);
        return VLC_ENOMEM;
    }

    /* find min. between no. samples and output buffer length:
     * (important, if one IR is longer than the output buffer) */
    int i_n_read = ( ( i_n_samples - 1 ) < i_output_buffer_length ) ?
                     ( i_n_samples - 1 ) : i_output_buffer_length;
    for( int j = 0; j < i_n_read; j++ )
    {   /* initialize output buf with saved signal from overflow buf */
        *( pf_dest + 2 * j ) = *( pf_ringbuffer_l + i_write );
        *( pf_dest + 2 * j + 1 ) = *( pf_ringbuffer_r + i_write );
        *( pf_ringbuffer_l + i_write ) = 0.0; /* re-set read samples to zero */
        *( pf_ringbuffer_r + i_write ) = 0.0;
        /* update ringbuffer read/write position */
        i_write  = ( i_write + 1 ) & i_modulo;
    }

    /* initialize rest of output buffer with 0 */
    memset( pf_dest + 2 * i_n_read, 0,
            sizeof( float ) * 2 * ( i_output_buffer_length - i_n_read ) );

    for( int i = 0; i < i_n_conv; i++ )
    {   /* outer loop: go through all input channels to be convolved */
        i_offset = i * i_n_fft; /* no. samples already processed */

        /* fill FFT input with 0 (we want to zero-pad) */
        memset( p_fft_in, 0, sizeof( kiss_fft_cpx ) * i_n_fft );

        for( int j = 0; j < i_output_buffer_length; j++ )
        {   /* pepare input for FFT */
            /* write all samples of current input channel to FFT input array */
            p_fft_in[j].r = *( pf_src + i + j * i_input_nb );
        }

        /* transform input signal of current channel to frequency domain */
        kiss_fft( p_sys->p_fft_cfg, p_fft_in, p_fft_out );
        for( int j = 0; j < i_n_fft; j++ )
        {   /* complex multiplication of input signal and HRTFs */
            p_fft_in_l[j].r = /* left output channel (real): */
                ( p_fft_out[j].r * ( p_hrtf_l + i_offset + j )->r -
                  p_fft_out[j].i * ( p_hrtf_l + i_offset + j )->i );
            p_fft_in_l[j].i = /* left output channel (imag): */
                ( p_fft_out[j].r * ( p_hrtf_l + i_offset + j )->i +
                  p_fft_out[j].i * ( p_hrtf_l + i_offset + j )->r );
            p_fft_in_r[j].r = /* right output channel (real): */
                ( p_fft_out[j].r * ( p_hrtf_r + i_offset + j )->r -
                  p_fft_out[j].i * ( p_hrtf_r + i_offset + j )->i );
            p_fft_in_r[j].i = /* right output channel (imag): */
                ( p_fft_out[j].r * ( p_hrtf_r + i_offset + j )->i +
                  p_fft_out[j].i * ( p_hrtf_r + i_offset + j )->r );
        }
        /* transform output signal of current channel back to time domain */
        kiss_fft( p_sys->p_ifft_cfg, p_fft_in_l, p_fft_out_l );
        kiss_fft( p_sys->p_ifft_cfg, p_fft_in_r, p_fft_out_r );
        for( int j = 0; j < i_output_buffer_length; j++ )
        {   /* write output signal of current channel to output buffer */
            *( pf_dest + 2 * j ) += p_fft_out_l[j].r / (float) i_n_fft;
            *( pf_dest + 2 * j + 1 ) += p_fft_out_r[j].r / (float) i_n_fft;
        }

        int i_write_pos = 0;
        for( int j = 0; j < i_n_samples - 1; j++ ) /* overflow length is IR length - 1 */
        {   /* write the rest of output signal to overflow buffer */
            i_write_pos = ( i_write + j ) & i_modulo;
            *( pf_ringbuffer_l + i_write_pos ) +=
                p_fft_out_l[i_output_buffer_length + j].r / (float) i_n_fft; /* right channel */
            *( pf_ringbuffer_r + i_write_pos ) +=
                p_fft_out_r[i_output_buffer_length + j].r / (float) i_n_fft; /* left channel */
        }
    }

    /* go through all samples of current input buffer: LFE, count clippings */
    for ( int i = 0; i < i_output_buffer_length; i++ )
    {
        if ( p_sys->b_lfe ) /* LFE */
        {
            /* apply gain to LFE signal and add to output buffer */
            *( pf_dest ) +=
                *( pf_src + i_n_conv + i * i_input_nb ) * t_data_l->f_gain_lfe;
            *( pf_dest + 1 ) +=
                *( pf_src + i_n_conv + i * i_input_nb ) * t_data_r->f_gain_lfe;
        }
        /* clippings counter */
        if ( fabs( *( pf_dest ) ) >= 1 ) /* if current output sample  >= 1 */
        {   /* left channel */
            *t_data_l->pi_n_clippings = *t_data_l->pi_n_clippings + 1;
        }
        if ( fabs( *( pf_dest + 1 ) ) >= 1 ) /* if current output sample  >= 1 */
        {   /* right channel */
            *t_data_r->pi_n_clippings = *t_data_r->pi_n_clippings + 1;
        }
        /* move output buffer pointer by +2 to get to next sample of processed channel: */
        pf_dest += 2;
    }

    /* remember read/write position in ringbuffer for next call */
    t_data_l->i_write = t_data_r->i_write = i_write;

    /* free temporary memory */
    free( p_fft_in );
    free( p_fft_out);
    free( p_fft_in_l);
    free( p_fft_in_r);
    free( p_fft_out_l);
    free( p_fft_out_r);

    return VLC_SUCCESS;
}

/*****************************************************************************
* LoadData: Load the impulse responses (reversed) for required source positions
*         to pf_data_l and pf_data_r and applies the gain (GUI) to them.
*
* FindM: Find the impulse response closest to a required source position
******************************************************************************/

static int LoadData ( filter_t *p_filter, int i_azim, int i_elev, float f_radius)
{
    struct filter_sys_t *p_sys = p_filter->p_sys;
    if( unlikely( !p_sys->sofa[p_sys->i_sofa_id].i_ncid ) )
    {   /* if an invalid SOFA file has been selected */
        p_sys->b_mute = true; /* mute output */
        msg_Err( p_filter,
            "Selected SOFA file is invalid. Please select valid SOFA file." );
        return VLC_EGENERIC;
    }
    const int i_n_samples = p_sys->sofa[p_sys->i_sofa_id].i_n_samples;
    const int i_n_fft = p_sys->i_n_fft;
    int i_n_conv = p_sys->i_n_conv; /* no. channels to convolve (excl. LFE) */
    int pi_delay_l[i_n_conv]; /* broadband delay for each IR */
    int pi_delay_r[i_n_conv];
    int i_input_nb = aout_FormatNbChannels( &p_filter->fmt_in.audio ); /* no. input channels */
    float f_gain_lin = expf( (p_sys->f_gain - 3 * i_input_nb) / 20 * logf(10) ); /* GUI gain - 3dB/channel */

    float *pf_data_ir_l = NULL;
    float *pf_data_ir_r = NULL;
    kiss_fft_cpx *p_data_hrtf_l = NULL;
    kiss_fft_cpx *p_data_hrtf_r = NULL;
    kiss_fft_cpx *p_fft_in_l = NULL;
    kiss_fft_cpx *p_fft_in_r = NULL;

    if( p_sys->i_processing_type == PROC_TIME_DOM )
    {
        /* get temporary IR for L and R channel */
        pf_data_ir_l =
            malloc( sizeof( float ) * i_n_conv * i_n_samples );
        if( !pf_data_ir_l )
            return VLC_ENOMEM; /* memory allocation failed */
        pf_data_ir_r =
            malloc( sizeof( float ) * i_n_conv * i_n_samples );
        if( !pf_data_ir_r )
            return VLC_ENOMEM; /* memory allocation failed */
    }
    else if( ( p_sys->i_processing_type == PROC_FREQ_DOM )  &&
             ( p_sys->i_n_fft != 0) )
    {
        /* get temporary HRTF memory for L and R channel */
        p_data_hrtf_l =
            malloc( sizeof( kiss_fft_cpx ) * i_n_conv * i_n_fft );
        if( !p_data_hrtf_l )
            return VLC_ENOMEM; /* memory allocation failed */
        p_data_hrtf_r =
            malloc( sizeof( kiss_fft_cpx ) * i_n_conv * i_n_fft );
        if( !p_data_hrtf_r )
        {
            free( p_data_hrtf_l );
            return VLC_ENOMEM; /* memory allocation failed */
        }
    }

    int i_offset = 0; /* used for faster pointer arithmetics in for-loop */

    int i_m[p_sys->i_n_conv]; /* measurement index m of IR closest to required source positions */
    if ( p_sys->i_switch ) /* if switch on GUI not 0, use pre-defined virtual source positions */
    {
        i_azim = p_sys->pi_azimuth_array[p_sys->i_switch - 1];
        i_elev = p_sys->pi_elevation_array[p_sys->i_switch -1];
    }
    int i_azim_orig = i_azim;

    for ( int i = 0; i < p_sys->i_n_conv; i++ )
    {   /* load and store IRs and corresponding delays */
        i_azim = (int)( p_sys->pf_speaker_pos[i] + i_azim_orig ) % 360;
        /* get id of IR closest to desired position */
        i_m[i] = FindM( p_sys, i_azim, i_elev, f_radius );

        /* load the delays associated with the current IRs */
        pi_delay_l[i] = *( p_sys->sofa[p_sys->i_sofa_id].pi_data_delay + 2 * i_m[i] );
        pi_delay_r[i] = *( p_sys->sofa[p_sys->i_sofa_id].pi_data_delay + 2 * i_m[i] + 1);

        if( p_sys->i_processing_type == PROC_TIME_DOM )
        {
            i_offset = i * i_n_samples; /* no. samples already written */
            for ( int j = 0 ; j < i_n_samples; j++ )
            {
                /* load reversed IRs of the specified source position
                 * sample-by-sample for left and right ear; and apply gain */
                *( pf_data_ir_l + i_offset + j ) = /* left channel */
                *( p_sys->sofa[p_sys->i_sofa_id].pf_data_ir +
                2 * i_m[i] * i_n_samples + i_n_samples - 1 - j ) * f_gain_lin;
                *( pf_data_ir_r + i_offset + j ) = /* right channel */
                *( p_sys->sofa[p_sys->i_sofa_id].pf_data_ir +
                2 * i_m[i] * i_n_samples + i_n_samples - 1 - j  + i_n_samples ) * f_gain_lin;
            }
        }
        else if( ( p_sys->i_processing_type == PROC_FREQ_DOM ) &&
                 ( p_sys->i_n_fft != 0) )
        {          /* only load & transform if FFT length is known */
            /* temporary arrays for FFT input/output data
             * calloc zero-initializes them! */
            //kiss_fft_cpx p_fft_in_l[i_n_fft], p_fft_in_r[i_n_fft];
            p_fft_in_l = calloc( i_n_fft, sizeof( kiss_fft_cpx ) );
            p_fft_in_r = calloc( i_n_fft, sizeof( kiss_fft_cpx ) );
            if( !p_fft_in_l || !p_fft_in_r )
            {   /* free and return error on memory allocation fail */
                free( p_data_hrtf_l );
                free( p_data_hrtf_r );
                free( p_fft_in_l );
                free( p_fft_in_r );
                return VLC_ENOMEM;
            }

            /* initalize FFT input to zero -> TODO could be removed if calloc
             * delay samples and zero-pad samples are already filled with 0! */
            //memset( p_fft_in_l, 0, sizeof( kiss_fft_cpx ) * i_n_fft );
            //memset( p_fft_in_r, 0, sizeof( kiss_fft_cpx ) * i_n_fft );

            i_offset = i * i_n_fft; /* no. samples already written */
            for ( int j = 0 ; j < i_n_samples; j++ )
            {
                /* load non-reversed IRs of the specified source position
                 * sample-by-sample and apply gain,
                 * L channel is loaded to real part, R channel to imag part,
                 * IRs ared shifted by L and R delay */
                p_fft_in_l[ pi_delay_l[i] + j ].r = /* left channel */
                *( p_sys->sofa[p_sys->i_sofa_id].pf_data_ir +
                   2 * i_m[i] * i_n_samples + j ) * f_gain_lin;
                p_fft_in_r[ pi_delay_r[i] + j ].r = /* right channel */
                *( p_sys->sofa[p_sys->i_sofa_id].pf_data_ir +
                   ( 2 * i_m[i] + 1 ) * i_n_samples + j ) * f_gain_lin;
            }

            /* actually transform to frequency domain (IRs -> HRTFs) */
            kiss_fft( p_sys->p_fft_cfg, p_fft_in_l, p_data_hrtf_l + i_offset );
            kiss_fft( p_sys->p_fft_cfg, p_fft_in_r, p_data_hrtf_r + i_offset );
        }

        msg_Dbg( p_filter, "Index: %d, Azimuth: %f, Elevation: %f, Radius: %f of SOFA file.",
                 i_m[i], *(p_sys->sofa[p_sys->i_sofa_id].pf_sp_a + i_m[i]),
                 *(p_sys->sofa[p_sys->i_sofa_id].pf_sp_e + i_m[i]),
                 *(p_sys->sofa[p_sys->i_sofa_id].pf_sp_r + i_m[i]) );
    }

    /* copy IRs and delays to allocated memory in the filter_sys_t struct: */
    vlc_mutex_lock( &p_sys->lock );
    if( p_sys->i_processing_type == PROC_TIME_DOM )
    {
        memcpy ( p_sys->pf_data_ir_l, pf_data_ir_l,
            sizeof( float ) * i_n_conv * i_n_samples );
        memcpy ( p_sys->pf_data_ir_r, pf_data_ir_r,
            sizeof( float ) * i_n_conv * i_n_samples );

        free( pf_data_ir_l ); /* free temporary IR memory */
        free( pf_data_ir_r );
    }
    else if( ( p_sys->i_processing_type == PROC_FREQ_DOM ) &&
             ( p_sys->i_n_fft != 0) )
    {   /* if required length of p_sys->pf_data has changed */
        p_sys->p_data_hrtf_l = realloc( p_sys->p_data_hrtf_l,
                          sizeof( kiss_fft_cpx ) * i_n_fft * p_sys->i_n_conv );
        p_sys->p_data_hrtf_r = realloc( p_sys->p_data_hrtf_r,
                          sizeof( kiss_fft_cpx ) * i_n_fft * p_sys->i_n_conv );
        if( ( !p_sys->p_data_hrtf_l ) || ( !p_sys->p_data_hrtf_r ) )
        {
            return VLC_ENOMEM; /* memory allocation failed */
        }
        memcpy ( p_sys->p_data_hrtf_l, p_data_hrtf_l, /* copy HRTF data to */
            sizeof( kiss_fft_cpx ) * i_n_conv * i_n_fft ); /* filter struct */
        memcpy ( p_sys->p_data_hrtf_r, p_data_hrtf_r,
            sizeof( kiss_fft_cpx ) * i_n_conv * i_n_fft );

        free( p_data_hrtf_l ); /* free temporary HRTF memory */
        free( p_data_hrtf_r );
        free( p_fft_in_l ); /* free temporary FFT memory */
        free( p_fft_in_r );
    }

    memcpy ( p_sys->pi_delay_l , &pi_delay_l[0] , sizeof( int ) * p_sys->i_n_conv );
    memcpy ( p_sys->pi_delay_r , &pi_delay_r[0] , sizeof( int ) * p_sys->i_n_conv );
    vlc_mutex_unlock( &p_sys->lock );

    return VLC_SUCCESS;
}

static int FindM ( filter_sys_t *p_sys, int i_azim, int i_elev, float f_radius )
{
    /* get source positions and M of currently selected SOFA file */
    float *pf_sp_a = p_sys->sofa[p_sys->i_sofa_id].pf_sp_a; /* azimuth angle */
    float *pf_sp_e = p_sys->sofa[p_sys->i_sofa_id].pf_sp_e; /* elevation angle */
    float *pf_sp_r = p_sys->sofa[p_sys->i_sofa_id].pf_sp_r; /* radius */
    int i_m_dim = p_sys->sofa[p_sys->i_sofa_id].i_m_dim; /* no. measurements */

    int i_best_id = 0; /* index m currently closest to desired source pos. */
    float f_delta = 1000; /* offset between desired and currently best pos. */
    float f_current;
    for ( int i = 0; i < i_m_dim ; i++ )
    {   /* search through all measurements in currently selected SOFA file */
        /* distance of current to desired source position: */
        f_current = fabs ( *(pf_sp_a++) - i_azim )
                    + fabs( *(pf_sp_e++) - i_elev )
                    + fabs( *(pf_sp_r++) - f_radius );
        if ( f_current <= f_delta )
        {   /* if current distance is smaller than smallest distance so far */
            f_delta = f_current;
            i_best_id = i; /* remember index */
        }
    }
    return i_best_id;
}

/*****************************************************************************
* Close:
******************************************************************************/

static void Close( vlc_object_t *p_this )
{
    filter_t *p_filter = ( filter_t* )p_this;
    filter_sys_t *p_sys = p_filter->p_sys;
    vlc_object_t *p_out = p_filter->obj.parent;

    /* delete GUI callbacks */
    var_DelCallback( p_out, "sofalizer-gain", GainCallback, p_filter );
    var_DelCallback( p_out, "sofalizer-rotation", RotationCallback, p_filter );
    var_DelCallback( p_out, "sofalizer-elevation", ElevationCallback, p_filter );
    var_DelCallback( p_out, "sofalizer-switch", SwitchCallback, p_filter );
    var_DelCallback( p_out, "sofalizer-select", SelectCallback, p_filter );
    var_DelCallback( p_out, "sofalizer-radius", RadiusCallback, p_filter );
    var_DelCallback( p_filter->obj.libvlc, "head-rotation", HeadRotationCallback, p_filter );

    vlc_mutex_destroy( &p_sys->lock ); /* get rid of mutex lock */

    FreeAllSofa( p_filter ); /* free memory used for the SOFA files' data */
    FreeFilter( p_filter ); /* free filter memory */
}
