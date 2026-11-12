/*****************************************************************************
 * wlc_http.c: WLC Control Center - small HTTP server, access control, routing
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
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <vlc_fs.h>
#include <vlc_rand.h>
#include <vlc_url.h>
#include <vlc_strings.h>

#include "wlc_ui.h"

#ifndef MSG_NOSIGNAL
# define MSG_NOSIGNAL 0
#endif

/*****************************************************************************
 * Small helpers
 *****************************************************************************/
void wlc_json_str( struct vlc_memstream *ms, const char *s )
{
    vlc_memstream_putc( ms, '"' );
    if( s != NULL )
        for( const unsigned char *p = (const unsigned char *)s; *p; p++ )
        {
            switch( *p )
            {
                case '"':  vlc_memstream_puts( ms, "\\\"" ); break;
                case '\\': vlc_memstream_puts( ms, "\\\\" ); break;
                case '\n': vlc_memstream_puts( ms, "\\n" ); break;
                case '\r': vlc_memstream_puts( ms, "\\r" ); break;
                case '\t': vlc_memstream_puts( ms, "\\t" ); break;
                default:
                    if( *p < 0x20 )
                        vlc_memstream_printf( ms, "\\u%04x", *p );
                    else
                        vlc_memstream_putc( ms, *p );
            }
        }
    vlc_memstream_putc( ms, '"' );
}

const json_value *wlc_jget( const json_value *obj, const char *key )
{
    if( obj == NULL || obj->type != json_object )
        return NULL;
    for( unsigned i = 0; i < obj->u.object.length; i++ )
        if( !strcmp( obj->u.object.values[i].name, key ) )
            return obj->u.object.values[i].value;
    return NULL;
}

const char *wlc_jstr( const json_value *obj, const char *key )
{
    const json_value *v = wlc_jget( obj, key );
    return (v != NULL && v->type == json_string) ? v->u.string.ptr : NULL;
}

double wlc_jnum( const json_value *obj, const char *key, double def )
{
    const json_value *v = wlc_jget( obj, key );
    if( v == NULL ) return def;
    if( v->type == json_integer ) return (double)v->u.integer;
    if( v->type == json_double ) return v->u.dbl;
    return def;
}

bool wlc_jbool( const json_value *obj, const char *key, bool def )
{
    const json_value *v = wlc_jget( obj, key );
    if( v == NULL ) return def;
    if( v->type == json_boolean ) return v->u.boolean;
    if( v->type == json_integer ) return v->u.integer != 0;
    return def;
}

char *wlc_qparam( wlc_req_t *req, const char *name )
{
    if( req->query == NULL )
        return NULL;
    const size_t nlen = strlen( name );
    for( const char *p = req->query; p != NULL && *p; )
    {
        const char *amp = strchr( p, '&' );
        size_t len = amp ? (size_t)(amp - p) : strlen( p );
        if( len > nlen && !strncmp( p, name, nlen ) && p[nlen] == '=' )
        {
            char *raw = strndup( p + nlen + 1, len - nlen - 1 );
            if( raw == NULL )
                return NULL;
            for( char *c = raw; *c; c++ )
                if( *c == '+' ) *c = ' ';
            vlc_uri_decode( raw );
            return raw;
        }
        p = amp ? amp + 1 : NULL;
    }
    return NULL;
}

void wlc_new_token( char out[33] )
{
    unsigned char raw[16];
    vlc_rand_bytes( raw, sizeof(raw) );
    for( unsigned i = 0; i < 16; i++ )
        sprintf( out + 2 * i, "%02x", raw[i] );
}

char *wlc_realpath( const char *path )
{
    if( path == NULL || path[0] != '/' )
        return NULL;
    char buf[PATH_MAX];
    if( realpath( path, buf ) == NULL )
        return NULL;
    return strdup( buf );
}

static bool under( const char *path, const char *root )
{
    size_t len = strlen( root );
    if( len == 1 && root[0] == '/' )
        return true;
    return !strncmp( path, root, len ) && (path[len] == '/' || path[len] == '\0');
}

/* Is this (already resolved) path inside a library folder, or a file that
 * was explicitly cast? */
bool wlc_path_shared( intf_sys_t *sys, const char *real )
{
    bool ok = false;
    vlc_mutex_lock( &sys->lock );
    for( unsigned i = 0; i < sys->nroots && !ok; i++ )
        ok = under( real, sys->roots[i] );
    for( unsigned i = 0; i < WLC_MAX_GRANTS && !ok; i++ )
        ok = sys->grants[i] != NULL && !strcmp( real, sys->grants[i] );
    vlc_mutex_unlock( &sys->lock );
    return ok;
}

bool wlc_path_ok( wlc_req_t *req, const char *real )
{
    return req->local || wlc_path_shared( req->sys, real );
}

