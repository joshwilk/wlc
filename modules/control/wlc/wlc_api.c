/*****************************************************************************
 * wlc_api.c: WLC Control Center - player remote, media library, bulk
 *            renamer and Power EQ endpoints
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

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <vlc_aout.h>
#include <vlc_fs.h>
#include <vlc_url.h>
#include <vlc_input_item.h>
#include <vlc_meta.h>

/*****************************************************************************
 * Player status and remote control
 *****************************************************************************/
void wlc_status_json( intf_sys_t *sys, struct vlc_memstream *ms )
{
    playlist_t *pl = sys->pl;
    input_thread_t *in = playlist_CurrentInput( pl );
    const char *state = "stopped";
    double pos = 0., len = 0., rate = 1.;
    char *title = NULL, *artist = NULL, *album = NULL, *path = NULL;

    if( in != NULL )
    {
        switch( var_GetInteger( in, "state" ) )
        {
            case PLAYING_S: state = "playing"; break;
            case PAUSE_S:   state = "paused"; break;
            case OPENING_S: state = "opening"; break;
            default: break;
        }
        pos = var_GetInteger( in, "time" ) / (double)CLOCK_FREQ;
        len = var_GetInteger( in, "length" ) / (double)CLOCK_FREQ;
        rate = var_GetFloat( in, "rate" );
        input_item_t *item = input_GetItem( in );
        if( item != NULL )
        {
            title = input_item_GetTitleFbName( item );
            artist = input_item_GetArtist( item );
            album = input_item_GetAlbum( item );
            char *uri = input_item_GetURI( item );
            if( uri != NULL )
            {
                path = vlc_uri2path( uri );
                if( path == NULL )
                    path = uri;
                else
                    free( uri );
            }
        }
        vlc_object_release( in );
    }

    vlc_memstream_printf( ms, "{\"state\":\"%s\",\"time\":%.2f,\"length\":%.2f,"
        "\"rate\":%.3f,\"volume\":%.3f,\"random\":%s,\"loop\":%s,\"repeat\":%s,"
        "\"title\":", state, pos, len, rate,
        (double)playlist_VolumeGet( pl ),
        var_GetBool( pl, "random" ) ? "true" : "false",
        var_GetBool( pl, "loop" ) ? "true" : "false",
        var_GetBool( pl, "repeat" ) ? "true" : "false" );
    wlc_json_str( ms, title ? title : "" );
    vlc_memstream_puts( ms, ",\"artist\":" );
    wlc_json_str( ms, artist ? artist : "" );
    vlc_memstream_puts( ms, ",\"album\":" );
    wlc_json_str( ms, album ? album : "" );
    vlc_memstream_puts( ms, ",\"path\":" );
    wlc_json_str( ms, path ? path : "" );
    vlc_mutex_lock( &sys->lock );
    vlc_memstream_printf( ms, ",\"eq\":%s,\"name\":", sys->eq_on ? "true" : "false" );
    wlc_json_str( ms, sys->name );
    vlc_mutex_unlock( &sys->lock );
    vlc_memstream_putc( ms, '}' );
    free( title ); free( artist ); free( album ); free( path );
}

void wlc_h_status( wlc_req_t *req )
{
    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    wlc_status_json( req->sys, &ms );
    wlc_send_json( req, 200, &ms );
}

static bool url_castable( const char *url )
{
    return !strncasecmp( url, "http://", 7 ) || !strncasecmp( url, "https://", 8 );
}

