/*****************************************************************************
 * wlceq.c: WLC Power EQ - 10/15/31-band graphic + 12-band parametric
 *          equalizer with preamp, stereo tools and a clip-guard limiter
 *****************************************************************************
 * Copyright (C) 2026 Josh Wilk
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
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_aout.h>
#include <vlc_filter.h>
#include <vlc_charset.h>

#ifndef M_PI
# define M_PI 3.14159265358979323846
#endif

#define WLCEQ_MAX_GRAPHIC 31
#define WLCEQ_MAX_PARAM   12
#define WLCEQ_MAX_STAGES  (WLCEQ_MAX_GRAPHIC + WLCEQ_MAX_PARAM)
#define WLCEQ_MAX_CH      AOUT_CHAN_MAX
#define WLCEQ_GAIN_LIMIT  24.0f

static int  Open ( vlc_object_t * );
static void Close( vlc_object_t * );

#define GRAPHIC_TEXT N_("Graphic band gains")
#define GRAPHIC_LONGTEXT N_( \
    "10, 15 or 31 gains in dB (-24 to 24) separated by spaces. The number " \
    "of values picks the ISO band layout: 10 = octave, 15 = 2/3 octave, " \
    "31 = 1/3 octave (20 Hz to 20 kHz)." )
#define PARAM_TEXT N_("Parametric bands")
#define PARAM_LONGTEXT N_( \
    "Up to 12 bands as \"type,frequency,gain,Q\" separated by semicolons. " \
    "Types: peak, lowshelf, highshelf, lowpass, highpass, notch, bandpass. " \
    "Example: \"highpass,30,0,0.7;peak,3000,-2.5,1.4\"." )
#define PREAMP_TEXT N_("Preamp")
#define PREAMP_LONGTEXT N_("Gain applied before the equalizer, in dB (-24 to 24).")
#define LIMITER_TEXT N_("Clip guard")
#define LIMITER_LONGTEXT N_( \
    "Pull the level down for a moment when boosts would clip, instead of " \
    "distorting." )
#define BALANCE_TEXT N_("Balance")
#define BALANCE_LONGTEXT N_("Left/right balance for stereo, -1 (left) to 1 (right).")
#define WIDTH_TEXT N_("Stereo width")
#define WIDTH_LONGTEXT N_("0 = mono, 1 = unchanged, 2 = extra wide. Stereo only.")

vlc_module_begin ()
    set_description( N_("WLC Power EQ (graphic + parametric)") )
    set_shortname( N_("Power EQ") )
    set_capability( "audio filter", 0 )
    set_category( CAT_AUDIO )
    set_subcategory( SUBCAT_AUDIO_AFILTER )
    add_string( "wlceq-graphic", "", GRAPHIC_TEXT, GRAPHIC_LONGTEXT, false )
    add_string( "wlceq-parametric", "", PARAM_TEXT, PARAM_LONGTEXT, false )
    add_float_with_range( "wlceq-preamp", 0.f, -24.f, 24.f, PREAMP_TEXT,
                          PREAMP_LONGTEXT, false )
    add_bool( "wlceq-limiter", true, LIMITER_TEXT, LIMITER_LONGTEXT, false )
    add_float_with_range( "wlceq-balance", 0.f, -1.f, 1.f, BALANCE_TEXT,
                          BALANCE_LONGTEXT, false )
    add_float_with_range( "wlceq-width", 1.f, 0.f, 2.f, WIDTH_TEXT,
                          WIDTH_LONGTEXT, false )
    set_callbacks( Open, Close )
    add_shortcut( "wlceq" )
vlc_module_end ()

typedef struct
{
    float b0, b1, b2, a1, a2;
} biquad_t;

struct filter_sys_t
{
    vlc_mutex_t lock;
    unsigned    rate;
    unsigned    channels;

    unsigned    stages;
    biquad_t    bq[WLCEQ_MAX_STAGES];
    float       z[WLCEQ_MAX_CH][WLCEQ_MAX_STAGES][2];

    float       preamp;      /* linear */
    bool        limiter;
    float       lim_gain;    /* current limiter gain */
    float       lim_release; /* per-frame release coefficient */
    float       balance;
    float       width;

    /* raw settings, kept so that one changed variable can rebuild all */
    char       *graphic;
    char       *parametric;
};