static const struct { const char *ext; char kind; const char *mime; } exts[] = {
    { "mp4", 'v', "video/mp4" }, { "m4v", 'v', "video/mp4" },
    { "mkv", 'v', "video/x-matroska" }, { "webm", 'v', "video/webm" },
    { "avi", 'v', "video/x-msvideo" }, { "mov", 'v', "video/quicktime" },
    { "wmv", 'v', "video/x-ms-wmv" }, { "flv", 'v', "video/x-flv" },
    { "mpg", 'v', "video/mpeg" }, { "mpeg", 'v', "video/mpeg" },
    { "ts", 'v', "video/mp2t" }, { "m2ts", 'v', "video/mp2t" },
    { "mts", 'v', "video/mp2t" }, { "vob", 'v', "video/mpeg" },
    { "ogv", 'v', "video/ogg" }, { "3gp", 'v', "video/3gpp" },
    { "divx", 'v', "video/x-msvideo" }, { "asf", 'v', "video/x-ms-asf" },
    { "rmvb", 'v', "application/vnd.rn-realmedia-vbr" },
    { "amv", 'v', "video/x-amv" },
    { "mp3", 'a', "audio/mpeg" }, { "m4a", 'a', "audio/mp4" },
    { "aac", 'a', "audio/aac" }, { "flac", 'a', "audio/flac" },
    { "ogg", 'a', "audio/ogg" }, { "oga", 'a', "audio/ogg" },
    { "opus", 'a', "audio/ogg" }, { "wav", 'a', "audio/wav" },
    { "aif", 'a', "audio/aiff" }, { "aiff", 'a', "audio/aiff" },
    { "wma", 'a', "audio/x-ms-wma" }, { "ape", 'a', "audio/x-ape" },
    { "alac", 'a', "audio/mp4" }, { "mka", 'a', "audio/x-matroska" },
    { "ac3", 'a', "audio/ac3" }, { "dts", 'a', "audio/vnd.dts" },
    { "mid", 'a', "audio/midi" }, { "m4b", 'a', "audio/mp4" },
    { "jpg", 'i', "image/jpeg" }, { "jpeg", 'i', "image/jpeg" },
    { "png", 'i', "image/png" }, { "gif", 'i', "image/gif" },
    { "webp", 'i', "image/webp" }, { "bmp", 'i', "image/bmp" },
    { "srt", 's', "text/plain; charset=utf-8" },
    { "vtt", 's', "text/vtt; charset=utf-8" },
};

static int ext_lookup( const char *name )
{
    const char *dot = strrchr( name, '.' );
    if( dot == NULL || dot[1] == '\0' )
        return -1;
    for( size_t i = 0; i < ARRAY_SIZE(exts); i++ )
        if( !strcasecmp( dot + 1, exts[i].ext ) )
            return (int)i;
    return -1;
}

char wlc_kind( const char *name )
{
    int i = ext_lookup( name );
    return i < 0 ? 0 : exts[i].kind;
}

const char *wlc_mime( const char *name )
{
    int i = ext_lookup( name );
    return i < 0 ? "application/octet-stream" : exts[i].mime;
}

void wlc_local_addrs( struct vlc_memstream *ms )
{
    struct ifaddrs *list = NULL;
    bool first = true;

    vlc_memstream_putc( ms, '[' );
    if( getifaddrs( &list ) == 0 )
    {
        for( struct ifaddrs *a = list; a != NULL; a = a->ifa_next )
        {
            if( a->ifa_addr == NULL || a->ifa_addr->sa_family != AF_INET
             || !(a->ifa_flags & IFF_UP) || (a->ifa_flags & IFF_LOOPBACK) )
                continue;
            char buf[INET_ADDRSTRLEN];
            const struct sockaddr_in *in = (const struct sockaddr_in *)a->ifa_addr;
            if( inet_ntop( AF_INET, &in->sin_addr, buf, sizeof(buf) ) == NULL )
                continue;
            if( !first ) vlc_memstream_putc( ms, ',' );
            first = false;
            wlc_json_str( ms, buf );
        }
        freeifaddrs( list );
    }
    vlc_memstream_putc( ms, ']' );
}

/*****************************************************************************
 * Responses
 *****************************************************************************/
static bool send_all( intf_sys_t *sys, int fd, const void *data, size_t len )
{
    const char *p = data;
    while( len > 0 )
    {
        if( atomic_load( &sys->stop ) )
            return false;
        ssize_t n = send( fd, p, len, MSG_NOSIGNAL );
        if( n < 0 )
        {
            if( errno == EINTR )
                continue;
            return false;
        }
        p += n;
        len -= n;
    }
    return true;
}

static const char *reason( int code )
{
    switch( code )
    {
        case 200: return "OK";
        case 206: return "Partial Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 409: return "Conflict";
        case 416: return "Range Not Satisfiable";
        case 429: return "Too Many Requests";
        default:  return "Error";
    }
}