int wlc_player_cmd( wlc_req_t *req, const char *cmd, const json_value *args,
                    const char **err )
{
    intf_sys_t *sys = req->sys;
    playlist_t *pl = sys->pl;
    *err = "unknown command";
    if( cmd == NULL )
        return VLC_EGENERIC;

    if( !strcmp( cmd, "play" ) || !strcmp( cmd, "resume" ) )
    {
        input_thread_t *in = playlist_CurrentInput( pl );
        if( in != NULL )
        {
            if( var_GetInteger( in, "state" ) == PAUSE_S )
                var_SetInteger( in, "state", PLAYING_S );
            vlc_object_release( in );
        }
        else
            playlist_Play( pl );
    }
    else if( !strcmp( cmd, "pause" ) )
    {
        input_thread_t *in = playlist_CurrentInput( pl );
        if( in != NULL )
        {
            if( var_GetInteger( in, "state" ) == PLAYING_S )
                var_SetInteger( in, "state", PAUSE_S );
            vlc_object_release( in );
        }
    }
    else if( !strcmp( cmd, "toggle" ) )
    {
        if( playlist_Status( pl ) == PLAYLIST_STOPPED )
            playlist_Play( pl );
        else
            playlist_TogglePause( pl );
    }
    else if( !strcmp( cmd, "stop" ) )
        playlist_Stop( pl );
    else if( !strcmp( cmd, "next" ) )
        playlist_Next( pl );
    else if( !strcmp( cmd, "prev" ) )
        playlist_Prev( pl );
    else if( !strcmp( cmd, "seek" ) || !strcmp( cmd, "seekrel" ) )
    {
        input_thread_t *in = playlist_CurrentInput( pl );
        if( in != NULL )
        {
            double v = wlc_jnum( args, "value", 0. );
            mtime_t t = (mtime_t)(v * CLOCK_FREQ);
            if( cmd[4] == 'r' )
                t += var_GetInteger( in, "time" );
            if( t < 0 ) t = 0;
            var_SetInteger( in, "time", t );
            vlc_object_release( in );
        }
    }
    else if( !strcmp( cmd, "volume" ) )
    {
        double v = wlc_jnum( args, "value", 1. );
        if( v < 0. ) v = 0.;
        if( v > 2. ) v = 2.;
        playlist_VolumeSet( pl, (float)v );
    }
    else if( !strcmp( cmd, "rate" ) )
    {
        double v = wlc_jnum( args, "value", 1. );
        if( v < 0.25 ) v = 0.25;
        if( v > 4. ) v = 4.;
        input_thread_t *in = playlist_CurrentInput( pl );
        if( in != NULL )
        {
            var_SetFloat( in, "rate", (float)v );
            vlc_object_release( in );
        }
    }
    else if( !strcmp( cmd, "random" ) || !strcmp( cmd, "loop" )
          || !strcmp( cmd, "repeat" ) || !strcmp( cmd, "fullscreen" ) )
        var_ToggleBool( pl, cmd );
    else if( !strcmp( cmd, "clear" ) )
        playlist_Clear( pl, pl_Unlocked );
    else if( !strcmp( cmd, "goto" ) )
    {
        int id = (int)wlc_jnum( args, "id", -1 );
        playlist_Lock( pl );
        playlist_item_t *item = playlist_ItemGetById( pl, id );
        if( item != NULL )
            playlist_ViewPlay( pl, NULL, item );
        playlist_Unlock( pl );
        if( item == NULL )
        {
            *err = "no such playlist item";
            return VLC_EGENERIC;
        }
    }
    else if( !strcmp( cmd, "add" ) )
    {
        const bool play = wlc_jbool( args, "play", true );
        const char *title = wlc_jstr( args, "title" );
        const char *url = wlc_jstr( args, "url" );
        const json_value *paths = wlc_jget( args, "paths" );
        const char *one = wlc_jstr( args, "path" );
        unsigned added = 0;

        if( url != NULL )
        {
            if( !url_castable( url ) )
            {
                *err = "only http and https addresses can be opened remotely";
                return VLC_EGENERIC;
            }
            playlist_AddExt( pl, url, title, play, 0, NULL, 0, true );
            added++;
        }
        const unsigned n = one ? 1
            : (paths && paths->type == json_array ? paths->u.array.length : 0);
        for( unsigned i = 0; i < n; i++ )
        {
            const char *p = one;
            if( p == NULL )
            {
                const json_value *v = paths->u.array.values[i];
                if( v->type != json_string )
                    continue;
                p = v->u.string.ptr;
            }
            char *real = wlc_realpath( p );
            if( real != NULL && wlc_path_ok( req, real ) )
            {
                char *uri = vlc_path2uri( real, NULL );
                if( uri != NULL )
                {
                    playlist_AddExt( pl, uri, NULL, play && added == 0, 0,
                                     NULL, 0, true );
                    added++;
                    free( uri );
                }
            }
            free( real );
        }
        if( added == 0 )
        {
            *err = "nothing to add (missing file, or not in a shared folder)";
            return VLC_EGENERIC;
        }
    }
    else
        return VLC_EGENERIC;
    return VLC_SUCCESS;
}

void wlc_h_cmd( wlc_req_t *req )
{
    const char *err;
    if( wlc_player_cmd( req, wlc_jstr( req->json, "cmd" ), req->json, &err ) )
        wlc_error( req, 400, err );
    else
        wlc_h_status( req );
}

static void playlist_dump( struct vlc_memstream *ms, playlist_item_t *node,
                           const playlist_item_t *cur, bool *first, int depth )
{
    for( int i = 0; i < node->i_children; i++ )
    {
        playlist_item_t *it = node->pp_children[i];
        if( it->i_children >= 0 )
        {
            if( depth < 4 )
                playlist_dump( ms, it, cur, first, depth + 1 );
            continue;
        }
        char *name = input_item_GetTitleFbName( it->p_input );
        char *artist = input_item_GetArtist( it->p_input );
        if( !*first ) vlc_memstream_putc( ms, ',' );
        *first = false;
        vlc_memstream_printf( ms, "{\"id\":%d,\"current\":%s,\"duration\":%.1f,"
            "\"name\":", it->i_id, it == cur ? "true" : "false",
            input_item_GetDuration( it->p_input ) / (double)CLOCK_FREQ );
        wlc_json_str( ms, name ? name : "" );
        vlc_memstream_puts( ms, ",\"artist\":" );
        wlc_json_str( ms, artist ? artist : "" );
        vlc_memstream_putc( ms, '}' );
        free( name ); free( artist );
    }
}

void wlc_h_playlist( wlc_req_t *req )
{
    playlist_t *pl = req->sys->pl;
    struct vlc_memstream ms;
    bool first = true;
    vlc_memstream_open( &ms );
    vlc_memstream_puts( &ms, "{\"items\":[" );
    playlist_Lock( pl );
    playlist_dump( &ms, pl->p_playing, playlist_CurrentPlayingItem( pl ),
                   &first, 0 );
    playlist_Unlock( pl );
    vlc_memstream_puts( &ms, "]}" );
    wlc_send_json( req, 200, &ms );
}

/*****************************************************************************
 * Folders: browsing and the library
 *****************************************************************************/