enum { T_PEAK, T_LOWSHELF, T_HIGHSHELF, T_LOWPASS, T_HIGHPASS, T_NOTCH,
       T_BANDPASS };

static const float iso31[31] = {
    20, 25, 31.5f, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500,
    630, 800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000,
    10000, 12500, 16000, 20000 };
static const float iso15[15] = {
    25, 40, 63, 100, 160, 250, 400, 630, 1000, 1600, 2500, 4000, 6300,
    10000, 16000 };
static const float iso10[10] = {
    31.5f, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };

static float clampf( float v, float lo, float hi )
{
    if( !(v >= lo) ) return lo; /* also catches NaN */
    if( v > hi ) return hi;
    return v;
}

/* RBJ audio EQ cookbook biquads. Returns false if the band is a no-op or
 * cannot exist at this sample rate. */
static bool MakeBiquad( biquad_t *bq, int type, float freq, float gain_db,
                        float q, unsigned rate )
{
    if( freq < 10.f || freq > rate * 0.49f )
        return false;
    q = clampf( q, 0.1f, 30.f );
    gain_db = clampf( gain_db, -WLCEQ_GAIN_LIMIT, WLCEQ_GAIN_LIMIT );
    if( (type == T_PEAK || type == T_LOWSHELF || type == T_HIGHSHELF)
     && fabsf( gain_db ) < 0.01f )
        return false;

    const double A = pow( 10.0, gain_db / 40.0 );
    const double w0 = 2.0 * M_PI * freq / rate;
    const double cw = cos( w0 ), sw = sin( w0 );
    const double alpha = sw / (2.0 * q);
    double b0, b1, b2, a0, a1, a2;

    switch( type )
    {
        case T_PEAK:
            b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
            a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
            break;
        case T_LOWSHELF:
        {
            const double s = 2 * sqrt( A ) * alpha;
            b0 = A * ((A + 1) - (A - 1) * cw + s);
            b1 = 2 * A * ((A - 1) - (A + 1) * cw);
            b2 = A * ((A + 1) - (A - 1) * cw - s);
            a0 = (A + 1) + (A - 1) * cw + s;
            a1 = -2 * ((A - 1) + (A + 1) * cw);
            a2 = (A + 1) + (A - 1) * cw - s;
            break;
        }
        case T_HIGHSHELF:
        {
            const double s = 2 * sqrt( A ) * alpha;
            b0 = A * ((A + 1) + (A - 1) * cw + s);
            b1 = -2 * A * ((A - 1) + (A + 1) * cw);
            b2 = A * ((A + 1) + (A - 1) * cw - s);
            a0 = (A + 1) - (A - 1) * cw + s;
            a1 = 2 * ((A - 1) - (A + 1) * cw);
            a2 = (A + 1) - (A - 1) * cw - s;
            break;
        }
        case T_LOWPASS:
            b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2;
            a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
            break;
        case T_HIGHPASS:
            b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2;
            a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
            break;
        case T_NOTCH:
            b0 = 1; b1 = -2 * cw; b2 = 1;
            a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
            break;
        case T_BANDPASS:
            b0 = alpha; b1 = 0; b2 = -alpha;
            a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
            break;
        default:
            return false;
    }
    bq->b0 = b0 / a0; bq->b1 = b1 / a0; bq->b2 = b2 / a0;
    bq->a1 = a1 / a0; bq->a2 = a2 / a0;
    return true;
}