void wlc_send( wlc_req_t *req, int code, const char *ctype, const char *extra,
               const void *body, size_t len )
{
    char head[768];
    int n = snprintf( head, sizeof(head),
        "HTTP/1.1 %d %s\r\n"
        "Server: WLC/" WLC_VERSION "\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Connection: close\r\n"
        "%s\r\n", code, reason( code ), ctype, len, extra ? extra : "" );
    if( n < 0 || (size_t)n >= sizeof(head) )
        return;
    if( send_all( req->sys, req->fd, head, n ) && len > 0
     && strcmp( req->method, "HEAD" ) )
        send_all( req->sys, req->fd, body, len );
}

void wlc_send_json( wlc_req_t *req, int code, struct vlc_memstream *ms )
{
    if( vlc_memstream_close( ms ) )
    {
        wlc_send( req, 500, "application/json", NULL, "{}", 2 );
        return;
    }
    wlc_send( req, code, "application/json; charset=utf-8", NULL,
              ms->ptr, ms->length );
    free( ms->ptr );
}

void wlc_error( wlc_req_t *req, int code, const char *msg )
{
    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    vlc_memstream_puts( &ms, "{\"ok\":false,\"error\":" );
    wlc_json_str( &ms, msg );
    vlc_memstream_putc( &ms, '}' );
    wlc_send_json( req, code, &ms );
}

void wlc_ok( wlc_req_t *req )
{
    wlc_send( req, 200, "application/json", NULL, "{\"ok\":true}", 11 );
}

/*****************************************************************************
 * File streaming with byte ranges (what makes seeking work for cast
 * receivers and browsers)
 *****************************************************************************/
void wlc_serve_file( wlc_req_t *req, const char *real )
{
    struct stat st;
    int fd = vlc_open( real, O_RDONLY );
    if( fd == -1 )
    {
        wlc_error( req, 404, "file not found" );
        return;
    }
    if( fstat( fd, &st ) || !S_ISREG( st.st_mode ) )
    {
        vlc_close( fd );
        wlc_error( req, 404, "not a file" );
        return;
    }

    int64_t size = st.st_size, start = 0, end = size - 1;
    int code = 200;
    if( req->range[0] != '\0' && size > 0 )
    {
        const char *r = req->range;
        if( !strncmp( r, "bytes=", 6 ) )
        {
            r += 6;
            char *e;
            if( *r == '-' )
            {   /* last N bytes */
                int64_t n = strtoll( r + 1, &e, 10 );
                if( n > 0 )
                {
                    start = n >= size ? 0 : size - n;
                    code = 206;
                }
            }
            else
            {
                int64_t a = strtoll( r, &e, 10 );
                if( e != r && *e == '-' )
                {
                    int64_t b = e[1] ? strtoll( e + 1, NULL, 10 ) : size - 1;
                    if( a >= size || b < a )
                    {
                        char extra[80];
                        snprintf( extra, sizeof(extra),
                                  "Content-Range: bytes */%"PRId64"\r\n", size );
                        vlc_close( fd );
                        wlc_send( req, 416, "text/plain", extra, "", 0 );
                        return;
                    }
                    start = a;
                    end = b >= size ? size - 1 : b;
                    code = 206;
                }
            }
        }
    }
    const int64_t count = size > 0 ? end - start + 1 : 0;

    char head[640];
    int n = snprintf( head, sizeof(head),
        "HTTP/1.1 %d %s\r\n"
        "Server: WLC/" WLC_VERSION "\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %"PRId64"\r\n"
        "Accept-Ranges: bytes\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Connection: close\r\n",
        code, reason( code ), wlc_mime( real ), count );
    if( code == 206 )
        n += snprintf( head + n, sizeof(head) - n,
            "Content-Range: bytes %"PRId64"-%"PRId64"/%"PRId64"\r\n",
            start, end, size );
    n += snprintf( head + n, sizeof(head) - n, "\r\n" );

    if( send_all( req->sys, req->fd, head, n ) && count > 0
     && strcmp( req->method, "HEAD" )
     && lseek( fd, start, SEEK_SET ) == start )
    {
        char *buf = malloc( 65536 );
        int64_t left = count;
        while( buf != NULL && left > 0 )
        {
            ssize_t got = read( fd, buf, left > 65536 ? 65536 : (size_t)left );
            if( got <= 0 || !send_all( req->sys, req->fd, buf, got ) )
                break;
            left -= got;
        }
        free( buf );
    }
    vlc_close( fd );
}

/*****************************************************************************
 * Tiny HTTP client, used to talk to other WLC instances when casting
 *****************************************************************************/