static void entry_json( struct vlc_memstream *ms, const char *path,
                        const char *name, bool dir, char kind,
                        int64_t size, int64_t mtime )
{
    vlc_memstream_puts( ms, "{\"name\":" );
    wlc_json_str( ms, name );
    vlc_memstream_puts( ms, ",\"path\":" );
    wlc_json_str( ms, path );
    vlc_memstream_printf( ms, ",\"dir\":%s,\"kind\":\"%c\",\"size\":%"PRId64
        ",\"mtime\":%"PRId64"}", dir ? "true" : "false", kind ? kind : '-',
        size, mtime );
}

void wlc_h_browse( wlc_req_t *req )
{
    intf_sys_t *sys = req->sys;
    char *want = wlc_qparam( req, "path" );
    char *all = wlc_qparam( req, "all" );
    const bool show_all = all != NULL && all[0] == '1';
    struct vlc_memstream ms;
    free( all );

    if( want == NULL || want[0] == '\0' )
    {   /* starting points */
        free( want );
        bool first = true;
        vlc_memstream_open( &ms );
        vlc_memstream_puts( &ms, "{\"path\":\"\",\"parent\":null,\"entries\":[" );
        vlc_mutex_lock( &sys->lock );
        for( unsigned i = 0; i < sys->nroots; i++ )
        {
            if( !first ) vlc_memstream_putc( &ms, ',' );
            first = false;
            entry_json( &ms, sys->roots[i], sys->roots[i], true, 0, 0, 0 );
        }
        vlc_mutex_unlock( &sys->lock );
        if( req->local )
        {
            char *home = config_GetUserDir( VLC_HOME_DIR );
            static const char *const extra[] = { "/Volumes", "/media", "/mnt", "/" };
            if( home != NULL )
            {
                if( !first ) vlc_memstream_putc( &ms, ',' );
                first = false;
                entry_json( &ms, home, home, true, 0, 0, 0 );
                free( home );
            }
            for( size_t i = 0; i < ARRAY_SIZE(extra); i++ )
            {
                struct stat st;
                if( vlc_stat( extra[i], &st ) || !S_ISDIR( st.st_mode ) )
                    continue;
                if( !first ) vlc_memstream_putc( &ms, ',' );
                first = false;
                entry_json( &ms, extra[i], extra[i], true, 0, 0, 0 );
            }
        }
        vlc_memstream_puts( &ms, "]}" );
        wlc_send_json( req, 200, &ms );
        return;
    }

    char *real = wlc_realpath( want );
    free( want );
    DIR *dir = NULL;
    if( real == NULL || !wlc_path_ok( req, real )
     || (dir = vlc_opendir( real )) == NULL )
    {
        free( real );
        wlc_error( req, 404, "folder not found or not shared" );
        return;
    }

    vlc_memstream_open( &ms );
    vlc_memstream_puts( &ms, "{\"path\":" );
    wlc_json_str( &ms, real );
    vlc_memstream_puts( &ms, ",\"parent\":" );
    char *parent = strdup( real );
    char *slash = parent ? strrchr( parent, '/' ) : NULL;
    if( slash != NULL && strlen( real ) > 1 )
    {
        if( slash == parent ) slash[1] = '\0'; else *slash = '\0';
        if( req->local || wlc_path_shared( sys, parent ) )
            wlc_json_str( &ms, parent );
        else
            wlc_json_str( &ms, "" );
    }
    else
        vlc_memstream_puts( &ms, "null" );
    free( parent );
    vlc_memstream_puts( &ms, ",\"entries\":[" );

    bool first = true;
    unsigned count = 0;
    const char *name;
    while( (name = vlc_readdir( dir )) != NULL && count < 20000 )
    {
        if( name[0] == '.' )
            continue;
        char *full;
        if( asprintf( &full, "%s/%s", strcmp( real, "/" ) ? real : "", name ) < 0 )
            continue;
        struct stat st;
        if( !vlc_stat( full, &st ) )
        {
            const bool isdir = S_ISDIR( st.st_mode );
            const char kind = isdir ? 0 : wlc_kind( name );
            if( isdir || (S_ISREG( st.st_mode ) && (show_all || (kind && kind != 's'))) )
            {
                if( !first ) vlc_memstream_putc( &ms, ',' );
                first = false;
                entry_json( &ms, full, name, isdir, kind,
                            isdir ? 0 : (int64_t)st.st_size, (int64_t)st.st_mtime );
                count++;
            }
        }
        free( full );
    }
    closedir( dir );
    vlc_memstream_puts( &ms, "]}" );
    free( real );
    wlc_send_json( req, 200, &ms );
}

static void lib_free( intf_sys_t *sys )
{
    for( size_t i = 0; i < sys->nlib; i++ )
        free( sys->lib[i].path );
    free( sys->lib );
    sys->lib = NULL;
    sys->nlib = 0;
    sys->lib_valid = false;
    sys->lib_truncated = false;
}

