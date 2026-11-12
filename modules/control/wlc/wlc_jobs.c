/*****************************************************************************
 * wlc_jobs.c: WLC Control Center - batch converter built on the player's
 *             own transcoding chain, running beside normal playback
 *****************************************************************************
 * Copyright (C) 2026 Josh Wilk
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#include "wlc.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <vlc_fs.h>
#include <vlc_url.h>
#include <vlc_input_item.h>

typedef struct
{
    const char *id, *label, *ext, *mux;
    const char *vcodec, *acodec;   /* NULL vcodec: audio only */
    int vb, ab, height, samplerate;
    const char *vextra;
} wlc_profile_t;

static const wlc_profile_t profiles[] = {
    { "mp4-h264", "MP4 - H.264 + AAC (plays almost everywhere)", "mp4", "mp4",
      "h264", "mp4a", 2500, 160, 0, 0, NULL },
    { "mp4-720", "MP4 - 720p for phones and tablets", "mp4", "mp4",
      "h264", "mp4a", 1500, 128, 720, 0, NULL },
    { "mp4-480", "MP4 - 480p small file", "mp4", "mp4",
      "h264", "mp4a", 800, 96, 480, 44100, NULL },
    { "mkv-h264", "MKV - H.264 + AAC", "mkv", "ffmpeg{mux=matroska}",
      "h264", "mp4a", 2500, 160, 0, 0, NULL },
    { "webm-vp8", "WebM - VP8 + Vorbis (for the web)", "webm", "ffmpeg{mux=webm}",
      "VP80", "vorb", 2000, 128, 0, 0, NULL },
    { "ts-mpeg2", "MPEG-TS - MPEG-2 + MP2 (old TVs and DVD-era gear)", "ts", "ts",
      "mp2v", "mpga", 4000, 192, 0, 0, NULL },
    { "mp3", "MP3", "mp3", "raw", NULL, "mp3", 0, 192, 0, 44100, NULL },
    { "m4a", "AAC (.m4a)", "m4a", "mp4", NULL, "mp4a", 0, 160, 0, 0, NULL },
    { "ogg", "Ogg Vorbis", "ogg", "ogg", NULL, "vorb", 0, 160, 0, 0, NULL },
    { "opus", "Opus", "opus", "ogg", NULL, "opus", 0, 128, 0, 48000, NULL },
    { "flac", "FLAC (lossless)", "flac", "raw", NULL, "flac", 0, 0, 0, 0, NULL },
    { "wav", "WAV (16-bit PCM)", "wav", "wav", NULL, "s16l", 0, 0, 0, 0, NULL },
};

void wlc_h_profiles( wlc_req_t *req )
{
    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    vlc_memstream_puts( &ms, "{\"profiles\":[" );
    for( size_t i = 0; i < ARRAY_SIZE(profiles); i++ )
    {
        const wlc_profile_t *p = &profiles[i];
        if( i ) vlc_memstream_putc( &ms, ',' );
        vlc_memstream_printf( &ms, "{\"id\":\"%s\",\"video\":%s,\"ext\":\"%s\","
            "\"vb\":%d,\"ab\":%d,\"height\":%d,\"samplerate\":%d,\"label\":",
            p->id, p->vcodec ? "true" : "false", p->ext, p->vb, p->ab,
            p->height, p->samplerate );
        wlc_json_str( &ms, p->label );
        vlc_memstream_putc( &ms, '}' );
    }
    vlc_memstream_puts( &ms, "]}" );
    wlc_send_json( req, 200, &ms );
}

static void job_free( wlc_job_t *job )
{
    free( job->src ); free( job->dst ); free( job->part ); free( job->label );
    free( job->chain ); free( job->mux ); free( job->msg );
    free( job );
}

static bool exists( const char *path )
{
    struct stat st;
    return !vlc_lstat( path, &st );
}

/* dir/base.ext, or "base (2).ext" and so on when that name is taken */
static char *free_name( const char *dir, const char *base, const char *ext )
{
    for( int i = 1; i < 1000; i++ )
    {
        char *path;
        int r = (i == 1) ? asprintf( &path, "%s/%s.%s", dir, base, ext )
                         : asprintf( &path, "%s/%s (%d).%s", dir, base, i, ext );
        if( r < 0 )
            return NULL;
        if( !exists( path ) )
            return path;
        free( path );
    }
    return NULL;
}

