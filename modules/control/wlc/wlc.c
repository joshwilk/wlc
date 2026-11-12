/*****************************************************************************
 * wlc.c: WLC Control Center - media center, power EQ, converter, bulk
 *        renamer, LAN server and WilkCast casting, served to a browser
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

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <spawn.h>

#include <vlc_plugin.h>
#include <vlc_fs.h>
#include <vlc_rand.h>

extern char **environ;

static int  Open ( vlc_object_t * );
static void Close( vlc_object_t * );

#define PORT_TEXT N_("Control Center port")
#define PORT_LONGTEXT N_( \
    "First TCP port to try for the WLC Control Center. If it is taken, the " \
    "next nine are tried." )
#define OPEN_TEXT N_("Open the Control Center at startup")
#define OPEN_LONGTEXT N_( \
    "Open the WLC Control Center in the default web browser when WLC starts." )

vlc_module_begin ()
    set_shortname( N_("WLC Control Center") )
    set_description( N_("WLC Control Center (media center, EQ, converter, renamer, LAN server, casting)") )
    set_category( CAT_INTERFACE )
    set_subcategory( SUBCAT_INTERFACE_CONTROL )
    set_capability( "interface", 0 )
    add_integer_with_range( "wlc-port", WLC_DEFAULT_PORT, 1024, 65525,
                            PORT_TEXT, PORT_LONGTEXT, true )
    add_bool( "wlc-open", false, OPEN_TEXT, OPEN_LONGTEXT, false )
    set_callbacks( Open, Close )
    add_shortcut( "wlc" )
vlc_module_end ()

/*****************************************************************************
 * Files in the user's configuration directory
 *****************************************************************************/
char *wlc_cfg_path( intf_sys_t *sys, const char *name )
{
    char *path;
    if( sys->cfg_dir == NULL
     || asprintf( &path, "%s/%s", sys->cfg_dir, name ) < 0 )
        return NULL;
    return path;
}

char *wlc_read_file( const char *path, size_t max, size_t *len )
{
    int fd = vlc_open( path, O_RDONLY );
    if( fd == -1 )
        return NULL;
    struct stat st;
    char *data = NULL;
    if( !fstat( fd, &st ) && S_ISREG( st.st_mode ) && (size_t)st.st_size <= max
     && (data = malloc( st.st_size + 1 )) != NULL )
    {
        size_t got = 0;
        while( got < (size_t)st.st_size )
        {
            ssize_t n = read( fd, data + got, st.st_size - got );
            if( n <= 0 )
                break;
            got += n;
        }
        data[got] = '\0';
        if( len != NULL )
            *len = got;
    }
    vlc_close( fd );
    return data;
}

/* Write to a temporary name first so a crash never leaves half a file. */
int wlc_write_file( const char *path, const void *data, size_t len )
{
    char *tmp;
    if( asprintf( &tmp, "%s.tmp", path ) < 0 )
        return -1;
    int ret = -1;
    int fd = vlc_open( tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600 );
    if( fd != -1 )
    {
        const char *p = data;
        size_t left = len;
        while( left > 0 )
        {
            ssize_t n = write( fd, p, left );
            if( n <= 0 )
                break;
            p += n;
            left -= n;
        }
        vlc_close( fd );
        if( left == 0 && !vlc_rename( tmp, path ) )
            ret = 0;
        else
            vlc_unlink( tmp );
    }
    free( tmp );
    return ret;
}