static void lib_scan_dir( wlc_entry_t **list, size_t *n, size_t *cap,
                          const char *path, int depth, bool *truncated )
{
    DIR *dir = vlc_opendir( path );
    if( dir == NULL )
        return;
    const char *name;
    while( (name = vlc_readdir( dir )) != NULL )
    {
        if( name[0] == '.' )
            continue;
        if( *n >= WLC_MAX_LIB )
        {
            *truncated = true;
            break;
        }
        char *full;
        if( asprintf( &full, "%s/%s", path, name ) < 0 )
            continue;
        struct stat st;
        if( vlc_stat( full, &st ) )
        {
            free( full );
            continue;
        }
        if( S_ISDIR( st.st_mode ) )
        {
            if( depth < 16 )
                lib_scan_dir( list, n, cap, full, depth + 1, truncated );
            free( full );
            continue;
        }
        const char kind = wlc_kind( name );
        if( !S_ISREG( st.st_mode ) || kind == 0 || kind == 's' )
        {
            free( full );
            continue;
        }
        if( *n == *cap )
        {
            size_t ncap = *cap ? *cap * 2 : 512;
            wlc_entry_t *nl = realloc( *list, ncap * sizeof(**list) );
            if( nl == NULL )
            {
                free( full );
                break;
            }
            *list = nl;
            *cap = ncap;
        }
        (*list)[*n] = (wlc_entry_t){ .path = full, .size = st.st_size,
                                     .mtime = st.st_mtime, .kind = kind };
        (*n)++;
    }
    closedir( dir );
}

void wlc_h_library( wlc_req_t *req )
{
    intf_sys_t *sys = req->sys;
    char *rescan = wlc_qparam( req, "rescan" );
    const bool force = rescan != NULL && rescan[0] == '1';
    free( rescan );

    vlc_mutex_lock( &sys->lock );
    if( force || !sys->lib_valid )
    {
        /* Copy the folder list, scan without the lock, then swap in. */
        char *roots[WLC_MAX_ROOTS];
        unsigned nroots = sys->nroots;
        for( unsigned i = 0; i < nroots; i++ )
            roots[i] = strdup( sys->roots[i] );
        vlc_mutex_unlock( &sys->lock );

        wlc_entry_t *list = NULL;
        size_t n = 0, cap = 0;
        bool truncated = false;
        for( unsigned i = 0; i < nroots; i++ )
        {
            if( roots[i] != NULL )
                lib_scan_dir( &list, &n, &cap, roots[i], 0, &truncated );
            free( roots[i] );
        }

        vlc_mutex_lock( &sys->lock );
        lib_free( sys );
        sys->lib = list;
        sys->nlib = n;
        sys->lib_valid = true;
        sys->lib_truncated = truncated;
    }

    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    vlc_memstream_printf( &ms, "{\"truncated\":%s,\"items\":[",
                          sys->lib_truncated ? "true" : "false" );
    for( size_t i = 0; i < sys->nlib; i++ )
    {
        const wlc_entry_t *e = &sys->lib[i];
        if( i ) vlc_memstream_putc( &ms, ',' );
        vlc_memstream_puts( &ms, "{\"p\":" );
        wlc_json_str( &ms, e->path );
        vlc_memstream_printf( &ms, ",\"k\":\"%c\",\"s\":%"PRId64",\"m\":%"PRId64"}",
                              e->kind, e->size, e->mtime );
    }
    vlc_memstream_puts( &ms, "]}" );
    vlc_mutex_unlock( &sys->lock );
    wlc_send_json( req, 200, &ms );
}

void wlc_h_roots( wlc_req_t *req )
{
    intf_sys_t *sys = req->sys;
    const char *add = wlc_jstr( req->json, "add" );
    const char *rem = wlc_jstr( req->json, "remove" );
    const char *err = NULL;

    if( add != NULL )
    {
        char *real = wlc_realpath( add );
        struct stat st;
        if( real == NULL || vlc_stat( real, &st ) || !S_ISDIR( st.st_mode ) )
            err = "that folder does not exist";
        else if( !strcmp( real, "/" ) )
            err = "sharing the whole disk is not allowed; pick a folder";
        else
        {
            vlc_mutex_lock( &sys->lock );
            bool dup = false;
            for( unsigned i = 0; i < sys->nroots; i++ )
                dup |= !strcmp( sys->roots[i], real );
            if( dup )
                ;
            else if( sys->nroots >= WLC_MAX_ROOTS )
                err = "too many folders";
            else
            {
                sys->roots[sys->nroots++] = real;
                real = NULL;
                sys->lib_valid = false;
                wlc_save( sys );
            }
            vlc_mutex_unlock( &sys->lock );
        }
        free( real );
    }
    if( rem != NULL )
    {
        vlc_mutex_lock( &sys->lock );
        for( unsigned i = 0; i < sys->nroots; i++ )
            if( !strcmp( sys->roots[i], rem ) )
            {
                free( sys->roots[i] );
                memmove( &sys->roots[i], &sys->roots[i + 1],
                         (sys->nroots - i - 1) * sizeof(char *) );
                sys->nroots--;
                sys->lib_valid = false;
                wlc_save( sys );
                break;
            }
        vlc_mutex_unlock( &sys->lock );
    }
    if( err != NULL )
        wlc_error( req, 400, err );
    else
        wlc_ok( req );
}

/*****************************************************************************
 * Tags and cover art, read with the player's own parser
 *****************************************************************************/