int wlc_http_request( intf_sys_t *sys, const char *host, int port,
                      const char *method, const char *path, const char *pin,
                      const char *body, char **resp, char my_ip[64] )
{
    *resp = NULL;
    int fd = net_ConnectTCP( sys->intf, host, port );
    if( fd == -1 )
        return -1;

    /* the core hands out non-blocking sockets; this small client wants
     * plain blocking calls with a timeout */
    int fl = fcntl( fd, F_GETFL );
    if( fl != -1 )
        fcntl( fd, F_SETFL, fl & ~O_NONBLOCK );
    struct timeval tv = { .tv_sec = 6 };
    setsockopt( fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv) );
    setsockopt( fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv) );

    if( my_ip != NULL )
    {
        struct sockaddr_in me;
        socklen_t len = sizeof(me);
        my_ip[0] = '\0';
        if( !getsockname( fd, (struct sockaddr *)&me, &len )
         && me.sin_family == AF_INET )
            inet_ntop( AF_INET, &me.sin_addr, my_ip, 64 );
    }

    char *msg;
    size_t blen = body ? strlen( body ) : 0;
    int n = asprintf( &msg,
        "%s %s HTTP/1.1\r\nHost: %s:%d\r\nUser-Agent: WLC/" WLC_VERSION "\r\n"
        "Content-Type: application/json\r\nContent-Length: %zu\r\n"
        "X-WilkCast-Pin: %s\r\nX-WilkCast-From: %s\r\nConnection: close\r\n\r\n%s",
        method, path, host, port, blen, pin ? pin : "", sys->id,
        body ? body : "" );
    int status = -1;
    if( n >= 0 )
    {
        if( send_all( sys, fd, msg, n ) )
        {
            size_t cap = 65536, used = 0;
            char *buf = malloc( cap + 1 );
            while( buf != NULL && used < cap )
            {
                ssize_t got = recv( fd, buf + used, cap - used, 0 );
                if( got <= 0 )
                    break;
                used += got;
            }
            if( buf != NULL )
            {
                buf[used] = '\0';
                if( used > 12 && !strncmp( buf, "HTTP/1.", 7 ) )
                    status = atoi( buf + 9 );
                const char *b = strstr( buf, "\r\n\r\n" );
                if( b != NULL )
                    *resp = strdup( b + 4 );
                free( buf );
            }
        }
        free( msg );
    }
    net_Close( fd );
    return status;
}

/*****************************************************************************
 * Access control
 *****************************************************************************/
static bool session_valid( intf_sys_t *sys, const char *cookie )
{
    const char *p = strstr( cookie, "wlc_s=" );
    if( p == NULL )
        return false;
    p += 6;
    if( strlen( p ) < 32 )
        return false;
    bool ok = false;
    vlc_mutex_lock( &sys->lock );
    for( unsigned i = 0; i < WLC_MAX_SESS && !ok; i++ )
        ok = sys->sessions[i][0] != '\0' && !strncmp( sys->sessions[i], p, 32 );
    vlc_mutex_unlock( &sys->lock );
    return ok;
}

/* The Host header must be an address literal or "localhost": a page on
 * some website must not be able to reach us through DNS rebinding. */
static bool host_sane( const char *host )
{
    if( host[0] == '\0' )
        return false;
    if( !strncmp( host, "localhost", 9 ) && (host[9] == '\0' || host[9] == ':') )
        return true;
    if( !strncmp( host, "[::1]", 5 ) )
        return true;
    for( const char *p = host; *p; p++ )
        if( !(*p >= '0' && *p <= '9') && *p != '.' && *p != ':' )
            return false;
    return true;
}

static bool origin_sane( const wlc_req_t *req )
{
    if( req->origin[0] == '\0' )
        return true;
    return !strncmp( req->origin, "http://", 7 )
        && !strcmp( req->origin + 7, req->host );
}

static bool pin_check( wlc_req_t *req, const char *pin )
{
    intf_sys_t *sys = req->sys;
    bool ok = false;
    vlc_mutex_lock( &sys->lock );
    if( sys->fail_until > mdate() )
    {
        vlc_mutex_unlock( &sys->lock );
        return false;
    }
    if( pin != NULL && sys->pin[0] != '\0' && strlen( pin ) == strlen( sys->pin ) )
    {
        unsigned char diff = 0;   /* constant-time compare */
        for( size_t i = 0; sys->pin[i]; i++ )
            diff |= (unsigned char)(pin[i] ^ sys->pin[i]);
        ok = diff == 0;
    }
    if( ok )
        sys->fail_count = 0;
    else if( ++sys->fail_count >= 5 )
    {
        sys->fail_count = 0;
        sys->fail_until = mdate() + INT64_C(60) * CLOCK_FREQ;
        msg_Warn( sys->intf, "WLC: too many wrong PINs, locked for 60 s" );
    }
    vlc_mutex_unlock( &sys->lock );
    return ok;
}