static void job_start( intf_sys_t *sys, wlc_job_t *job )
{
    char *uri = vlc_path2uri( job->src, NULL );
    char *esc = config_StringEscape( job->part );
    char *sout = NULL;
    input_item_t *item = uri ? input_item_New( uri, NULL ) : NULL;

    job->state = WLC_JOB_ERROR;
    if( item != NULL && esc != NULL
     && asprintf( &sout, "sout=#%s%sstd{access=file,mux=%s,dst='%s'}",
                  job->chain, job->chain[0] ? ":" : "", job->mux, esc ) >= 0 )
    {
        input_item_AddOption( item, sout, VLC_INPUT_OPTION_TRUSTED );
        input_item_AddOption( item, "no-sout-all", VLC_INPUT_OPTION_TRUSTED );
        input_item_AddOption( item, "no-interact", VLC_INPUT_OPTION_TRUSTED );
        if( job->audio_only )
            input_item_AddOption( item, "no-video", VLC_INPUT_OPTION_TRUSTED );
        job->input = input_Create( VLC_OBJECT(sys->intf), item, NULL, NULL, NULL );
        if( job->input != NULL && input_Start( job->input ) == VLC_SUCCESS )
        {
            job->state = WLC_JOB_RUNNING;
            msg_Dbg( sys->intf, "WLC convert #%d: %s", job->id, sout );
        }
        else if( job->input != NULL )
        {
            input_Close( job->input );
            job->input = NULL;
        }
    }
    if( job->state == WLC_JOB_ERROR )
        job->msg = strdup( "could not start the conversion" );
    if( item != NULL )
        input_item_Release( item );
    free( sout );
    free( esc );
    free( uri );
}

static void job_finish( intf_sys_t *sys, wlc_job_t *job, int state )
{
    if( job->input != NULL )
    {
        input_Stop( job->input );
        input_Close( job->input );
        job->input = NULL;
    }
    struct stat st;
    if( state == WLC_JOB_DONE
     && (vlc_stat( job->part, &st ) || st.st_size < 64) )
    {
        state = WLC_JOB_ERROR;
        free( job->msg );
        job->msg = strdup( "nothing was written - this format may not be "
                           "supported for that file" );
    }
    if( state == WLC_JOB_DONE )
    {
        /* pick the final name only now, so nothing is ever overwritten */
        char *dir = strdup( job->dst );
        char *slash = dir ? strrchr( dir, '/' ) : NULL;
        char *final = NULL;
        if( slash != NULL )
        {
            *slash = '\0';
            char *base = slash + 1;
            char *dot = strrchr( base, '.' );
            if( dot != NULL )
            {
                *dot = '\0';
                final = free_name( dir, base, dot + 1 );
            }
        }
        if( final != NULL && !vlc_rename( job->part, final ) )
        {
            free( job->dst );
            job->dst = final;
            job->progress = 1.f;
            sys->lib_valid = false;
        }
        else
        {
            free( final );
            state = WLC_JOB_ERROR;
            job->msg = strdup( "could not move the finished file into place" );
        }
        free( dir );
    }
    if( state != WLC_JOB_DONE )
        vlc_unlink( job->part );
    job->state = state;
}

static void *job_thread( void *data )
{
    intf_sys_t *sys = data;

    while( !atomic_load( &sys->stop ) )
    {
        vlc_mutex_lock( &sys->lock );
        int running = 0;
        for( size_t i = 0; i < sys->njobs; i++ )
        {
            wlc_job_t *job = sys->jobs[i];
            if( job->state == WLC_JOB_QUEUED && job->cancel )
                job->state = WLC_JOB_CANCELLED;
            if( job->state != WLC_JOB_RUNNING )
                continue;
            const int st = var_GetInteger( job->input, "state" );
            job->progress = var_GetFloat( job->input, "position" );
            if( job->cancel )
                job_finish( sys, job, WLC_JOB_CANCELLED );
            else if( st == ERROR_S )
            {
                job->msg = strdup( "the file could not be read or encoded" );
                job_finish( sys, job, WLC_JOB_ERROR );
            }
            else if( st == END_S )
                job_finish( sys, job, WLC_JOB_DONE );
            else
                running++;
        }
        for( size_t i = 0; i < sys->njobs && running < sys->parallel; i++ )
            if( sys->jobs[i]->state == WLC_JOB_QUEUED )
            {
                job_start( sys, sys->jobs[i] );
                if( sys->jobs[i]->state == WLC_JOB_RUNNING )
                    running++;
            }
        vlc_mutex_unlock( &sys->lock );
        msleep( 250000 );
    }
    return NULL;
}

int wlc_jobs_start( intf_sys_t *sys )
{
    sys->job_thread_ok = !vlc_clone( &sys->job_thread, job_thread, sys,
                                     VLC_THREAD_PRIORITY_LOW );
    return sys->job_thread_ok ? VLC_SUCCESS : VLC_EGENERIC;
}