static input_item_t *preparse( intf_sys_t *sys, const char *real, bool want_art )
{
    char *uri = vlc_path2uri( real, NULL );
    if( uri == NULL )
        return NULL;
    input_item_t *item = input_item_New( uri, NULL );
    free( uri );
    if( item == NULL )
        return NULL;

    libvlc_int_t *libvlc = sys->intf->obj.libvlc;
    if( libvlc_MetadataRequest( libvlc, item, META_REQUEST_OPTION_SCOPE_LOCAL,
                                4000, item ) )
        return item;
    for( int i = 0; i < 250 && !input_item_IsPreparsed( item )
                 && !atomic_load( &sys->stop ); i++ )
        msleep( 20000 );
    if( want_art )
    {
        libvlc_ArtRequest( libvlc, item, META_REQUEST_OPTION_SCOPE_LOCAL );
        for( int i = 0; i < 75 && !atomic_load( &sys->stop ); i++ )
        {
            char *art = input_item_GetArtURL( item );
            bool done = art != NULL && !strncmp( art, "file://", 7 );
            free( art );
            if( done || input_item_IsArtFetched( item ) )
                break;
            msleep( 20000 );
        }
    }
    return item;
}

static void meta_field( struct vlc_memstream *ms, const char *key, char *val )
{
    vlc_memstream_printf( ms, ",\"%s\":", key );
    wlc_json_str( ms, val ? val : "" );
    free( val );
}

void wlc_h_meta( wlc_req_t *req )
{
    char *p = wlc_qparam( req, "path" );
    char *real = wlc_realpath( p );
    free( p );
    if( real == NULL || !wlc_path_ok( req, real ) )
    {
        free( real );
        wlc_error( req, 404, "not found or not shared" );
        return;
    }
    input_item_t *item = preparse( req->sys, real, false );
    free( real );
    if( item == NULL )
    {
        wlc_error( req, 500, "could not read the file" );
        return;
    }
    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    vlc_memstream_printf( &ms, "{\"ok\":true,\"parsed\":%s,\"duration\":%.2f",
        input_item_IsPreparsed( item ) ? "true" : "false",
        input_item_GetDuration( item ) / (double)CLOCK_FREQ );
    meta_field( &ms, "title", input_item_GetTitle( item ) );
    meta_field( &ms, "artist", input_item_GetArtist( item ) );
    meta_field( &ms, "album", input_item_GetAlbum( item ) );
    meta_field( &ms, "albumartist", input_item_GetAlbumArtist( item ) );
    meta_field( &ms, "genre", input_item_GetGenre( item ) );
    meta_field( &ms, "year", input_item_GetDate( item ) );
    meta_field( &ms, "track", input_item_GetTrackNumber( item ) );
    meta_field( &ms, "disc", input_item_GetDiscNumber( item ) );
    meta_field( &ms, "show", input_item_GetShowName( item ) );
    meta_field( &ms, "season", input_item_GetSeason( item ) );
    meta_field( &ms, "episode", input_item_GetEpisode( item ) );
    vlc_memstream_putc( &ms, '}' );
    input_item_Release( item );
    wlc_send_json( req, 200, &ms );
}

void wlc_h_art( wlc_req_t *req )
{
    intf_sys_t *sys = req->sys;
    char *p = wlc_qparam( req, "path" );
    char *real = NULL, *art = NULL;

    if( p == NULL || p[0] == '\0' )
    {   /* what is playing now */
        input_thread_t *in = playlist_CurrentInput( sys->pl );
        if( in != NULL )
        {
            input_item_t *item = input_GetItem( in );
            if( item != NULL )
                art = input_item_GetArtURL( item );
            vlc_object_release( in );
        }
    }
    else if( (real = wlc_realpath( p )) != NULL && wlc_path_ok( req, real ) )
    {
        input_item_t *item = preparse( sys, real, true );
        if( item != NULL )
        {
            art = input_item_GetArtURL( item );
            input_item_Release( item );
        }
    }
    free( p );
    free( real );

    char *file = (art != NULL && !strncmp( art, "file://", 7 ))
               ? vlc_uri2path( art ) : NULL;
    free( art );
    if( file != NULL && wlc_kind( file ) == 'i' )
        wlc_serve_file( req, file );
    else
        wlc_error( req, 404, "no cover art" );
    free( file );
}

/*****************************************************************************
 * Bulk renamer: all-or-nothing, never overwrites, every batch can be undone
 *****************************************************************************/
#define WLC_KEEP_BATCHES 30

static void batch_clear( wlc_batch_t *b )
{
    for( size_t i = 0; i < b->n; i++ )
    {
        free( b->from[i] );
        free( b->to[i] );
    }
    free( b->from );
    free( b->to );
    memset( b, 0, sizeof(*b) );
}

static void rename_save( intf_sys_t *sys )   /* lock held */
{
    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    vlc_memstream_putc( &ms, '[' );
    for( size_t b = 0; b < sys->nbatches; b++ )
    {
        const wlc_batch_t *bt = &sys->batches[b];
        if( b ) vlc_memstream_putc( &ms, ',' );
        vlc_memstream_printf( &ms, "\n{\"id\":%d,\"when\":%"PRId64",\"undone\":%s,"
            "\"pairs\":[", bt->id, bt->when, bt->undone ? "true" : "false" );
        for( size_t i = 0; i < bt->n; i++ )
        {
            if( i ) vlc_memstream_putc( &ms, ',' );
            vlc_memstream_putc( &ms, '[' );
            wlc_json_str( &ms, bt->from[i] );
            vlc_memstream_putc( &ms, ',' );
            wlc_json_str( &ms, bt->to[i] );
            vlc_memstream_putc( &ms, ']' );
        }
        vlc_memstream_puts( &ms, "]}" );
    }
    vlc_memstream_puts( &ms, "\n]\n" );
    if( vlc_memstream_close( &ms ) )
        return;
    char *path = wlc_cfg_path( sys, "wlc-renames.json" );
    if( path != NULL )
        wlc_write_file( path, ms.ptr, ms.length );
    free( path );
    free( ms.ptr );
}