/*****************************************************************************
 * Handlers that belong to the server itself
 *****************************************************************************/
static void h_info( wlc_req_t *req )
{
    intf_sys_t *sys = req->sys;
    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    vlc_mutex_lock( &sys->lock );
    vlc_memstream_puts( &ms, "{\"app\":\"WLC\",\"version\":\"" WLC_VERSION "\",\"name\":" );
    wlc_json_str( &ms, sys->name );
    vlc_memstream_puts( &ms, ",\"id\":" );
    wlc_json_str( &ms, sys->id );
    vlc_memstream_printf( &ms, ",\"port\":%d,\"local\":%s,\"authed\":%s,"
        "\"lan\":%s,\"castAccept\":\"%s\",\"parallel\":%d",
        sys->port, req->local ? "true" : "false",
        req->authed ? "true" : "false", sys->lan ? "true" : "false",
        sys->cast_accept, sys->parallel );
    if( req->local )
    {
        vlc_memstream_puts( &ms, ",\"pin\":" );
        wlc_json_str( &ms, sys->pin );
        vlc_memstream_puts( &ms, ",\"addrs\":" );
        wlc_local_addrs( &ms );
    }
    if( req->authed )
    {
        vlc_memstream_puts( &ms, ",\"roots\":[" );
        for( unsigned i = 0; i < sys->nroots; i++ )
        {
            if( i ) vlc_memstream_putc( &ms, ',' );
            wlc_json_str( &ms, sys->roots[i] );
        }
        vlc_memstream_putc( &ms, ']' );
    }
    vlc_memstream_putc( &ms, '}' );
    vlc_mutex_unlock( &sys->lock );
    wlc_send_json( req, 200, &ms );
}

static void h_login( wlc_req_t *req )
{
    intf_sys_t *sys = req->sys;
    if( !pin_check( req, wlc_jstr( req->json, "pin" ) ) )
    {
        wlc_error( req, 401, "wrong PIN, or locked after too many tries" );
        return;
    }
    char token[33], extra[160];
    wlc_new_token( token );
    vlc_mutex_lock( &sys->lock );
    memcpy( sys->sessions[sys->sess_next], token, 33 );
    sys->sess_next = (sys->sess_next + 1) % WLC_MAX_SESS;
    vlc_mutex_unlock( &sys->lock );
    snprintf( extra, sizeof(extra),
        "Set-Cookie: wlc_s=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=2592000\r\n",
        token );
    wlc_send( req, 200, "application/json", extra, "{\"ok\":true}", 11 );
}

static void h_settings( wlc_req_t *req )
{
    intf_sys_t *sys = req->sys;
    const json_value *j = req->json;
    vlc_mutex_lock( &sys->lock );
    const char *s = wlc_jstr( j, "name" );
    if( s != NULL && s[0] != '\0' )
        strlcpy( sys->name, s, sizeof(sys->name) );
    s = wlc_jstr( j, "castAccept" );
    if( s != NULL && (!strcmp( s, "pin" ) || !strcmp( s, "open" )
                   || !strcmp( s, "off" )) )
        strlcpy( sys->cast_accept, s, sizeof(sys->cast_accept) );
    if( wlc_jget( j, "lan" ) != NULL )
        sys->lan = wlc_jbool( j, "lan", sys->lan );
    int par = (int)wlc_jnum( j, "parallel", sys->parallel );
    if( par >= 1 && par <= 8 )
        sys->parallel = par;
    if( wlc_jbool( j, "newPin", false ) )
    {
        uint32_t r;
        vlc_rand_bytes( &r, sizeof(r) );
        snprintf( sys->pin, sizeof(sys->pin), "%06u", (unsigned)(r % 1000000) );
        memset( sys->sessions, 0, sizeof(sys->sessions) );
    }
    wlc_save( sys );
    vlc_mutex_unlock( &sys->lock );
    h_info( req );
}

static void h_ui_get( wlc_req_t *req )
{
    char *path = wlc_cfg_path( req->sys, "wlc-ui.json" );
    size_t len = 0;
    char *data = path ? wlc_read_file( path, 4 * 1024 * 1024, &len ) : NULL;
    if( data != NULL && len > 0 )
        wlc_send( req, 200, "application/json; charset=utf-8", NULL, data, len );
    else
        wlc_send( req, 200, "application/json", NULL, "{}", 2 );
    free( data );
    free( path );
}

static void h_ui_put( wlc_req_t *req )
{
    if( req->json == NULL || req->json->type != json_object )
    {
        wlc_error( req, 400, "expected a JSON object" );
        return;
    }
    char *path = wlc_cfg_path( req->sys, "wlc-ui.json" );
    if( path != NULL && !wlc_write_file( path, req->body, req->body_len ) )
        wlc_ok( req );
    else
        wlc_error( req, 500, "could not save" );
    free( path );
}