void wlc_jobs_stop( intf_sys_t *sys )
{
    if( sys->job_thread_ok )
        vlc_join( sys->job_thread, NULL );
    for( size_t i = 0; i < sys->njobs; i++ )
    {
        wlc_job_t *job = sys->jobs[i];
        if( job->state == WLC_JOB_RUNNING )
            job_finish( sys, job, WLC_JOB_CANCELLED );
        job_free( job );
    }
    free( sys->jobs );
    sys->jobs = NULL;
    sys->njobs = 0;
}

static bool custom_ok( const char *s )
{
    /* transcode options only: no way to add another output or a path */
    if( strlen( s ) > 400 )
        return false;
    for( ; *s; s++ )
        if( strchr( "#:'\"\\/", *s ) != NULL || (unsigned char)*s < 0x20 )
            return false;
    return true;
}

void wlc_h_convert( wlc_req_t *req )
{
    intf_sys_t *sys = req->sys;
    const json_value *j = req->json;
    const json_value *files = wlc_jget( j, "files" );
    const char *pid = wlc_jstr( j, "profile" );
    const char *out = wlc_jstr( j, "out" );
    const char *sub = wlc_jstr( j, "sub" );
    const char *custom = wlc_jstr( j, "custom" );
    const wlc_profile_t *p = NULL;

    for( size_t i = 0; pid != NULL && i < ARRAY_SIZE(profiles); i++ )
        if( !strcmp( profiles[i].id, pid ) )
            p = &profiles[i];
    if( p == NULL || files == NULL || files->type != json_array
     || files->u.array.length == 0 )
    {
        wlc_error( req, 400, "pick files and a format first" );
        return;
    }
    if( custom != NULL && custom[0] != '\0' && !custom_ok( custom ) )
    {
        wlc_error( req, 400, "custom options may not contain # : / or quotes" );
        return;
    }
    if( sub != NULL && (strchr( sub, '/' ) != NULL || !strcmp( sub, ".." )
                     || sub[0] == '.') )
        sub = NULL;
    if( sub == NULL || sub[0] == '\0' )
        sub = "Converted";

    /* the transcode{} part, shared by all files of this request */
    const int vb = (int)wlc_jnum( j, "vb", p->vb );
    const int ab = (int)wlc_jnum( j, "ab", p->ab );
    const int height = (int)wlc_jnum( j, "height", p->height );
    const int rate = (int)wlc_jnum( j, "samplerate", p->samplerate );
    const int channels = (int)wlc_jnum( j, "channels", 0 );
    const double fps = wlc_jnum( j, "fps", 0. );
    const bool remux = wlc_jbool( j, "remux", false );
    struct vlc_memstream cs;
    vlc_memstream_open( &cs );
    if( !remux )
    {
        vlc_memstream_puts( &cs, "transcode{" );
        if( custom != NULL && custom[0] != '\0' )
            vlc_memstream_puts( &cs, custom );
        else
        {
            if( p->vcodec != NULL )
            {
                vlc_memstream_printf( &cs, "vcodec=%s", p->vcodec );
                if( vb >= 50 && vb <= 100000 )
                    vlc_memstream_printf( &cs, ",vb=%d", vb );
                if( height >= 96 && height <= 4320 )
                    vlc_memstream_printf( &cs, ",height=%d", height );
                if( fps >= 1. && fps <= 120. )
                    vlc_memstream_printf( &cs, ",fps=%d", (int)fps );
            }
            else
                vlc_memstream_puts( &cs, "vcodec=none" );
            vlc_memstream_printf( &cs, ",acodec=%s", p->acodec );
            if( ab >= 8 && ab <= 1024 )
                vlc_memstream_printf( &cs, ",ab=%d", ab );
            if( rate >= 8000 && rate <= 192000 )
                vlc_memstream_printf( &cs, ",samplerate=%d", rate );
            if( channels >= 1 && channels <= 8 )
                vlc_memstream_printf( &cs, ",channels=%d", channels );
            vlc_memstream_puts( &cs, ",scodec=none" );
        }
        vlc_memstream_putc( &cs, '}' );
    }
    if( vlc_memstream_close( &cs ) )
    {
        wlc_error( req, 500, "out of memory" );
        return;
    }

    unsigned queued = 0;
    vlc_mutex_lock( &sys->lock );
    for( unsigned i = 0; i < files->u.array.length; i++ )
    {
        const json_value *f = files->u.array.values[i];
        if( f->type != json_string )
            continue;
        char *real = wlc_realpath( f->u.string.ptr );
        struct stat st;
        if( real == NULL || vlc_stat( real, &st ) || !S_ISREG( st.st_mode ) )
        {
            free( real );
            continue;
        }
        /* where it goes */
        char *dir = strdup( real );
        char *slash = strrchr( dir, '/' );
        char *base = strdup( slash + 1 );
        *slash = '\0';
        char *dot = strrchr( base, '.' );
        if( dot != NULL && dot != base )
            *dot = '\0';
        char *outdir = NULL;
        if( out != NULL && out[0] == '/' )
            outdir = strdup( out );
        else if( out != NULL && !strcmp( out, "same" ) )
            outdir = strdup( dir[0] ? dir : "/" );
        else if( asprintf( &outdir, "%s/%s", dir, sub ) < 0 )
            outdir = NULL;

        wlc_job_t *job = calloc( 1, sizeof(*job) );
        wlc_job_t **nj = realloc( sys->jobs, (sys->njobs + 1) * sizeof(*nj) );
        if( nj != NULL )
            sys->jobs = nj;
        if( job != NULL && nj != NULL && outdir != NULL )
        {
            vlc_mkdir( outdir, 0755 );
            job->id = ++sys->job_seq;
            job->src = real;
            real = NULL;
            job->label = strdup( p->label );
            job->chain = strdup( cs.ptr );
            job->mux = strdup( p->mux );
            job->audio_only = p->vcodec == NULL && !remux;
            if( asprintf( &job->dst, "%s/%s.%s", outdir, base, p->ext ) < 0 )
                job->dst = NULL;
            if( asprintf( &job->part, "%s/.wlc-%d-%d.part", outdir,
                          (int)getpid(), job->id ) < 0 )
                job->part = NULL;
            if( job->dst && job->part && job->chain && job->mux )
            {
                job->state = WLC_JOB_QUEUED;
                sys->jobs[sys->njobs++] = job;
                job = NULL;
                queued++;
            }
        }
        if( job != NULL )
            job_free( job );
        free( outdir ); free( base ); free( dir ); free( real );
    }
    vlc_mutex_unlock( &sys->lock );
    free( cs.ptr );

    char buf[64];
    int n = snprintf( buf, sizeof(buf), "{\"ok\":true,\"queued\":%u}", queued );
    wlc_send( req, 200, "application/json", NULL, buf, n );
}