void wlc_rename_load( intf_sys_t *sys )
{
    char *path = wlc_cfg_path( sys, "wlc-renames.json" );
    size_t len = 0;
    char *data = path ? wlc_read_file( path, 32 * 1024 * 1024, &len ) : NULL;
    json_value *j = data ? json_parse( data ) : NULL;
    free( path );

    if( j != NULL && j->type == json_array )
    {
        sys->batches = calloc( j->u.array.length + 1, sizeof(wlc_batch_t) );
        for( unsigned b = 0; sys->batches && b < j->u.array.length; b++ )
        {
            const json_value *o = j->u.array.values[b];
            const json_value *pairs = wlc_jget( o, "pairs" );
            if( pairs == NULL || pairs->type != json_array )
                continue;
            wlc_batch_t *bt = &sys->batches[sys->nbatches];
            bt->id = (int)wlc_jnum( o, "id", 0 );
            bt->when = (int64_t)wlc_jnum( o, "when", 0 );
            bt->undone = wlc_jbool( o, "undone", false );
            bt->from = calloc( pairs->u.array.length + 1, sizeof(char *) );
            bt->to = calloc( pairs->u.array.length + 1, sizeof(char *) );
            if( bt->from == NULL || bt->to == NULL )
            {
                batch_clear( bt );
                continue;
            }
            for( unsigned i = 0; i < pairs->u.array.length; i++ )
            {
                const json_value *pr = pairs->u.array.values[i];
                if( pr->type != json_array || pr->u.array.length != 2
                 || pr->u.array.values[0]->type != json_string
                 || pr->u.array.values[1]->type != json_string )
                    continue;
                bt->from[bt->n] = strdup( pr->u.array.values[0]->u.string.ptr );
                bt->to[bt->n] = strdup( pr->u.array.values[1]->u.string.ptr );
                bt->n++;
            }
            if( bt->id >= sys->batch_seq )
                sys->batch_seq = bt->id + 1;
            sys->nbatches++;
        }
    }
    if( j != NULL )
        json_value_free( j );
    free( data );
}

static bool name_valid( const char *path )
{
    if( path == NULL || path[0] != '/' || strstr( path, "/../" ) != NULL )
        return false;
    const char *base = strrchr( path, '/' ) + 1;
    return base[0] != '\0' && strcmp( base, "." ) && strcmp( base, ".." )
        && strlen( base ) <= 255;
}

/* Runs one batch. On any problem nothing is left renamed.
 * Returns NULL on success or a message (static, or malloc'ed in *dyn). */
static const char *rename_run( size_t n, char **from, char **to, int tag,
                               char **dyn )
{
    struct stat *st = calloc( n ? n : 1, sizeof(*st) );
    char **tmp = calloc( n ? n : 1, sizeof(char *) );
    const char *err = NULL;
    size_t moved = 0, placed = 0;
    *dyn = NULL;
    if( st == NULL || tmp == NULL )
    {
        err = "out of memory";
        goto out;
    }

    /* 1. check everything before touching anything */
    for( size_t i = 0; i < n && err == NULL; i++ )
    {
        if( !name_valid( from[i] ) || !name_valid( to[i] ) )
            err = "invalid file name in the list";
        else if( vlc_lstat( from[i], &st[i] ) )
        {
            if( asprintf( dyn, "no longer there: %s", from[i] ) < 0 ) *dyn = NULL;
            err = "a file is no longer there";
        }
    }
    for( size_t i = 0; i < n && err == NULL; i++ )
    {
        for( size_t k = i + 1; k < n && err == NULL; k++ )
            if( !strcasecmp( to[i], to[k] ) )
            {
                if( asprintf( dyn, "two files would get the same name: %s",
                              to[i] ) < 0 ) *dyn = NULL;
                err = "two files would get the same name";
            }
        struct stat dst;
        if( err == NULL && !vlc_lstat( to[i], &dst ) )
        {   /* something has that name already; fine only if it is one of
             * the files being renamed in this batch */
            bool ours = false;
            for( size_t k = 0; k < n && !ours; k++ )
                ours = st[k].st_dev == dst.st_dev && st[k].st_ino == dst.st_ino;
            if( !ours )
            {
                if( asprintf( dyn, "would overwrite: %s", to[i] ) < 0 ) *dyn = NULL;
                err = "a rename would overwrite an existing file";
            }
        }
    }
    if( err != NULL )
        goto out;

    /* 2. move every file to a temporary name next to it */
    for( ; moved < n; moved++ )
    {
        const size_t i = moved;
        const char *slash = strrchr( from[i], '/' );
        if( asprintf( &tmp[i], "%.*s/.wlc-rename-%d-%zu.tmp",
                      (int)(slash - from[i]), from[i], tag, i ) < 0 )
        {
            tmp[i] = NULL;
            err = "out of memory";
            break;
        }
        struct stat x;
        if( !vlc_lstat( tmp[i], &x ) || vlc_rename( from[i], tmp[i] ) )
        {
            if( asprintf( dyn, "could not rename %s (%s)", from[i],
                          vlc_strerror_c( errno ) ) < 0 ) *dyn = NULL;
            err = "a file could not be renamed";
            break;
        }
    }
    /* 3. move them to their final names */
    if( err == NULL )
        for( ; placed < n; placed++ )
        {
            const size_t i = placed;
            struct stat x;
            if( !vlc_lstat( to[i], &x ) || vlc_rename( tmp[i], to[i] ) )
            {
                if( asprintf( dyn, "could not create %s (%s)", to[i],
                              vlc_strerror_c( errno ) ) < 0 ) *dyn = NULL;
                err = "a file could not be renamed";
                break;
            }
        }
    /* roll back on failure */
    if( err != NULL )
    {
        for( size_t i = 0; i < placed; i++ )
            vlc_rename( to[i], tmp[i] );
        for( size_t i = 0; i < moved; i++ )
            vlc_rename( tmp[i], from[i] );
    }
out:
    if( tmp != NULL )
        for( size_t i = 0; i < n; i++ )
            free( tmp[i] );
    free( tmp );
    free( st );
    return err;
}

