/*****************************************************************************
 * wlc_cast.c: WLC Control Center - WilkCast, a small casting protocol for
 *             home and office networks (see doc/wilkcast.md)
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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <math.h>

#include <vlc_url.h>

#define MAGIC      "WILKCAST/1 "
#define PEER_TTL   (INT64_C(15) * CLOCK_FREQ)
#define SCREEN_TTL (INT64_C(15) * CLOCK_FREQ)
#define BEACON_GAP (INT64_C(4) * CLOCK_FREQ)
#define BASE_V1    "/cast/v1/"

static const char *const cast_cmds[] = { "play", "resume", "pause", "toggle",
    "stop", "seek", "seekrel", "volume", "next", "prev" };

static bool cast_cmd_allowed( const char *cmd )
{
    for( size_t i = 0; cmd != NULL && i < ARRAY_SIZE(cast_cmds); i++ )
        if( !strcmp( cmd, cast_cmds[i] ) )
            return true;
    return false;
}

static bool url_ok( const char *url )
{
    return url != NULL && strlen( url ) < 4096
        && (!strncasecmp( url, "http://", 7 ) || !strncasecmp( url, "https://", 8 ));
}

/* A peer's API prefix must stay inside /cast/v1/ and be plain. */
static bool base_ok( const char *b )
{
    size_t len = b ? strlen( b ) : 0;
    if( len < strlen( BASE_V1 ) || len >= 64 || strncmp( b, BASE_V1, strlen( BASE_V1 ) )
     || b[len - 1] != '/' || strstr( b, ".." ) )
        return false;
    for( const char *p = b; *p; p++ )
        if( !( (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z')
            || (*p >= 'A' && *p <= 'Z') || *p == '/' || *p == '-' || *p == '_' ) )
            return false;
    return true;
}

static bool hex_id( const char *id )
{
    if( id == NULL || strlen( id ) != 32 )
        return false;
    for( const char *p = id; *p; p++ )
        if( !( (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') ) )
            return false;
    return true;
}

/*****************************************************************************
 * Browser screens (lock must be held for all of these)
 *****************************************************************************/
static wlc_screen_t *screen_get( intf_sys_t *sys, const char *id )
{
    for( unsigned i = 0; id != NULL && i < sys->nscreens; i++ )
        if( !strcmp( sys->screens[i].id, id ) )
            return &sys->screens[i];
    return NULL;
}

static void screen_clear_cmds( wlc_screen_t *s )
{
    for( unsigned i = 0; i < s->ncmds; i++ )
        free( s->cmds[i] );
    s->ncmds = 0;
}

static void screen_remove( intf_sys_t *sys, unsigned i )
{
    screen_clear_cmds( &sys->screens[i] );
    memmove( &sys->screens[i], &sys->screens[i + 1],
             (sys->nscreens - i - 1) * sizeof(wlc_screen_t) );
    sys->nscreens--;
}

/* takes ownership of cmd */
static void screen_push( wlc_screen_t *s, char *cmd, bool replaces )
{
    if( cmd == NULL )
        return;
    if( replaces )
        screen_clear_cmds( s );
    if( s->ncmds == WLC_SCREEN_CMDS )
    {
        free( s->cmds[0] );
        memmove( s->cmds, s->cmds + 1, (WLC_SCREEN_CMDS - 1) * sizeof(char *) );
        s->ncmds--;
    }
    s->cmds[s->ncmds++] = cmd;
}

static void screen_status_json( intf_sys_t *sys, const wlc_screen_t *s,
                                struct vlc_memstream *ms )
{
    vlc_memstream_printf( ms, "{\"state\":\"%s\",\"time\":%.2f,\"length\":%.2f,"
                          "\"volume\":%.3f,\"title\":", s->state[0] ? s->state : "stopped",
                          s->time, s->length, s->volume );
    wlc_json_str( ms, s->title );
    vlc_memstream_puts( ms, ",\"name\":" );
    wlc_json_str( ms, s->name );
    vlc_memstream_puts( ms, ",\"error\":" );
    wlc_json_str( ms, s->error );
    vlc_memstream_puts( ms, ",\"kind\":\"browser\",\"host\":" );
    wlc_json_str( ms, sys->name );
    vlc_memstream_putc( ms, '}' );
}

/* Turn a WilkCast "play" body into a "load" command for a screen. */
static char *screen_load_cmd( const json_value *j, const char **err )
{
    const char *url = wlc_jstr( j, "url" );
    if( !url_ok( url ) )
    {
        *err = "url must be http or https";
        return NULL;
    }
    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    vlc_memstream_puts( &ms, "{\"cmd\":\"load\",\"url\":" );
    wlc_json_str( &ms, url );
    const char *title = wlc_jstr( j, "title" );
    vlc_memstream_puts( &ms, ",\"title\":" );
    wlc_json_str( &ms, title ? title : "" );
    vlc_memstream_printf( &ms, ",\"position\":%.2f,\"queue\":[",
                          fmax( 0., wlc_jnum( j, "position", 0. ) ) );
    const json_value *q = wlc_jget( j, "queue" );
    unsigned n = 0;
    for( unsigned i = 0; q != NULL && q->type == json_array
                         && i < q->u.array.length && n < WLC_MAX_QUEUE; i++ )
    {
        const char *u = wlc_jstr( q->u.array.values[i], "url" );
        if( !url_ok( u ) )
            continue;
        const char *t = wlc_jstr( q->u.array.values[i], "title" );
        vlc_memstream_puts( &ms, n++ ? ",{\"url\":" : "{\"url\":" );
        wlc_json_str( &ms, u );
        vlc_memstream_puts( &ms, ",\"title\":" );
        wlc_json_str( &ms, t ? t : "" );
        vlc_memstream_putc( &ms, '}' );
    }
    vlc_memstream_puts( &ms, "]}" );
    return vlc_memstream_close( &ms ) ? NULL : ms.ptr;
}

static char *screen_ctl_cmd( const json_value *j, const char **err )
{
    const char *cmd = wlc_jstr( j, "cmd" );
    if( !cast_cmd_allowed( cmd ) )
    {
        *err = "command not allowed for casting";
        return NULL;
    }
    char *out;
    if( asprintf( &out, "{\"cmd\":\"%s\",\"value\":%.3f}", cmd,
                  wlc_jnum( j, "value", 0. ) ) < 0 )
        return NULL;
    return out;
}

void wlc_screens_free( intf_sys_t *sys )
{
    while( sys->nscreens > 0 )
        screen_remove( sys, sys->nscreens - 1 );
}

/*****************************************************************************
 * Discovery: a JSON beacon on UDP multicast + broadcast every few seconds
 *****************************************************************************/
static void send_datagram( intf_sys_t *sys, const char *data, size_t len )
{
    struct sockaddr_in group = { .sin_family = AF_INET,
                                 .sin_port = htons( WLC_CAST_PORT ) };
    inet_pton( AF_INET, WLC_CAST_GROUP, &group.sin_addr );

    struct ifaddrs *list = NULL;
    if( getifaddrs( &list ) != 0 )
        return;
    for( struct ifaddrs *a = list; a != NULL; a = a->ifa_next )
    {
        if( a->ifa_addr == NULL || a->ifa_addr->sa_family != AF_INET
         || !(a->ifa_flags & IFF_UP) )
            continue;
        const struct sockaddr_in *in = (const struct sockaddr_in *)a->ifa_addr;
        /* multicast out of this interface */
        if( a->ifa_flags & IFF_MULTICAST )
        {
            setsockopt( sys->ufd, IPPROTO_IP, IP_MULTICAST_IF,
                        &in->sin_addr, sizeof(in->sin_addr) );
            sendto( sys->ufd, data, len, 0,
                    (struct sockaddr *)&group, sizeof(group) );
        }
        /* and plain broadcast, for networks that drop multicast */
        if( (a->ifa_flags & IFF_BROADCAST) && a->ifa_broadaddr != NULL
         && a->ifa_broadaddr->sa_family == AF_INET )
        {
            struct sockaddr_in bc = *(struct sockaddr_in *)a->ifa_broadaddr;
            bc.sin_port = htons( WLC_CAST_PORT );
            sendto( sys->ufd, data, len, 0, (struct sockaddr *)&bc, sizeof(bc) );
        }
    }
    freeifaddrs( list );
}

static void announce( intf_sys_t *sys, const char *id, const char *name,
                      int port, const char *auth, const char *base,
                      const char *kind, bool bye )
{
    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    vlc_memstream_puts( &ms, MAGIC "{\"id\":" );
    wlc_json_str( &ms, id );
    vlc_memstream_puts( &ms, ",\"name\":" );
    wlc_json_str( &ms, name );
    vlc_memstream_printf( &ms, ",\"port\":%d,\"auth\":\"%s\",\"kind\":\"%s\"",
                          port, auth, kind );
    if( base != NULL )
    {
        vlc_memstream_puts( &ms, ",\"base\":" );
        wlc_json_str( &ms, base );
    }
    vlc_memstream_puts( &ms, bye ? ",\"bye\":true}" : "}" );
    if( vlc_memstream_close( &ms ) )
        return;
    if( ms.length < 1024 )
        send_datagram( sys, ms.ptr, ms.length );
    free( ms.ptr );
}

static void beacon( intf_sys_t *sys, bool bye )
{
    char id[40], name[64], auth[8];
    struct { char id[40], name[64]; bool gone; } scr[WLC_MAX_SCREENS];
    unsigned nscr = 0;
    int port;

    vlc_mutex_lock( &sys->lock );
    strlcpy( id, sys->id, sizeof(id) );
    strlcpy( name, sys->name, sizeof(name) );
    strlcpy( auth, sys->cast_accept, sizeof(auth) );
    port = sys->port;
    /* forget screens whose page was closed, announce the rest */
    for( unsigned i = 0; i < sys->nscreens; )
    {
        wlc_screen_t *s = &sys->screens[i];
        const bool gone = bye || s->seen + SCREEN_TTL < mdate();
        strlcpy( scr[nscr].id, s->id, sizeof(scr[nscr].id) );
        strlcpy( scr[nscr].name, s->name, sizeof(scr[nscr].name) );
        scr[nscr++].gone = gone;
        if( gone && !bye )
            screen_remove( sys, i );
        else
            i++;
    }
    vlc_mutex_unlock( &sys->lock );

    announce( sys, id, name, port, auth, NULL, "wlc", bye );
    for( unsigned i = 0; i < nscr; i++ )
    {
        char base[64];
        snprintf( base, sizeof(base), BASE_V1 "s/%.32s/", scr[i].id );
        announce( sys, scr[i].id, scr[i].name, port, auth, base, "browser",
                  scr[i].gone );
    }
}

static void peer_seen( intf_sys_t *sys, const char *id, const char *name,
                       const char *ip, int port, const char *auth,
                       const char *base, const char *kind,
                       bool manual, bool bye )
{
    vlc_mutex_lock( &sys->lock );
    if( strcmp( id, sys->id ) && screen_get( sys, id ) == NULL )
    {
        unsigned slot = sys->npeers;
        for( unsigned i = 0; i < sys->npeers; i++ )
            if( !strcmp( sys->peers[i].id, id ) )
                slot = i;
        if( bye )
        {
            if( slot < sys->npeers )
            {
                memmove( &sys->peers[slot], &sys->peers[slot + 1],
                         (sys->npeers - slot - 1) * sizeof(wlc_peer_t) );
                sys->npeers--;
            }
        }
        else if( slot < WLC_MAX_PEERS )
        {
            wlc_peer_t *p = &sys->peers[slot];
            if( slot == sys->npeers )
            {
                memset( p, 0, sizeof(*p) );
                sys->npeers++;
            }
            strlcpy( p->id, id, sizeof(p->id) );
            strlcpy( p->name, name && name[0] ? name : "WLC", sizeof(p->name) );
            strlcpy( p->ip, ip, sizeof(p->ip) );
            strlcpy( p->auth, auth ? auth : "pin", sizeof(p->auth) );
            strlcpy( p->base, base_ok( base ) ? base : BASE_V1, sizeof(p->base) );
            strlcpy( p->kind, kind && kind[0] ? kind : "wlc", sizeof(p->kind) );
            p->port = port;
            p->seen = mdate();
            p->manual |= manual;
        }
    }
    vlc_mutex_unlock( &sys->lock );
}

static void *cast_thread( void *data )
{
    intf_sys_t *sys = data;
    mtime_t next = 0;
    char buf[1024];

    while( !atomic_load( &sys->stop ) )
    {
        if( mdate() >= next )
        {
            beacon( sys, false );
            next = mdate() + BEACON_GAP;
            /* forget devices that went quiet */
            vlc_mutex_lock( &sys->lock );
            for( unsigned i = 0; i < sys->npeers; )
            {
                wlc_peer_t *p = &sys->peers[i];
                if( !p->manual && p->seen + PEER_TTL < mdate() )
                {
                    memmove( p, p + 1, (sys->npeers - i - 1) * sizeof(*p) );
                    sys->npeers--;
                }
                else
                    i++;
            }
            vlc_mutex_unlock( &sys->lock );
        }

        struct pollfd ufd = { .fd = sys->ufd, .events = POLLIN };
        if( poll( &ufd, 1, 500 ) <= 0 )
            continue;
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        ssize_t n = recvfrom( sys->ufd, buf, sizeof(buf) - 1, 0,
                              (struct sockaddr *)&from, &flen );
        if( n <= (ssize_t)strlen( MAGIC ) || strncmp( buf, MAGIC, strlen( MAGIC ) ) )
            continue;
        buf[n] = '\0';
        const char *payload = buf + strlen( MAGIC );
        if( payload[0] == '?' )
        {   /* someone just started and wants to know who is here */
            next = 0;
            continue;
        }
        json_value *j = json_parse( payload );
        const char *id = wlc_jstr( j, "id" );
        const int port = (int)wlc_jnum( j, "port", 0 );
        if( id != NULL && strlen( id ) >= 8 && strlen( id ) < 40
         && port > 0 && port < 65536 )
        {
            char ip[64];
            inet_ntop( AF_INET, &from.sin_addr, ip, sizeof(ip) );
            peer_seen( sys, id, wlc_jstr( j, "name" ), ip, port,
                       wlc_jstr( j, "auth" ), wlc_jstr( j, "base" ),
                       wlc_jstr( j, "kind" ), false,
                       wlc_jbool( j, "bye", false ) );
        }
        if( j != NULL )
            json_value_free( j );
    }
    beacon( sys, true );
    return NULL;
}

int wlc_cast_start( intf_sys_t *sys )
{
    int fd = vlc_socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP, true );
    if( fd == -1 )
        return VLC_EGENERIC;
    setsockopt( fd, SOL_SOCKET, SO_REUSEADDR, &(int){ 1 }, sizeof(int) );
#ifdef SO_REUSEPORT
    setsockopt( fd, SOL_SOCKET, SO_REUSEPORT, &(int){ 1 }, sizeof(int) );
#endif
    setsockopt( fd, SOL_SOCKET, SO_BROADCAST, &(int){ 1 }, sizeof(int) );
    setsockopt( fd, IPPROTO_IP, IP_MULTICAST_TTL, &(unsigned char){ 1 }, 1 );
    setsockopt( fd, IPPROTO_IP, IP_MULTICAST_LOOP, &(unsigned char){ 1 }, 1 );

    struct sockaddr_in addr = { .sin_family = AF_INET,
                                .sin_port = htons( WLC_CAST_PORT ),
                                .sin_addr.s_addr = htonl( INADDR_ANY ) };
    if( bind( fd, (struct sockaddr *)&addr, sizeof(addr) ) )
    {
        msg_Warn( sys->intf, "WilkCast: cannot listen on UDP %d (%s); other "
                  "devices can still be added by address", WLC_CAST_PORT,
                  vlc_strerror_c( errno ) );
        net_Close( fd );
        return VLC_EGENERIC;
    }

    /* join the group on every interface that can do multicast */
    struct ifaddrs *list = NULL;
    if( getifaddrs( &list ) == 0 )
    {
        for( struct ifaddrs *a = list; a != NULL; a = a->ifa_next )
        {
            if( a->ifa_addr == NULL || a->ifa_addr->sa_family != AF_INET
             || !(a->ifa_flags & IFF_UP) || !(a->ifa_flags & IFF_MULTICAST) )
                continue;
            struct ip_mreq mreq;
            inet_pton( AF_INET, WLC_CAST_GROUP, &mreq.imr_multiaddr );
            mreq.imr_interface = ((struct sockaddr_in *)a->ifa_addr)->sin_addr;
            setsockopt( fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq) );
        }
        freeifaddrs( list );
    }

    sys->ufd = fd;
    sys->cast_thread_ok = !vlc_clone( &sys->cast_thread, cast_thread, sys,
                                      VLC_THREAD_PRIORITY_LOW );
    if( !sys->cast_thread_ok )
    {
        net_Close( fd );
        sys->ufd = -1;
        return VLC_EGENERIC;
    }
    /* ask who is already here */
    struct sockaddr_in group = { .sin_family = AF_INET,
                                 .sin_port = htons( WLC_CAST_PORT ) };
    inet_pton( AF_INET, WLC_CAST_GROUP, &group.sin_addr );
    sendto( fd, MAGIC "?", strlen( MAGIC ) + 1, 0,
            (struct sockaddr *)&group, sizeof(group) );
    return VLC_SUCCESS;
}

void wlc_cast_stop( intf_sys_t *sys )
{
    if( sys->cast_thread_ok )
        vlc_join( sys->cast_thread, NULL );
    if( sys->ufd != -1 )
        net_Close( sys->ufd );
    sys->ufd = -1;
}

/*****************************************************************************
 * Sender side
 *****************************************************************************/
static void peer_json( struct vlc_memstream *ms, const wlc_peer_t *p, bool first )
{
    if( !first ) vlc_memstream_putc( ms, ',' );
    vlc_memstream_puts( ms, "{\"id\":" );
    wlc_json_str( ms, p->id );
    vlc_memstream_puts( ms, ",\"name\":" );
    wlc_json_str( ms, p->name );
    vlc_memstream_puts( ms, ",\"ip\":" );
    wlc_json_str( ms, p->ip );
    vlc_memstream_puts( ms, ",\"kind\":" );
    wlc_json_str( ms, p->kind );
    vlc_memstream_printf( ms, ",\"port\":%d,\"auth\":\"%s\",\"manual\":%s,\"own\":%s}",
                          p->port, p->auth, p->manual ? "true" : "false",
                          p->own ? "true" : "false" );
}

/* lock must be held */
static void screen_as_peer( intf_sys_t *sys, const wlc_screen_t *s, wlc_peer_t *p )
{
    memset( p, 0, sizeof(*p) );
    strlcpy( p->id, s->id, sizeof(p->id) );
    strlcpy( p->name, s->name, sizeof(p->name) );
    strlcpy( p->ip, s->host, sizeof(p->ip) );
    strlcpy( p->auth, sys->cast_accept, sizeof(p->auth) );
    snprintf( p->base, sizeof(p->base), BASE_V1 "s/%.32s/", s->id );
    strlcpy( p->kind, "browser", sizeof(p->kind) );
    p->port = sys->port;
    p->seen = s->seen;
    p->own = true;
}

void wlc_h_peers( wlc_req_t *req )
{
    intf_sys_t *sys = req->sys;
    struct vlc_memstream ms;
    bool first = true;
    vlc_memstream_open( &ms );
    vlc_memstream_puts( &ms, "{\"peers\":[" );
    vlc_mutex_lock( &sys->lock );
    for( unsigned i = 0; i < sys->nscreens; i++ )
    {
        wlc_peer_t p;
        screen_as_peer( sys, &sys->screens[i], &p );
        peer_json( &ms, &p, first );
        first = false;
    }
    for( unsigned i = 0; i < sys->npeers; i++ )
    {
        peer_json( &ms, &sys->peers[i], first );
        first = false;
    }
    vlc_mutex_unlock( &sys->lock );
    vlc_memstream_puts( &ms, "]}" );
    wlc_send_json( req, 200, &ms );
}

static bool host_ok( const char *host )
{
    if( host == NULL || host[0] == '\0' || strlen( host ) > 63 )
        return false;
    for( const char *p = host; *p; p++ )
        if( !( (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z')
            || (*p >= 'A' && *p <= 'Z') || *p == '.' || *p == '-' ) )
            return false;
    return true;
}

/* GET /cast/v1/info on another device; also tells us which of our own
 * addresses that device can reach. */
static bool peer_probe( intf_sys_t *sys, const char *host, int port,
                        wlc_peer_t *out, char my_ip[64] )
{
    char *resp = NULL;
    bool ok = false;
    if( wlc_http_request( sys, host, port, "GET", BASE_V1 "info", NULL, NULL,
                          &resp, my_ip ) == 200 && resp != NULL )
    {
        json_value *j = json_parse( resp );
        const char *id = wlc_jstr( j, "id" );
        if( id != NULL && strlen( id ) < 40 && wlc_jnum( j, "wilkcast", 0 ) >= 1 )
        {
            const char *name = wlc_jstr( j, "name" );
            const char *auth = wlc_jstr( j, "auth" );
            const char *kind = wlc_jstr( j, "kind" );
            memset( out, 0, sizeof(*out) );
            strlcpy( out->id, id, sizeof(out->id) );
            strlcpy( out->name, name ? name : "WLC", sizeof(out->name) );
            strlcpy( out->auth, auth ? auth : "pin", sizeof(out->auth) );
            strlcpy( out->ip, host, sizeof(out->ip) );
            strlcpy( out->base, BASE_V1, sizeof(out->base) );
            strlcpy( out->kind, kind ? kind : "wlc", sizeof(out->kind) );
            out->port = port;
            ok = true;
        }
        if( j != NULL )
            json_value_free( j );
    }
    free( resp );
    return ok;
}

void wlc_h_peer_add( wlc_req_t *req )
{
    intf_sys_t *sys = req->sys;
    const char *host = wlc_jstr( req->json, "host" );
    int port = (int)wlc_jnum( req->json, "port", WLC_DEFAULT_PORT );
    wlc_peer_t p;

    if( !host_ok( host ) || port < 1 || port > 65535 )
        wlc_error( req, 400, "enter an address like 192.168.1.20" );
    else if( !peer_probe( sys, host, port, &p, NULL ) )
        wlc_error( req, 404, "no WilkCast device answered at that address" );
    else if( !strcmp( p.id, sys->id ) )
        wlc_error( req, 400, "that is this WLC" );
    else
    {
        peer_seen( sys, p.id, p.name, p.ip, p.port, p.auth, p.base, p.kind,
                   true, false );
        wlc_ok( req );
    }
}

static bool peer_find( intf_sys_t *sys, const char *id, wlc_peer_t *out )
{
    bool found = false;
    if( id == NULL )
        return false;
    vlc_mutex_lock( &sys->lock );
    for( unsigned i = 0; i < sys->npeers && !found; i++ )
        if( !strcmp( sys->peers[i].id, id ) )
        {
            *out = sys->peers[i];
            found = true;
        }
    const wlc_screen_t *s = found ? NULL : screen_get( sys, id );
    if( s != NULL )
    {
        screen_as_peer( sys, s, out );
        found = true;
    }
    vlc_mutex_unlock( &sys->lock );
    return found;
}

/* percent-encode a path but keep the slashes */
static char *encode_path( const char *path )
{
    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    const char *p = path;
    while( *p )
    {
        const char *slash = strchr( p, '/' );
        size_t len = slash ? (size_t)(slash - p) : strlen( p );
        if( len > 0 )
        {
            char *seg = strndup( p, len );
            char *enc = seg ? vlc_uri_encode( seg ) : NULL;
            if( enc != NULL )
                vlc_memstream_puts( &ms, enc );
            free( enc );
            free( seg );
        }
        if( slash == NULL )
            break;
        vlc_memstream_putc( &ms, '/' );
        p = slash + 1;
    }
    return vlc_memstream_close( &ms ) ? NULL : ms.ptr;
}

static void relay( wlc_req_t *req, int status, char *resp )
{
    if( status == 200 )
        wlc_send( req, 200, "application/json", NULL,
                  resp ? resp : "{\"ok\":true}", resp ? strlen( resp ) : 11 );
    else if( status == 401 )
        wlc_error( req, 401, "that device wants its PIN" );
    else if( status == 403 )
        wlc_error( req, 403, "that device is not accepting casts" );
    else if( status == 404 && resp != NULL && strstr( resp, "screen" ) )
        wlc_error( req, 404, "that screen was closed" );
    else if( status < 0 )
        wlc_error( req, 404, "that device did not answer" );
    else
        wlc_error( req, 400, "that device refused the request" );
    free( resp );
}

/* Lets this one file be fetched with the media key and returns its URL as
 * seen from my_ip. */
static char *media_url( intf_sys_t *sys, const char *real, const char *my_ip )
{
    if( !wlc_path_shared( sys, real ) )
    {
        vlc_mutex_lock( &sys->lock );
        free( sys->grants[sys->grant_next] );
        sys->grants[sys->grant_next] = strdup( real );
        sys->grant_next = (sys->grant_next + 1) % WLC_MAX_GRANTS;
        vlc_mutex_unlock( &sys->lock );
    }
    char *enc = encode_path( real ), *url = NULL;
    if( enc == NULL || asprintf( &url, "http://%s:%d/media/%s%s", my_ip,
                                 sys->port, sys->media_key, enc ) < 0 )
        url = NULL;
    free( enc );
    return url;
}

static const char *base_name( const char *path )
{
    const char *b = strrchr( path, '/' );
    return b ? b + 1 : path;
}

/* Queue a command for one of our own screens; returns its status. */
static int screen_deliver( intf_sys_t *sys, const char *id, const char *op,
                           const json_value *j, struct vlc_memstream *ms,
                           const char **err )
{
    char *cmd = NULL;
    const bool load = !strcmp( op, "play" );
    if( load )
        cmd = screen_load_cmd( j, err );
    else if( !strcmp( op, "control" ) )
        cmd = screen_ctl_cmd( j, err );
    else if( strcmp( op, "status" ) )
    {
        *err = "no such cast endpoint";
        return 404;
    }
    if( cmd == NULL && strcmp( op, "status" ) )
        return 400;

    vlc_mutex_lock( &sys->lock );
    wlc_screen_t *s = screen_get( sys, id );
    if( s == NULL )
    {
        vlc_mutex_unlock( &sys->lock );
        free( cmd );
        *err = "that screen was closed";
        return 404;
    }
    if( cmd != NULL )
    {
        screen_push( s, cmd, load );
        if( load )
        {   /* show it as opening until the browser reports back */
            strlcpy( s->state, "opening", sizeof(s->state) );
            s->error[0] = '\0';
            const char *t = wlc_jstr( j, "title" );
            strlcpy( s->title, t ? t : "", sizeof(s->title) );
        }
    }
    if( load )
        vlc_memstream_puts( ms, "{\"ok\":true}" );
    else
        screen_status_json( sys, s, ms );
    vlc_mutex_unlock( &sys->lock );
    return 200;
}

void wlc_h_cast_send( wlc_req_t *req )
{
    intf_sys_t *sys = req->sys;
    const json_value *j = req->json;
    const char *pin = wlc_jstr( j, "pin" );
    const bool move = wlc_jbool( j, "move", false );
    wlc_peer_t peer, probe;
    char my_ip[64] = "";

    if( !peer_find( sys, wlc_jstr( j, "peer" ), &peer ) )
    {
        wlc_error( req, 404, "device not found; it may have gone offline" );
        return;
    }

    /* what to send: a list of files, one file, or whatever plays here */
    const json_value *paths = wlc_jget( j, "paths" );
    const char *want = wlc_jstr( j, "path" );
    char **reals = NULL, *url = NULL, *title = NULL;
    size_t nreal = 0;
    double position = wlc_jnum( j, "position", 0. );

    if( paths != NULL && paths->type == json_array && paths->u.array.length > 0 )
    {
        size_t max = paths->u.array.length;
        if( max > WLC_MAX_QUEUE ) max = WLC_MAX_QUEUE;
        reals = calloc( max, sizeof(char *) );
        for( size_t i = 0; reals != NULL && i < max; i++ )
        {
            const json_value *v = paths->u.array.values[i];
            char *r = v->type == json_string ? wlc_realpath( v->u.string.ptr ) : NULL;
            if( r != NULL && wlc_path_ok( req, r ) )
                reals[nreal++] = r;
            else
                free( r );
        }
    }
    else if( want != NULL && want[0] != '\0' )
    {
        char *r = wlc_realpath( want );
        if( r != NULL && wlc_path_ok( req, r ) && (reals = malloc( sizeof(char *) )) )
            reals[nreal++] = r;
        else
            free( r );
    }
    else
    {
        input_thread_t *in = playlist_CurrentInput( sys->pl );
        if( in != NULL )
        {
            input_item_t *item = input_GetItem( in );
            char *uri = item ? input_item_GetURI( item ) : NULL;
            if( uri != NULL )
            {
                char *path = vlc_uri2path( uri );
                char *r = path ? wlc_realpath( path ) : NULL;
                free( path );
                if( r != NULL && wlc_path_ok( req, r )
                 && (reals = malloc( sizeof(char *) )) != NULL )
                    reals[nreal++] = r;
                else
                {
                    free( r );
                    if( url_ok( uri ) )
                        url = strdup( uri );
                }
                free( uri );
            }
            if( item != NULL )
                title = input_item_GetTitleFbName( item );
            position = var_GetInteger( in, "time" ) / (double)CLOCK_FREQ;
            vlc_object_release( in );
        }
    }
    if( nreal == 0 && url == NULL )
    {
        free( reals ); free( title );
        wlc_error( req, 400, "nothing to cast: play something or pick a file" );
        return;
    }

    if( nreal > 0 )
    {
        /* find the address the receiver can reach us on */
        if( peer.own )
            strlcpy( my_ip, peer.ip, sizeof(my_ip) );
        else if( !peer_probe( sys, peer.ip, peer.port, &probe, my_ip ) )
            my_ip[0] = '\0';
        if( my_ip[0] == '\0' )
        {
            for( size_t i = 0; i < nreal; i++ ) free( reals[i] );
            free( reals ); free( title );
            wlc_error( req, 404, "that device did not answer" );
            return;
        }
        url = media_url( sys, reals[0], my_ip );
        if( title == NULL || nreal > 1 || want != NULL )
        {
            free( title );
            title = strdup( base_name( reals[0] ) );
        }
    }

    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    vlc_memstream_puts( &ms, "{\"url\":" );
    wlc_json_str( &ms, url );
    vlc_memstream_puts( &ms, ",\"title\":" );
    wlc_json_str( &ms, title ? title : "" );
    vlc_memstream_printf( &ms, ",\"position\":%.2f,\"from\":", move ? position : 0. );
    vlc_mutex_lock( &sys->lock );
    wlc_json_str( &ms, sys->name );
    vlc_mutex_unlock( &sys->lock );
    if( nreal > 1 )
    {
        vlc_memstream_puts( &ms, ",\"queue\":[" );
        for( size_t i = 1; i < nreal; i++ )
        {
            char *u = media_url( sys, reals[i], my_ip );
            vlc_memstream_puts( &ms, i > 1 ? ",{\"url\":" : "{\"url\":" );
            wlc_json_str( &ms, u ? u : "" );
            vlc_memstream_puts( &ms, ",\"title\":" );
            wlc_json_str( &ms, base_name( reals[i] ) );
            vlc_memstream_putc( &ms, '}' );
            free( u );
        }
        vlc_memstream_putc( &ms, ']' );
    }
    vlc_memstream_putc( &ms, '}' );

    int status = -1;
    char *resp = NULL;
    if( !vlc_memstream_close( &ms ) )
    {
        if( url != NULL && peer.own )
        {
            json_value *body = json_parse( ms.ptr );
            struct vlc_memstream out;
            const char *err = "could not cast";
            vlc_memstream_open( &out );
            status = screen_deliver( sys, peer.id, "play", body, &out, &err );
            if( !vlc_memstream_close( &out ) )
                resp = out.ptr;
            if( body != NULL )
                json_value_free( body );
        }
        else if( url != NULL )
        {
            char path[80];
            snprintf( path, sizeof(path), "%splay", peer.base );
            status = wlc_http_request( sys, peer.ip, peer.port, "POST", path,
                                       pin, ms.ptr, &resp, NULL );
        }
        free( ms.ptr );
    }
    if( status == 200 && move )
        playlist_Stop( sys->pl );
    for( size_t i = 0; i < nreal; i++ ) free( reals[i] );
    free( reals ); free( url ); free( title );
    relay( req, status, resp );
}

static void peer_call( wlc_req_t *req, const char *peer_id, const char *pin,
                       const char *op, const char *method, const char *body,
                       const json_value *j )
{
    wlc_peer_t peer;
    if( !peer_find( req->sys, peer_id, &peer ) )
    {
        wlc_error( req, 404, "device not found" );
        return;
    }
    char *resp = NULL;
    int status;
    if( peer.own )
    {
        struct vlc_memstream ms;
        const char *err = "refused";
        vlc_memstream_open( &ms );
        status = screen_deliver( req->sys, peer.id, op, j, &ms, &err );
        if( !vlc_memstream_close( &ms ) )
            resp = ms.ptr;
        if( status != 200 )
        {
            free( resp );
            wlc_error( req, status, err );
            return;
        }
    }
    else
    {
        char path[80];
        snprintf( path, sizeof(path), "%s%s", peer.base, op );
        status = wlc_http_request( req->sys, peer.ip, peer.port, method, path,
                                   pin, body, &resp, NULL );
    }
    relay( req, status, resp );
}

void wlc_h_cast_control( wlc_req_t *req )
{
    peer_call( req, wlc_jstr( req->json, "peer" ), wlc_jstr( req->json, "pin" ),
               "control", "POST", req->body, req->json );
}

void wlc_h_cast_peer_status( wlc_req_t *req )
{
    char *id = wlc_qparam( req, "peer" );
    char *pin = wlc_qparam( req, "pin" );
    peer_call( req, id, pin, "status", "GET", NULL, NULL );
    free( id );
    free( pin );
}

/*****************************************************************************
 * Browser screens: the page at /screen signs in, registers, then syncs
 *****************************************************************************/
static void host_only( const char *host, char out[64] )
{
    strlcpy( out, host, 64 );
    char *colon = strrchr( out, ':' );
    if( out[0] != '[' && colon != NULL && strchr( out, ':' ) == colon )
        *colon = '\0';
    else if( out[0] == '[' && (colon = strstr( out, "]:" )) != NULL )
        colon[1] = '\0';
}

void wlc_h_screen_register( wlc_req_t *req )
{
    intf_sys_t *sys = req->sys;
    const char *want_id = wlc_jstr( req->json, "id" );
    const char *name = wlc_jstr( req->json, "name" );
    char id[33];

    if( hex_id( want_id ) && strcmp( want_id, sys->id ) )
        strlcpy( id, want_id, sizeof(id) );
    else
        wlc_new_token( id );

    vlc_mutex_lock( &sys->lock );
    wlc_screen_t *s = screen_get( sys, id );
    if( s == NULL )
    {
        if( sys->nscreens == WLC_MAX_SCREENS )
        {   /* make room: drop the screen heard from least recently */
            unsigned old = 0;
            for( unsigned i = 1; i < sys->nscreens; i++ )
                if( sys->screens[i].seen < sys->screens[old].seen )
                    old = i;
            screen_remove( sys, old );
        }
        s = &sys->screens[sys->nscreens++];
        memset( s, 0, sizeof(*s) );
        strlcpy( s->id, id, sizeof(s->id) );
        s->volume = 1.;
    }
    strlcpy( s->name, name && name[0] ? name : "Browser screen", sizeof(s->name) );
    host_only( req->host, s->host );
    s->seen = mdate();
    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    vlc_memstream_puts( &ms, "{\"id\":" );
    wlc_json_str( &ms, s->id );
    vlc_memstream_puts( &ms, ",\"name\":" );
    wlc_json_str( &ms, s->name );
    vlc_memstream_puts( &ms, ",\"host\":" );
    wlc_json_str( &ms, sys->name );
    vlc_memstream_putc( &ms, '}' );
    vlc_mutex_unlock( &sys->lock );
    msg_Info( sys->intf, "WilkCast: browser screen \"%s\" joined from %s",
              name ? name : "", req->peer_ip );
    wlc_send_json( req, 200, &ms );
}

void wlc_h_screen_sync( wlc_req_t *req )
{
    intf_sys_t *sys = req->sys;
    const json_value *j = req->json;
    const json_value *st = wlc_jget( j, "status" );
    static const char *const states[] = { "playing", "paused", "stopped",
                                          "opening", "error" };

    vlc_mutex_lock( &sys->lock );
    wlc_screen_t *s = screen_get( sys, wlc_jstr( j, "id" ) );
    if( s == NULL )
    {
        vlc_mutex_unlock( &sys->lock );
        wlc_error( req, 404, "unknown screen; register again" );
        return;
    }
    s->seen = mdate();
    host_only( req->host, s->host );
    if( st != NULL && st->type == json_object )
    {
        const char *state = wlc_jstr( st, "state" );
        for( size_t i = 0; state != NULL && i < ARRAY_SIZE(states); i++ )
            if( !strcmp( state, states[i] ) )
                strlcpy( s->state, state, sizeof(s->state) );
        double v = wlc_jnum( st, "time", 0. );
        s->time = isfinite( v ) && v >= 0. ? v : 0.;
        v = wlc_jnum( st, "length", 0. );
        s->length = isfinite( v ) && v >= 0. ? v : 0.;
        v = wlc_jnum( st, "volume", 1. );
        s->volume = isfinite( v ) ? fmin( fmax( v, 0. ), 2. ) : 1.;
        const char *t = wlc_jstr( st, "title" );
        if( t != NULL )
            strlcpy( s->title, t, sizeof(s->title) );
        t = wlc_jstr( st, "error" );
        strlcpy( s->error, t ? t : "", sizeof(s->error) );
    }

    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    vlc_memstream_puts( &ms, "{\"name\":" );
    wlc_json_str( &ms, s->name );
    vlc_memstream_puts( &ms, ",\"cmds\":[" );
    for( unsigned i = 0; i < s->ncmds; i++ )
    {
        if( i ) vlc_memstream_putc( &ms, ',' );
        vlc_memstream_puts( &ms, s->cmds[i] );
    }
    vlc_memstream_puts( &ms, "]}" );
    screen_clear_cmds( s );
    vlc_mutex_unlock( &sys->lock );
    wlc_send_json( req, 200, &ms );
}

/*****************************************************************************
 * Receiver side: /cast/v1/...   (access was checked by the router)
 *****************************************************************************/
static void screen_v1( wlc_req_t *req, const char *what )
{
    intf_sys_t *sys = req->sys;
    char id[40] = "";
    const char *op = strchr( what, '/' );
    if( op != NULL && (size_t)(op - what) < sizeof(id) )
        memcpy( id, what, op - what ), id[op - what] = '\0', op++;
    else
        op = "";

    struct vlc_memstream ms;
    vlc_memstream_open( &ms );
    if( !strcmp( op, "info" ) )
    {
        vlc_mutex_lock( &sys->lock );
        const wlc_screen_t *s = screen_get( sys, id );
        if( s != NULL )
        {
            vlc_memstream_puts( &ms, "{\"wilkcast\":1,\"app\":\"WLC screen\","
                                "\"version\":\"" WLC_VERSION "\",\"kind\":\"browser\",\"id\":" );
            wlc_json_str( &ms, s->id );
            vlc_memstream_puts( &ms, ",\"name\":" );
            wlc_json_str( &ms, s->name );
            vlc_memstream_printf( &ms, ",\"auth\":\"%s\",\"via\":", sys->cast_accept );
            wlc_json_str( &ms, sys->name );
            vlc_memstream_putc( &ms, '}' );
        }
        vlc_mutex_unlock( &sys->lock );
        if( s != NULL )
            wlc_send_json( req, 200, &ms );
        else
        {
            free( vlc_memstream_close( &ms ) ? NULL : ms.ptr );
            wlc_error( req, 404, "that screen was closed" );
        }
        return;
    }
    if( (!strcmp( op, "play" ) || !strcmp( op, "control" )) && strcmp( req->method, "POST" ) )
    {
        free( vlc_memstream_close( &ms ) ? NULL : ms.ptr );
        wlc_error( req, 405, "use POST" );
        return;
    }
    const char *err = "refused";
    int status = screen_deliver( sys, id, op, req->json, &ms, &err );
    if( status == 200 )
    {
        if( !strcmp( op, "play" ) )
            msg_Info( sys->intf, "WilkCast: sending a cast from %s to a browser screen",
                      req->peer_ip );
        wlc_send_json( req, 200, &ms );
    }
    else
    {
        free( vlc_memstream_close( &ms ) ? NULL : ms.ptr );
        wlc_error( req, status, err );
    }
}

void wlc_h_castv1( wlc_req_t *req, const char *what )
{
    intf_sys_t *sys = req->sys;
    struct vlc_memstream ms;

    if( !strncmp( what, "s/", 2 ) )
    {
        screen_v1( req, what + 2 );
        return;
    }
    if( !strcmp( what, "info" ) )
    {
        vlc_memstream_open( &ms );
        vlc_mutex_lock( &sys->lock );
        vlc_memstream_puts( &ms, "{\"wilkcast\":1,\"app\":\"WLC\",\"version\":\""
                            WLC_VERSION "\",\"kind\":\"wlc\",\"id\":" );
        wlc_json_str( &ms, sys->id );
        vlc_memstream_puts( &ms, ",\"name\":" );
        wlc_json_str( &ms, sys->name );
        vlc_memstream_printf( &ms, ",\"auth\":\"%s\",\"screens\":[", sys->cast_accept );
        for( unsigned i = 0; i < sys->nscreens; i++ )
        {
            if( i ) vlc_memstream_putc( &ms, ',' );
            vlc_memstream_puts( &ms, "{\"id\":" );
            wlc_json_str( &ms, sys->screens[i].id );
            vlc_memstream_puts( &ms, ",\"name\":" );
            wlc_json_str( &ms, sys->screens[i].name );
            vlc_memstream_printf( &ms, ",\"base\":\"" BASE_V1 "s/%s/\"}",
                                  sys->screens[i].id );
        }
        vlc_memstream_puts( &ms, "]}" );
        vlc_mutex_unlock( &sys->lock );
        wlc_send_json( req, 200, &ms );
    }
    else if( !strcmp( what, "status" ) )
    {
        vlc_memstream_open( &ms );
        wlc_status_json( sys, &ms );
        wlc_send_json( req, 200, &ms );
    }
    else if( !strcmp( what, "play" ) && !strcmp( req->method, "POST" ) )
    {
        const char *url = wlc_jstr( req->json, "url" );
        const char *title = wlc_jstr( req->json, "title" );
        const double pos = wlc_jnum( req->json, "position", 0. );
        const json_value *queue = wlc_jget( req->json, "queue" );
        const bool has_queue = queue != NULL && queue->type == json_array
                            && queue->u.array.length > 0;
        if( !url_ok( url ) )
        {
            wlc_error( req, 400, "url must be http or https" );
            return;
        }
        char opt[48];
        const char *opts[1] = { opt };
        snprintf( opt, sizeof(opt), "start-time=%.2f", pos > 0. ? pos : 0. );
        msg_Info( sys->intf, "WilkCast: playing %s for %s", url, req->peer_ip );
        /* a queue replaces what was lined up here */
        if( has_queue )
            playlist_Clear( sys->pl, pl_Unlocked );
        if( playlist_AddExt( sys->pl, url, title && title[0] ? title : NULL,
                             true, 1, opts, VLC_INPUT_OPTION_TRUSTED, true ) )
        {
            wlc_error( req, 500, "could not start playback" );
            return;
        }
        for( unsigned i = 0; has_queue && i < queue->u.array.length
                             && i < WLC_MAX_QUEUE; i++ )
        {
            const char *u = wlc_jstr( queue->u.array.values[i], "url" );
            const char *t = wlc_jstr( queue->u.array.values[i], "title" );
            if( url_ok( u ) )
                playlist_AddExt( sys->pl, u, t && t[0] ? t : NULL, false,
                                 0, NULL, 0, true );
        }
        wlc_ok( req );
    }
    else if( !strcmp( what, "control" ) && !strcmp( req->method, "POST" ) )
    {
        const char *cmd = wlc_jstr( req->json, "cmd" );
        const char *err = "command not allowed for casting";
        if( !cast_cmd_allowed( cmd ) || wlc_player_cmd( req, cmd, req->json, &err ) )
            wlc_error( req, 400, err );
        else
        {
            vlc_memstream_open( &ms );
            wlc_status_json( sys, &ms );
            wlc_send_json( req, 200, &ms );
        }
    }
    else
        wlc_error( req, 404, "no such cast endpoint" );
}