void wlc_h_jobs( wlc_req_t *req )
{
    static const char *const names[] = { "queued", "running", "done", "error",
                                         "cancelled" };
    intf_sys_t *sys = req->sys;
    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    vlc_memstream_puts( &ms, "{\"jobs\":[" );
    vlc_mutex_lock( &sys->lock );
    for( size_t i = 0; i < sys->njobs; i++ )
    {
        const wlc_job_t *job = sys->jobs[i];
        if( i ) vlc_memstream_putc( &ms, ',' );
        vlc_memstream_printf( &ms, "{\"id\":%d,\"state\":\"%s\",\"progress\":%.4f,"
            "\"src\":", job->id, names[job->state], (double)job->progress );
        wlc_json_str( &ms, job->src );
        vlc_memstream_puts( &ms, ",\"dst\":" );
        wlc_json_str( &ms, job->dst );
        vlc_memstream_puts( &ms, ",\"label\":" );
        wlc_json_str( &ms, job->label );
        vlc_memstream_puts( &ms, ",\"msg\":" );
        wlc_json_str( &ms, job->msg ? job->msg : "" );
        vlc_memstream_putc( &ms, '}' );
    }
    vlc_mutex_unlock( &sys->lock );
    vlc_memstream_puts( &ms, "]}" );
    wlc_send_json( req, 200, &ms );
}

void wlc_h_jobs_cancel( wlc_req_t *req )
{
    intf_sys_t *sys = req->sys;
    const int id = (int)wlc_jnum( req->json, "id", -1 );
    const bool all = wlc_jbool( req->json, "all", false );
    const bool clear = wlc_jbool( req->json, "clear", false );

    vlc_mutex_lock( &sys->lock );
    for( size_t i = 0; i < sys->njobs; i++ )
    {
        wlc_job_t *job = sys->jobs[i];
        if( (all || job->id == id)
         && (job->state == WLC_JOB_QUEUED || job->state == WLC_JOB_RUNNING) )
            job->cancel = true;
    }
    if( clear )
    {
        size_t k = 0;
        for( size_t i = 0; i < sys->njobs; i++ )
        {
            wlc_job_t *job = sys->jobs[i];
            if( job->state >= WLC_JOB_DONE )
                job_free( job );
            else
                sys->jobs[k++] = job;
        }
        sys->njobs = k;
    }
    vlc_mutex_unlock( &sys->lock );
    wlc_ok( req );
}