void wlc_h_rename( wlc_req_t *req )
{
    intf_sys_t *sys = req->sys;
    const json_value *pairs = wlc_jget( req->json, "pairs" );
    if( pairs == NULL || pairs->type != json_array || pairs->u.array.length == 0 )
    {
        wlc_error( req, 400, "nothing to rename" );
        return;
    }
    const unsigned total = pairs->u.array.length;
    wlc_batch_t bt = { .when = (int64_t)time( NULL ) };
    bt.from = calloc( total, sizeof(char *) );
    bt.to = calloc( total, sizeof(char *) );
    if( bt.from == NULL || bt.to == NULL )
    {
        batch_clear( &bt );
        wlc_error( req, 500, "out of memory" );
        return;
    }
    for( unsigned i = 0; i < total; i++ )
    {
        const json_value *pr = pairs->u.array.values[i];
        const char *f = wlc_jstr( pr, "from" ), *t = wlc_jstr( pr, "to" );
        if( f == NULL || t == NULL )
        {
            batch_clear( &bt );
            wlc_error( req, 400, "each entry needs \"from\" and \"to\"" );
            return;
        }
        if( !strcmp( f, t ) )
            continue;
        bt.from[bt.n] = strdup( f );
        bt.to[bt.n] = strdup( t );
        bt.n++;
    }
    if( bt.n == 0 )
    {
        batch_clear( &bt );
        wlc_send( req, 200, "application/json", NULL,
                  "{\"ok\":true,\"renamed\":0}", 23 );
        return;
    }

    vlc_mutex_lock( &sys->lock );
    bt.id = sys->batch_seq++;
    char *dyn;
    const char *err = rename_run( bt.n, bt.from, bt.to, bt.id, &dyn );
    if( err == NULL )
    {
        if( sys->nbatches >= WLC_KEEP_BATCHES )
        {
            batch_clear( &sys->batches[0] );
            memmove( &sys->batches[0], &sys->batches[1],
                     (sys->nbatches - 1) * sizeof(wlc_batch_t) );
            sys->nbatches--;
        }
        wlc_batch_t *nb = realloc( sys->batches,
                                   (sys->nbatches + 1) * sizeof(wlc_batch_t) );
        if( nb != NULL )
        {
            sys->batches = nb;
            sys->batches[sys->nbatches++] = bt;
            rename_save( sys );
        }
        sys->lib_valid = false;
    }
    const int id = bt.id;
    const size_t count = bt.n;
    if( err != NULL )
        batch_clear( &bt );
    vlc_mutex_unlock( &sys->lock );

    if( err != NULL )
        wlc_error( req, 409, dyn ? dyn : err );
    else
    {
        char out[96];
        int n = snprintf( out, sizeof(out),
                          "{\"ok\":true,\"renamed\":%zu,\"batch\":%d}", count, id );
        wlc_send( req, 200, "application/json", NULL, out, n );
    }
    free( dyn );
}

void wlc_h_rename_undo( wlc_req_t *req )
{
    intf_sys_t *sys = req->sys;
    const int id = (int)wlc_jnum( req->json, "batch", -1 );
    const char *err = "no such rename batch";
    char *dyn = NULL;

    vlc_mutex_lock( &sys->lock );
    for( size_t b = 0; b < sys->nbatches; b++ )
    {
        wlc_batch_t *bt = &sys->batches[b];
        if( bt->id != id )
            continue;
        if( bt->undone )
            err = "already undone";
        else if( (err = rename_run( bt->n, bt->to, bt->from, 100000 + id,
                                    &dyn )) == NULL )
        {
            bt->undone = true;
            sys->lib_valid = false;
            rename_save( sys );
        }
        break;
    }
    vlc_mutex_unlock( &sys->lock );
    if( err != NULL )
        wlc_error( req, 409, dyn ? dyn : err );
    else
        wlc_ok( req );
    free( dyn );
}