/*****************************************************************************
 * Routing
 *****************************************************************************/
#define IS(m, p) (!strcmp( req->method, m ) && !strcmp( req->path, p ))

static void route( wlc_req_t *req )
{
    const bool get = !strcmp( req->method, "GET" ) || !strcmp( req->method, "HEAD" );

    if( !host_sane( req->host ) || !origin_sane( req ) )
    {
        wlc_error( req, 403, "request did not come from the WLC page" );
        return;
    }

    /* --- no login needed ------------------------------------------------ */
    if( get && (!strcmp( req->path, "/" ) || !strcmp( req->path, "/index.html" )) )
    {
        wlc_send( req, 200, "text/html; charset=utf-8",
            "Content-Security-Policy: default-src 'self' 'unsafe-inline' data: blob:; "
            "frame-ancestors 'none'\r\n", wlc_ui_html, wlc_ui_html_len );
        return;
    }
    if( get && !strcmp( req->path, "/screen" ) )
    {   /* WilkCast screen: turns any browser into a cast receiver */
        wlc_send( req, 200, "text/html; charset=utf-8",
            "Content-Security-Policy: default-src 'self' 'unsafe-inline' data: blob:; "
            "media-src * data: blob:; img-src * data: blob:; frame-ancestors 'none'\r\n",
            wlc_screen_html, wlc_screen_html_len );
        return;
    }
    if( get && !strcmp( req->path, "/api/info" ) ) { h_info( req ); return; }
    if( IS( "POST", "/api/login" ) ) { h_login( req ); return; }

    if( !strncmp( req->path, "/cast/v1/", 9 ) )
    {
        intf_sys_t *sys = req->sys;
        const char *what = req->path + 9;
        /* screens hosted here live under /cast/v1/s/<id>/ */
        const char *op = what;
        if( !strncmp( what, "s/", 2 ) )
        {
            const char *slash = strchr( what + 2, '/' );
            op = slash ? slash + 1 : "";
        }
        if( !strcmp( op, "info" ) )
        {
            wlc_h_castv1( req, what );
            return;
        }
        vlc_mutex_lock( &sys->lock );
        const bool off = !strcmp( sys->cast_accept, "off" );
        const bool open = !strcmp( sys->cast_accept, "open" );
        vlc_mutex_unlock( &sys->lock );
        if( off && !req->local )
        {
            wlc_error( req, 403, "this WLC does not accept casts" );
            return;
        }
        if( !req->authed && !open && !pin_check( req, req->pin ) )
        {
            wlc_error( req, 401, "PIN required" );
            return;
        }
        wlc_h_castv1( req, what );
        return;
    }

    if( get && !strncmp( req->path, "/media/", 7 ) )
    {   /* /media/<key>/<absolute path> : what cast receivers fetch */
        intf_sys_t *sys = req->sys;
        const char *rest = req->path + 7;
        if( strlen( rest ) < 34 || strncmp( rest, sys->media_key, 32 )
         || rest[32] != '/' )
        {
            wlc_error( req, 403, "bad media key" );
            return;
        }
        char *real = wlc_realpath( rest + 32 );
        if( real == NULL || !wlc_path_shared( sys, real ) )
            wlc_error( req, 404, "not shared" );
        else
            wlc_serve_file( req, real );
        free( real );
        return;
    }

    /* --- everything below needs this computer or a PIN session ----------- */
    if( !req->authed )
    {
        wlc_error( req, 401, "login required" );
        return;
    }

    if( get && !strcmp( req->path, "/api/status" ) )   { wlc_h_status( req ); return; }
    if( IS( "POST", "/api/cmd" ) )                     { wlc_h_cmd( req ); return; }
    if( get && !strcmp( req->path, "/api/playlist" ) ) { wlc_h_playlist( req ); return; }
    if( get && !strcmp( req->path, "/api/browse" ) )   { wlc_h_browse( req ); return; }
    if( get && !strcmp( req->path, "/api/library" ) )  { wlc_h_library( req ); return; }
    if( get && !strcmp( req->path, "/api/meta" ) )     { wlc_h_meta( req ); return; }
    if( get && !strcmp( req->path, "/api/art" ) )      { wlc_h_art( req ); return; }
    if( get && !strcmp( req->path, "/api/eq" ) )       { wlc_h_eq_get( req ); return; }
    if( IS( "POST", "/api/eq" ) )                      { wlc_h_eq_set( req ); return; }
    if( get && !strcmp( req->path, "/api/ui" ) )       { h_ui_get( req ); return; }
    if( get && !strcmp( req->path, "/api/peers" ) )    { wlc_h_peers( req ); return; }
    if( IS( "POST", "/api/peers/add" ) )               { wlc_h_peer_add( req ); return; }
    if( IS( "POST", "/api/cast/send" ) )               { wlc_h_cast_send( req ); return; }
    if( IS( "POST", "/api/cast/control" ) )            { wlc_h_cast_control( req ); return; }
    if( get && !strcmp( req->path, "/api/cast/status" ) ) { wlc_h_cast_peer_status( req ); return; }
    if( IS( "POST", "/api/screen/register" ) )         { wlc_h_screen_register( req ); return; }
    if( IS( "POST", "/api/screen/sync" ) )             { wlc_h_screen_sync( req ); return; }
    if( get && !strcmp( req->path, "/api/profiles" ) ) { wlc_h_profiles( req ); return; }
    if( get && !strcmp( req->path, "/api/file" ) )
    {
        char *p = wlc_qparam( req, "path" );
        char *real = wlc_realpath( p );
        if( real == NULL || !wlc_path_ok( req, real ) )
            wlc_error( req, 404, "not found or not shared" );
        else
            wlc_serve_file( req, real );
        free( real );
        free( p );
        return;
    }

    /* --- changes files or settings: this computer only ------------------- */
    if( !req->local )
    {
        wlc_error( req, 403, "only available on the computer running WLC" );
        return;
    }
    if( IS( "POST", "/api/roots" ) )           { wlc_h_roots( req ); return; }
    if( IS( "POST", "/api/rename" ) )          { wlc_h_rename( req ); return; }
    if( IS( "POST", "/api/rename/undo" ) )     { wlc_h_rename_undo( req ); return; }
    if( get && !strcmp( req->path, "/api/rename/history" ) ) { wlc_h_rename_history( req ); return; }
    if( IS( "POST", "/api/convert" ) )         { wlc_h_convert( req ); return; }
    if( get && !strcmp( req->path, "/api/jobs" ) ) { wlc_h_jobs( req ); return; }
    if( IS( "POST", "/api/jobs/cancel" ) )     { wlc_h_jobs_cancel( req ); return; }
    if( IS( "POST", "/api/settings" ) )        { h_settings( req ); return; }
    if( IS( "POST", "/api/ui" ) )              { h_ui_put( req ); return; }

    wlc_error( req, 404, "no such endpoint" );
}