static int ParseType( const char *s, size_t len )
{
    static const struct { const char *name; int type; } types[] = {
        { "peak", T_PEAK }, { "lowshelf", T_LOWSHELF },
        { "highshelf", T_HIGHSHELF }, { "lowpass", T_LOWPASS },
        { "highpass", T_HIGHPASS }, { "notch", T_NOTCH },
        { "bandpass", T_BANDPASS },
    };
    for( size_t i = 0; i < ARRAY_SIZE(types); i++ )
        if( strlen( types[i].name ) == len
         && !strncmp( types[i].name, s, len ) )
            return types[i].type;
    return -1;
}

/* Rebuild the whole biquad chain from the raw strings. Lock must be held.
 * Filter state is kept when the stage count is unchanged so that moving a
 * slider does not click. */
static void Rebuild( filter_sys_t *sys )
{
    biquad_t bq[WLCEQ_MAX_STAGES];
    unsigned n = 0;

    /* graphic section */
    float gains[WLCEQ_MAX_GRAPHIC];
    unsigned count = 0;
    if( sys->graphic != NULL )
    {
        const char *p = sys->graphic;
        while( *p != '\0' && count < WLCEQ_MAX_GRAPHIC )
        {
            char *end;
            float f = us_strtof( p, &end );
            if( end == p )
                break;
            gains[count++] = f;
            p = end;
            while( *p == ' ' || *p == ',' )
                p++;
        }
    }
    const float *freqs = NULL;
    float q = 1.41f;
    if( count == 31 )      { freqs = iso31; q = 4.32f; }
    else if( count == 15 ) { freqs = iso15; q = 2.15f; }
    else if( count == 10 ) { freqs = iso10; q = 1.41f; }
    if( freqs != NULL )
        for( unsigned i = 0; i < count; i++ )
            if( MakeBiquad( &bq[n], T_PEAK, freqs[i], gains[i], q,
                            sys->rate ) )
                n++;

    /* parametric section */
    if( sys->parametric != NULL )
    {
        const char *p = sys->parametric;
        unsigned bands = 0;
        while( *p != '\0' && bands < WLCEQ_MAX_PARAM )
        {
            const char *semi = strchr( p, ';' );
            size_t len = semi ? (size_t)(semi - p) : strlen( p );
            const char *comma = memchr( p, ',', len );
            if( comma != NULL )
            {
                int type = ParseType( p, comma - p );
                char *end;
                float f = us_strtof( comma + 1, &end );
                float g = 0.f, bandq = 1.f;
                if( *end == ',' )
                {
                    g = us_strtof( end + 1, &end );
                    if( *end == ',' )
                        bandq = us_strtof( end + 1, &end );
                }
                if( type >= 0
                 && MakeBiquad( &bq[n], type, f, g, bandq, sys->rate ) )
                    n++;
                bands++;
            }
            if( semi == NULL )
                break;
            p = semi + 1;
        }
    }

    if( n != sys->stages )
        memset( sys->z, 0, sizeof(sys->z) );
    memcpy( sys->bq, bq, n * sizeof(*bq) );
    sys->stages = n;
}