void wlc_h_rename_history( wlc_req_t *req )
{
    intf_sys_t *sys = req->sys;
    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    vlc_memstream_puts( &ms, "{\"batches\":[" );
    vlc_mutex_lock( &sys->lock );
    for( size_t b = sys->nbatches; b-- > 0; )
    {
        const wlc_batch_t *bt = &sys->batches[b];
        if( b != sys->nbatches - 1 ) vlc_memstream_putc( &ms, ',' );
        vlc_memstream_printf( &ms, "{\"id\":%d,\"when\":%"PRId64",\"undone\":%s,"
            "\"count\":%zu,\"sample\":[", bt->id, bt->when,
            bt->undone ? "true" : "false", bt->n );
        for( size_t i = 0; i < bt->n && i < 5; i++ )
        {
            if( i ) vlc_memstream_putc( &ms, ',' );
            vlc_memstream_putc( &ms, '[' );
            wlc_json_str( &ms, bt->from[i] );
            vlc_memstream_putc( &ms, ',' );
            wlc_json_str( &ms, bt->to[i] );
            vlc_memstream_putc( &ms, ']' );
        }
        vlc_memstream_puts( &ms, "]}" );
    }
    vlc_mutex_unlock( &sys->lock );
    vlc_memstream_puts( &ms, "]}" );
    wlc_send_json( req, 200, &ms );
}

/*****************************************************************************
 * Power EQ
 *****************************************************************************/
static void eq_set_all( vlc_object_t *obj, intf_sys_t *sys, bool create )
{
    if( create )
    {
        var_Create( obj, "wlceq-graphic", VLC_VAR_STRING );
        var_Create( obj, "wlceq-parametric", VLC_VAR_STRING );
        var_Create( obj, "wlceq-preamp", VLC_VAR_FLOAT );
        var_Create( obj, "wlceq-balance", VLC_VAR_FLOAT );
        var_Create( obj, "wlceq-width", VLC_VAR_FLOAT );
        var_Create( obj, "wlceq-limiter", VLC_VAR_BOOL );
    }
    var_SetString( obj, "wlceq-graphic", sys->eq_graphic ? sys->eq_graphic : "" );
    var_SetString( obj, "wlceq-parametric", sys->eq_param ? sys->eq_param : "" );
    var_SetFloat( obj, "wlceq-preamp", sys->eq_preamp );
    var_SetFloat( obj, "wlceq-balance", sys->eq_balance );
    var_SetFloat( obj, "wlceq-width", sys->eq_width );
    var_SetBool( obj, "wlceq-limiter", sys->eq_limiter );
}

void wlc_eq_apply( intf_sys_t *sys )
{
    playlist_t *pl = sys->pl;
    /* On the playlist, so that every new audio output inherits them... */
    eq_set_all( VLC_OBJECT(pl), sys, true );
    /* ...and on the running output, so that changes are heard at once. */
    audio_output_t *aout = playlist_GetAout( pl );
    if( aout != NULL )
    {
        if( var_Type( aout, "wlceq-graphic" ) != 0 )
            eq_set_all( VLC_OBJECT(aout), sys, false );
        vlc_object_release( aout );
    }
    playlist_EnableAudioFilter( pl, "wlceq", sys->eq_on );
}

void wlc_h_eq_get( wlc_req_t *req )
{
    intf_sys_t *sys = req->sys;
    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    vlc_mutex_lock( &sys->lock );
    vlc_memstream_printf( &ms, "{\"on\":%s,\"limiter\":%s,\"preamp\":%.2f,"
        "\"balance\":%.3f,\"width\":%.3f,\"graphic\":",
        sys->eq_on ? "true" : "false", sys->eq_limiter ? "true" : "false",
        (double)sys->eq_preamp, (double)sys->eq_balance, (double)sys->eq_width );
    wlc_json_str( &ms, sys->eq_graphic );
    vlc_memstream_puts( &ms, ",\"parametric\":" );
    wlc_json_str( &ms, sys->eq_param );
    vlc_memstream_putc( &ms, '}' );
    vlc_mutex_unlock( &sys->lock );
    wlc_send_json( req, 200, &ms );
}

static bool eq_text_ok( const char *s )
{
    if( strlen( s ) > 1024 )
        return false;
    for( ; *s; s++ )
        if( !( (*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'z')
            || strchr( " .,;-", *s ) != NULL ) )
            return false;
    return true;
}

void wlc_h_eq_set( wlc_req_t *req )
{
    intf_sys_t *sys = req->sys;
    const json_value *j = req->json;
    const char *g = wlc_jstr( j, "graphic" ), *p = wlc_jstr( j, "parametric" );
    if( (g != NULL && !eq_text_ok( g )) || (p != NULL && !eq_text_ok( p )) )
    {
        wlc_error( req, 400, "invalid equalizer settings" );
        return;
    }
    vlc_mutex_lock( &sys->lock );
    sys->eq_on = wlc_jbool( j, "on", sys->eq_on );
    sys->eq_limiter = wlc_jbool( j, "limiter", sys->eq_limiter );
    sys->eq_preamp = wlc_jnum( j, "preamp", sys->eq_preamp );
    sys->eq_balance = wlc_jnum( j, "balance", sys->eq_balance );
    sys->eq_width = wlc_jnum( j, "width", sys->eq_width );
    if( g != NULL )
    {
        free( sys->eq_graphic );
        sys->eq_graphic = strdup( g );
    }
    if( p != NULL )
    {
        free( sys->eq_param );
        sys->eq_param = strdup( p );
    }
    wlc_eq_apply( sys );
    if( wlc_jbool( j, "save", true ) )
        wlc_save( sys );
    vlc_mutex_unlock( &sys->lock );
    wlc_h_eq_get( req );
}

void wlc_api_cleanup( intf_sys_t *sys )
{
    lib_free( sys );
    for( size_t b = 0; b < sys->nbatches; b++ )
        batch_clear( &sys->batches[b] );
    free( sys->batches );
    sys->batches = NULL;
    sys->nbatches = 0;
}