/*****************************************************************************
 * Request reading
 *****************************************************************************/
static void header_copy( const char *head, const char *name, char *out, size_t max )
{
    out[0] = '\0';
    const size_t nlen = strlen( name );
    for( const char *line = strstr( head, "\r\n" ); line != NULL;
         line = strstr( line + 2, "\r\n" ) )
    {
        const char *p = line + 2;
        if( !strncasecmp( p, name, nlen ) && p[nlen] == ':' )
        {
            p += nlen + 1;
            while( *p == ' ' ) p++;
            const char *end = strstr( p, "\r\n" );
            size_t len = end ? (size_t)(end - p) : strlen( p );
            if( len >= max ) len = max - 1;
            memcpy( out, p, len );
            out[len] = '\0';
            return;
        }
    }
}

static void serve( intf_sys_t *sys, int fd, const struct sockaddr_in *peer )
{
    wlc_req_t req;
    memset( &req, 0, sizeof(req) );
    req.sys = sys;
    req.fd = fd;

    struct timeval tv = { .tv_sec = 15 };
    setsockopt( fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv) );
    tv.tv_sec = 30;
    setsockopt( fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv) );
#ifdef SO_NOSIGPIPE
    setsockopt( fd, SOL_SOCKET, SO_NOSIGPIPE, &(int){ 1 }, sizeof(int) );