static block_t *DoWork( filter_t *filter, block_t *block )
{
    filter_sys_t *sys = filter->p_sys;
    float *s = (float *)block->p_buffer;
    const unsigned ch = sys->channels;
    const unsigned frames = block->i_nb_samples;

    vlc_mutex_lock( &sys->lock );
    const unsigned stages = sys->stages;
    const float pre = sys->preamp;

    for( unsigned c = 0; c < ch; c++ )
    {
        float *p = s + c;
        for( unsigned st = 0; st < stages; st++ )
        {
            const biquad_t b = sys->bq[st];
            float z1 = sys->z[c][st][0], z2 = sys->z[c][st][1];
            float *q = p;
            for( unsigned i = 0; i < frames; i++, q += ch )
            {
                const float x = (st == 0) ? *q * pre : *q;
                const float y = b.b0 * x + z1;
                z1 = b.b1 * x - b.a1 * y + z2;
                z2 = b.b2 * x - b.a2 * y;
                *q = y;
            }
            /* flush denormals */
            if( fabsf( z1 ) < 1e-20f ) z1 = 0.f;
            if( fabsf( z2 ) < 1e-20f ) z2 = 0.f;
            sys->z[c][st][0] = z1; sys->z[c][st][1] = z2;
        }
        if( stages == 0 && pre != 1.f )
            for( unsigned i = 0; i < frames; i++ )
                p[i * ch] *= pre;
    }

    if( ch == 2 && (sys->width != 1.f || sys->balance != 0.f) )
    {
        const float w = sys->width;
        const float gl = sys->balance > 0.f ? 1.f - sys->balance : 1.f;
        const float gr = sys->balance < 0.f ? 1.f + sys->balance : 1.f;
        for( unsigned i = 0; i < frames; i++ )
        {
            const float mid = (s[2*i] + s[2*i+1]) * .5f;
            const float side = (s[2*i] - s[2*i+1]) * .5f * w;
            s[2*i]   = (mid + side) * gl;
            s[2*i+1] = (mid - side) * gr;
        }
    }

    if( sys->limiter )
    {
        float g = sys->lim_gain;
        const float rel = sys->lim_release;
        const float ceiling = 0.985f;
        for( unsigned i = 0; i < frames; i++ )
        {
            float peak = 0.f;
            for( unsigned c = 0; c < ch; c++ )
            {
                const float a = fabsf( s[i * ch + c] );
                if( a > peak ) peak = a;
            }
            if( peak * g > ceiling )
                g = ceiling / peak;          /* instant attack */
            else if( g < 1.f )
                g = 1.f - (1.f - g) * rel;   /* smooth release */
            if( g != 1.f )
                for( unsigned c = 0; c < ch; c++ )
                    s[i * ch + c] *= g;
        }
        sys->lim_gain = g;
    }
    vlc_mutex_unlock( &sys->lock );
    return block;
}

static int StringCallback( vlc_object_t *obj, char const *var,
                           vlc_value_t oldval, vlc_value_t newval, void *data )
{
    VLC_UNUSED(obj); VLC_UNUSED(oldval);
    filter_sys_t *sys = data;
    char *copy = newval.psz_string ? strdup( newval.psz_string ) : NULL;

    vlc_mutex_lock( &sys->lock );
    char **slot = !strcmp( var, "wlceq-graphic" ) ? &sys->graphic
                                                  : &sys->parametric;
    free( *slot );
    *slot = copy;
    Rebuild( sys );
    vlc_mutex_unlock( &sys->lock );
    return VLC_SUCCESS;
}

static int FloatCallback( vlc_object_t *obj, char const *var,
                          vlc_value_t oldval, vlc_value_t newval, void *data )
{
    VLC_UNUSED(obj); VLC_UNUSED(oldval);
    filter_sys_t *sys = data;

    vlc_mutex_lock( &sys->lock );
    if( !strcmp( var, "wlceq-preamp" ) )
        sys->preamp = powf( 10.f, clampf( newval.f_float, -24.f, 24.f ) / 20.f );
    else if( !strcmp( var, "wlceq-balance" ) )
        sys->balance = clampf( newval.f_float, -1.f, 1.f );
    else
        sys->width = clampf( newval.f_float, 0.f, 2.f );
    vlc_mutex_unlock( &sys->lock );
    return VLC_SUCCESS;
}

static int BoolCallback( vlc_object_t *obj, char const *var,
                         vlc_value_t oldval, vlc_value_t newval, void *data )
{
    VLC_UNUSED(obj); VLC_UNUSED(var); VLC_UNUSED(oldval);
    filter_sys_t *sys = data;

    vlc_mutex_lock( &sys->lock );
    sys->limiter = newval.b_bool;
    sys->lim_gain = 1.f;
    vlc_mutex_unlock( &sys->lock );
    return VLC_SUCCESS;
}