void wlc_save( intf_sys_t *sys )
{
    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    vlc_memstream_puts( &ms, "{\n \"id\": " );
    wlc_json_str( &ms, sys->id );
    vlc_memstream_puts( &ms, ",\n \"name\": " );
    wlc_json_str( &ms, sys->name );
    vlc_memstream_puts( &ms, ",\n \"pin\": " );
    wlc_json_str( &ms, sys->pin );
    vlc_memstream_printf( &ms, ",\n \"lan\": %s,\n \"castAccept\": \"%s\","
        "\n \"parallel\": %d,\n \"roots\": [",
        sys->lan ? "true" : "false", sys->cast_accept, sys->parallel );
    for( unsigned i = 0; i < sys->nroots; i++ )
    {
        if( i ) vlc_memstream_putc( &ms, ',' );
        wlc_json_str( &ms, sys->roots[i] );
    }
    vlc_memstream_printf( &ms, "],\n \"eq\": {\"on\": %s, \"limiter\": %s, "
        "\"preamp\": %.2f, \"balance\": %.3f, \"width\": %.3f, \"graphic\": ",
        sys->eq_on ? "true" : "false", sys->eq_limiter ? "true" : "false",
        (double)sys->eq_preamp, (double)sys->eq_balance, (double)sys->eq_width );
    wlc_json_str( &ms, sys->eq_graphic ? sys->eq_graphic : "" );
    vlc_memstream_puts( &ms, ", \"parametric\": " );
    wlc_json_str( &ms, sys->eq_param ? sys->eq_param : "" );
    vlc_memstream_puts( &ms, "}\n}\n" );
    if( vlc_memstream_close( &ms ) )
        return;
    char *path = wlc_cfg_path( sys, "wlc.json" );
    if( path != NULL && wlc_write_file( path, ms.ptr, ms.length ) )
        msg_Warn( sys->intf, "WLC: could not save %s", path );
    free( path );
    free( ms.ptr );
}

static void Load( intf_sys_t *sys )
{
    char *path = wlc_cfg_path( sys, "wlc.json" );
    size_t len = 0;
    char *data = path ? wlc_read_file( path, 1024 * 1024, &len ) : NULL;
    json_value *j = data ? json_parse( data ) : NULL;
    const char *s;

    if( (s = wlc_jstr( j, "id" )) != NULL && strlen( s ) >= 8 )
        strlcpy( sys->id, s, sizeof(sys->id) );
    if( (s = wlc_jstr( j, "name" )) != NULL && s[0] )
        strlcpy( sys->name, s, sizeof(sys->name) );
    if( (s = wlc_jstr( j, "pin" )) != NULL && strlen( s ) == 6 )
        strlcpy( sys->pin, s, sizeof(sys->pin) );
    if( (s = wlc_jstr( j, "castAccept" )) != NULL
     && (!strcmp( s, "pin" ) || !strcmp( s, "open" ) || !strcmp( s, "off" )) )
        strlcpy( sys->cast_accept, s, sizeof(sys->cast_accept) );
    sys->lan = wlc_jbool( j, "lan", true );
    sys->parallel = (int)wlc_jnum( j, "parallel", 1 );
    if( sys->parallel < 1 || sys->parallel > 8 )
        sys->parallel = 1;

    const json_value *roots = wlc_jget( j, "roots" );
    if( roots != NULL && roots->type == json_array )
        for( unsigned i = 0; i < roots->u.array.length
                          && sys->nroots < WLC_MAX_ROOTS; i++ )
        {
            const json_value *v = roots->u.array.values[i];
            if( v->type == json_string && v->u.string.ptr[0] == '/' )
                sys->roots[sys->nroots++] = strdup( v->u.string.ptr );
        }

    const json_value *eq = wlc_jget( j, "eq" );
    sys->eq_on = wlc_jbool( eq, "on", false );
    sys->eq_limiter = wlc_jbool( eq, "limiter", true );
    sys->eq_preamp = wlc_jnum( eq, "preamp", 0. );
    sys->eq_balance = wlc_jnum( eq, "balance", 0. );
    sys->eq_width = wlc_jnum( eq, "width", 1. );
    s = wlc_jstr( eq, "graphic" );
    sys->eq_graphic = strdup( s ? s : "" );
    s = wlc_jstr( eq, "parametric" );
    sys->eq_param = strdup( s ? s : "" );

    bool fresh = false;
    if( sys->id[0] == '\0' )
    {
        wlc_new_token( sys->id );
        fresh = true;
    }
    if( sys->pin[0] == '\0' )
    {
        uint32_t r;
        vlc_rand_bytes( &r, sizeof(r) );
        snprintf( sys->pin, sizeof(sys->pin), "%06u", (unsigned)(r % 1000000) );
        fresh = true;
    }
    if( sys->name[0] == '\0' )
    {
        char host[48] = "";
        gethostname( host, sizeof(host) - 1 );
        char *dot = strchr( host, '.' );
        if( dot != NULL ) *dot = '\0';
        snprintf( sys->name, sizeof(sys->name), "WLC on %s",
                  host[0] ? host : "this computer" );
    }
    if( sys->cast_accept[0] == '\0' )
        strcpy( sys->cast_accept, "pin" );
    if( fresh )
        wlc_save( sys );
    sys->first_run = fresh;

    if( j != NULL )
        json_value_free( j );
    free( data );
    free( path );
}