#endif

    inet_ntop( AF_INET, &peer->sin_addr, req.peer_ip, sizeof(req.peer_ip) );
    req.local = (ntohl( peer->sin_addr.s_addr ) >> 24) == 127;
    struct sockaddr_in me;
    socklen_t melen = sizeof(me);
    if( !getsockname( fd, (struct sockaddr *)&me, &melen ) )
        inet_ntop( AF_INET, &me.sin_addr, req.my_ip, sizeof(req.my_ip) );

    /* headers */
    size_t cap = 16384, used = 0;
    char *buf = malloc( cap + 1 );
    char *hend = NULL;
    if( buf == NULL )
        return;
    while( used < cap )
    {
        ssize_t n = recv( fd, buf + used, cap - used, 0 );
        if( n <= 0 )
            goto out;
        used += n;
        buf[used] = '\0';
        if( (hend = strstr( buf, "\r\n\r\n" )) != NULL )
            break;
    }
    if( hend == NULL )
        goto out;
    *hend = '\0';
    const size_t head_len = (hend - buf) + 4;

    /* request line */
    char *sp1 = strchr( buf, ' ' );
    char *sp2 = sp1 ? strchr( sp1 + 1, ' ' ) : NULL;
    char *eol = strstr( buf, "\r\n" );
    if( sp1 == NULL || sp2 == NULL || (eol != NULL && sp2 > eol)
     || (size_t)(sp1 - buf) >= sizeof(req.method) )
        goto out;
    memcpy( req.method, buf, sp1 - buf );
    char *target = strndup( sp1 + 1, sp2 - sp1 - 1 );
    if( target == NULL )
        goto out;
    char *q = strchr( target, '?' );
    if( q != NULL )
    {
        *q = '\0';
        req.query = strdup( q + 1 );
    }
    vlc_uri_decode( target );
    req.path = target;

    char clen[24];
    header_copy( buf, "Host", req.host, sizeof(req.host) );
    header_copy( buf, "Origin", req.origin, sizeof(req.origin) );
    header_copy( buf, "Cookie", req.cookie, sizeof(req.cookie) );
    header_copy( buf, "X-WilkCast-Pin", req.pin, sizeof(req.pin) );
    header_copy( buf, "Range", req.range, sizeof(req.range) );
    header_copy( buf, "Content-Length", clen, sizeof(clen) );

    /* body */
    size_t want = (size_t)strtoull( clen, NULL, 10 );
    if( want > WLC_MAX_BODY )
    {
        wlc_error( &req, 400, "request too large" );
        goto done;
    }
    if( want > 0 )
    {
        req.body = malloc( want + 1 );
        if( req.body == NULL )
            goto done;
        size_t have = used - head_len;
        if( have > want ) have = want;
        memcpy( req.body, buf + head_len, have );
        while( have < want )
        {
            ssize_t n = recv( fd, req.body + have, want - have, 0 );
            if( n <= 0 )
                goto done;
            have += n;
        }
        req.body[want] = '\0';
        req.body_len = want;
        req.json = json_parse( req.body );
    }

    req.authed = req.local || session_valid( sys, req.cookie );
    route( &req );

done:
    if( req.json != NULL )
        json_value_free( req.json );
    free( req.body );
    free( req.path );
    free( req.query );
out:
    free( buf );
}

static void *worker( void *data )
{
    intf_sys_t *sys = data;

    while( !atomic_load( &sys->stop ) )
    {
        struct pollfd ufd = { .fd = sys->lfd, .events = POLLIN };
        if( poll( &ufd, 1, 400 ) <= 0 )
            continue;
        struct sockaddr_in peer;
        socklen_t len = sizeof(peer);
        int fd = vlc_accept( sys->lfd, (struct sockaddr *)&peer, &len, false );
        if( fd == -1 )
            continue;
        /* On macOS and the BSDs an accepted socket inherits O_NONBLOCK from
         * the listening one (Linux's accept4 does not), and recv() would then
         * fail with EAGAIN before the request has arrived. Serve it blocking. */
        int fl = fcntl( fd, F_GETFL );
        if( fl != -1 && (fl & O_NONBLOCK) )
            fcntl( fd, F_SETFL, fl & ~O_NONBLOCK );
        if( peer.sin_family == AF_INET )
            serve( sys, fd, &peer );
        shutdown( fd, SHUT_RDWR );
        net_Close( fd );
    }
    return NULL;
}

int wlc_http_start( intf_sys_t *sys )
{
    const int base = var_InheritInteger( sys->intf, "wlc-port" );

    sys->lfd = -1;
    for( int port = base; port < base + 10 && sys->lfd == -1; port++ )
    {
        int fd = vlc_socket( AF_INET, SOCK_STREAM, IPPROTO_TCP, true );
        if( fd == -1 )
            return VLC_EGENERIC;
        setsockopt( fd, SOL_SOCKET, SO_REUSEADDR, &(int){ 1 }, sizeof(int) );
        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons( port ),
            .sin_addr.s_addr = htonl( sys->lan ? INADDR_ANY : INADDR_LOOPBACK ),
        };
        if( bind( fd, (struct sockaddr *)&addr, sizeof(addr) ) || listen( fd, 32 ) )
        {
            net_Close( fd );
            continue;
        }
        sys->lfd = fd;
        sys->port = port;
    }
    if( sys->lfd == -1 )
    {
        msg_Err( sys->intf, "WLC Control Center: no free port from %d", base );
        return VLC_EGENERIC;
    }
    for( sys->nworkers = 0; sys->nworkers < WLC_WORKERS; sys->nworkers++ )
        if( vlc_clone( &sys->workers[sys->nworkers], worker, sys,
                       VLC_THREAD_PRIORITY_LOW ) )
            break;
    if( sys->nworkers == 0 )
    {
        net_Close( sys->lfd );
        return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

void wlc_http_stop( intf_sys_t *sys )
{
    for( unsigned i = 0; i < sys->nworkers; i++ )
        vlc_join( sys->workers[i], NULL );
    if( sys->lfd != -1 )
        net_Close( sys->lfd );
}