static const char *const string_vars[] = { "wlceq-graphic", "wlceq-parametric" };
static const char *const float_vars[] = { "wlceq-preamp", "wlceq-balance",
                                          "wlceq-width" };

static int Open( vlc_object_t *obj )
{
    filter_t *filter = (filter_t *)obj;
    vlc_object_t *aout = filter->obj.parent;

    filter_sys_t *sys = calloc( 1, sizeof(*sys) );
    if( unlikely(sys == NULL) )
        return VLC_ENOMEM;

    filter->fmt_in.audio.i_format = VLC_CODEC_FL32;
    aout_FormatPrepare( &filter->fmt_in.audio );
    filter->fmt_out.audio = filter->fmt_in.audio;

    vlc_mutex_init( &sys->lock );
    sys->rate = filter->fmt_in.audio.i_rate;
    sys->channels = aout_FormatNbChannels( &filter->fmt_in.audio );
    if( sys->channels == 0 || sys->channels > WLCEQ_MAX_CH || sys->rate == 0 )
    {
        vlc_mutex_destroy( &sys->lock );
        free( sys );
        return VLC_EGENERIC;
    }
    sys->lim_gain = 1.f;
    sys->lim_release = expf( -1.f / (0.25f * sys->rate) ); /* 250 ms */

    /* The variables live on the audio output so that interfaces can change
     * them while playing. */
    for( size_t i = 0; i < ARRAY_SIZE(string_vars); i++ )
        var_Create( aout, string_vars[i], VLC_VAR_STRING | VLC_VAR_DOINHERIT );
    for( size_t i = 0; i < ARRAY_SIZE(float_vars); i++ )
        var_Create( aout, float_vars[i], VLC_VAR_FLOAT | VLC_VAR_DOINHERIT );
    var_Create( aout, "wlceq-limiter", VLC_VAR_BOOL | VLC_VAR_DOINHERIT );

    sys->graphic = var_GetString( aout, "wlceq-graphic" );
    sys->parametric = var_GetString( aout, "wlceq-parametric" );
    sys->preamp = powf( 10.f,
        clampf( var_GetFloat( aout, "wlceq-preamp" ), -24.f, 24.f ) / 20.f );
    sys->balance = clampf( var_GetFloat( aout, "wlceq-balance" ), -1.f, 1.f );
    sys->width = clampf( var_GetFloat( aout, "wlceq-width" ), 0.f, 2.f );
    sys->limiter = var_GetBool( aout, "wlceq-limiter" );
    Rebuild( sys );

    filter->p_sys = sys;
    filter->pf_audio_filter = DoWork;

    for( size_t i = 0; i < ARRAY_SIZE(string_vars); i++ )
        var_AddCallback( aout, string_vars[i], StringCallback, sys );
    for( size_t i = 0; i < ARRAY_SIZE(float_vars); i++ )
        var_AddCallback( aout, float_vars[i], FloatCallback, sys );
    var_AddCallback( aout, "wlceq-limiter", BoolCallback, sys );

    msg_Dbg( filter, "WLC Power EQ: %u stage(s), %u channel(s) at %u Hz",
             sys->stages, sys->channels, sys->rate );
    return VLC_SUCCESS;
}

static void Close( vlc_object_t *obj )
{
    filter_t *filter = (filter_t *)obj;
    filter_sys_t *sys = filter->p_sys;
    vlc_object_t *aout = filter->obj.parent;

    for( size_t i = 0; i < ARRAY_SIZE(string_vars); i++ )
        var_DelCallback( aout, string_vars[i], StringCallback, sys );
    for( size_t i = 0; i < ARRAY_SIZE(float_vars); i++ )
        var_DelCallback( aout, float_vars[i], FloatCallback, sys );
    var_DelCallback( aout, "wlceq-limiter", BoolCallback, sys );

    vlc_mutex_destroy( &sys->lock );
    free( sys->graphic );
    free( sys->parametric );
    free( sys );
}