/* mkdir -p: on a first run the configuration folder may not exist yet */
static void MakeDirs( const char *path )
{
    char *copy = strdup( path );
    if( copy == NULL )
        return;
    for( char *p = copy + 1; *p; p++ )
        if( *p == '/' )
        {
            *p = '\0';
            vlc_mkdir( copy, 0700 );
            *p = '/';
        }
    vlc_mkdir( copy, 0700 );
    free( copy );
}

static void OpenBrowser( intf_thread_t *intf, const char *url )
{
#ifdef __APPLE__
    char *const argv[] = { (char *)"open", (char *)url, NULL };
#else
    char *const argv[] = { (char *)"xdg-open", (char *)url, NULL };
#endif
    pid_t pid;
    if( posix_spawnp( &pid, argv[0], NULL, NULL, argv, environ ) )
        msg_Warn( intf, "WLC: could not open a browser for %s", url );
}

/*****************************************************************************
 * Open / Close
 *****************************************************************************/
static int Open( vlc_object_t *obj )
{
    intf_thread_t *intf = (intf_thread_t *)obj;
    intf_sys_t *sys = calloc( 1, sizeof(*sys) );
    if( unlikely(sys == NULL) )
        return VLC_ENOMEM;

    sys->intf = intf;
    sys->pl = pl_Get( intf );
    sys->lfd = sys->ufd = -1;
    vlc_mutex_init( &sys->lock );
    atomic_init( &sys->stop, false );
    sys->cfg_dir = config_GetUserDir( VLC_CONFIG_DIR );
    if( sys->cfg_dir != NULL )
        MakeDirs( sys->cfg_dir );
    wlc_new_token( sys->media_key );
    Load( sys );
    wlc_rename_load( sys );

    intf->p_sys = sys;
    if( wlc_http_start( sys ) )
    {
        wlc_api_cleanup( sys );
    wlc_screens_free( sys );
        vlc_mutex_destroy( &sys->lock );
        free( sys->cfg_dir );
        free( sys );
        return VLC_EGENERIC;
    }
    wlc_jobs_start( sys );
    if( sys->lan )
        wlc_cast_start( sys );

    vlc_mutex_lock( &sys->lock );
    wlc_eq_apply( sys );
    vlc_mutex_unlock( &sys->lock );

    /* Let the native interfaces find us (menu item on the Mac). */
    char url[64];
    snprintf( url, sizeof(url), "http://127.0.0.1:%d/", sys->port );
    vlc_object_t *libvlc = VLC_OBJECT(intf->obj.libvlc);
    var_Create( libvlc, "wlc-url", VLC_VAR_STRING );
    var_SetString( libvlc, "wlc-url", url );

    msg_Info( intf, "WLC Control Center: %s (%s)", url,
              sys->lan ? "LAN access on, PIN protected" : "this computer only" );
    bool open_now = var_InheritBool( intf, "wlc-open" );
#ifdef __APPLE__
    /* show people where the new tools live the first time WLC runs */
    open_now |= sys->first_run;
#endif
    if( open_now )
        OpenBrowser( intf, url );
    return VLC_SUCCESS;
}

static void Close( vlc_object_t *obj )
{
    intf_thread_t *intf = (intf_thread_t *)obj;
    intf_sys_t *sys = intf->p_sys;

    atomic_store( &sys->stop, true );
    wlc_cast_stop( sys );
    wlc_jobs_stop( sys );
    wlc_http_stop( sys );
    wlc_api_cleanup( sys );

    for( unsigned i = 0; i < sys->nroots; i++ )
        free( sys->roots[i] );
    for( unsigned i = 0; i < WLC_MAX_GRANTS; i++ )
        free( sys->grants[i] );
    free( sys->eq_graphic );
    free( sys->eq_param );
    free( sys->cfg_dir );
    vlc_mutex_destroy( &sys->lock );
    free( sys );
}
