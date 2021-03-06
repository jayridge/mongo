/* message

   todo: authenticate; encrypt?
*/

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "stdafx.h"
#include "message.h"
#include <time.h>
#include "../util/goodies.h"
#include "../util/background.h"
#include <fcntl.h>
#include <errno.h>
#include "../db/cmdline.h"
#include "../client/dbclient.h"

namespace mongo {

    bool noUnixSocket = false;

    bool objcheck = false;
    
// if you want trace output:
#define mmm(x)

#ifdef MSG_NOSIGNAL
    const int portSendFlags = MSG_NOSIGNAL;
    const int portRecvFlags = MSG_NOSIGNAL;
#else
    const int portSendFlags = 0;
    const int portRecvFlags = 0;
#endif

    vector<SockAddr> ipToAddrs(const char* ips, int port){
        vector<SockAddr> out;
        if (*ips == '\0'){
            out.push_back(SockAddr("0.0.0.0", port)); // IPv4 all

            if (IPv6Enabled())
                out.push_back(SockAddr("::", port)); // IPv6 all
#ifndef _WIN32
            if (!noUnixSocket)
                out.push_back(SockAddr(makeUnixSockPath(port).c_str(), port)); // Unix socket
#endif
            return out;
        }

        while(*ips){
            string ip;
            const char * comma = strchr(ips, ',');
            if (comma){
                ip = string(ips, comma - ips);
                ips = comma + 1;
            }else{
                ip = string(ips);
                ips = "";
            }

            SockAddr sa(ip.c_str(), port);
            out.push_back(sa);

#ifndef _WIN32
            if (!noUnixSocket && (sa.getAddr() == "127.0.0.1" || sa.getAddr() == "0.0.0.0")) // only IPv4
                out.push_back(SockAddr(makeUnixSockPath(port).c_str(), port));
#endif
        }
        return out;

    }

    /* listener ------------------------------------------------------------------- */

    void Listener::initAndListen() {
        vector<SockAddr> mine = ipToAddrs(_ip.c_str(), _port);
        vector<int> socks;
        int maxfd = 0; // needed for select()

        for (vector<SockAddr>::iterator it=mine.begin(), end=mine.end(); it != end; ++it){
            SockAddr& me = *it;

            int sock = ::socket(me.getType(), SOCK_STREAM, 0);
            if ( sock == INVALID_SOCKET ) {
                log() << "ERROR: listen(): invalid socket? " << OUTPUT_ERRNO << endl;
                return;
            }

            if (me.getType() == AF_UNIX){
#if !defined(_WIN32)
                unlink(me.getAddr().c_str());
#endif
            }else if (me.getType() == AF_INET6){
                // IPv6 can also accept IPv4 connections as mapped addresses (::ffff:127.0.0.1)
                // That causes a conflict if we don't do set it to IPV6_ONLY
                const int one = 1;
                setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, (const char*) &one, sizeof(one));
            }

            prebindOptions( sock );
            
            if ( ::bind(sock, me.raw(), me.addressSize) != 0 ) {
                int x = errno;
                log() << "listen(): bind() failed " << OUTPUT_ERRNOX(x) << " for socket: " << me.toString() << endl;
                if ( x == EADDRINUSE )
                    log() << "  addr already in use" << endl;
                closesocket(sock);
                return;
            }

            if ( ::listen(sock, 128) != 0 ) {
                log() << "listen(): listen() failed " << OUTPUT_ERRNO << endl;
                closesocket(sock);
                return;
            }

            ListeningSockets::get()->add( sock );

            socks.push_back(sock);
            if (sock > maxfd)
                maxfd = sock;
        }

        static long connNumber = 0;
        while ( ! inShutdown() ) {
            fd_set fds[1];
            FD_ZERO(fds);

            for (vector<int>::iterator it=socks.begin(), end=socks.end(); it != end; ++it){
                FD_SET(*it, fds);
            }

            const int ret = select(maxfd+1, fds, NULL, NULL, NULL);
            if (ret == 0){
                log() << "select() returned 0" << endl;
                continue;
            }
            else if (ret < 0){
                if ( ! inShutdown() )
                    log() << "select() failure: ret=" << ret << " " << OUTPUT_ERRNO << endl;
                return;
            }

            for (vector<int>::iterator it=socks.begin(), end=socks.end(); it != end; ++it){
                if (! (FD_ISSET(*it, fds)))
                    continue;

                SockAddr from;
                int s = accept(*it, from.raw(), &from.addressSize);
                if ( s < 0 ) {
                    int x = errno; // so no global issues
                    if ( x == ECONNABORTED || x == EBADF ) {
                        log() << "Listener on port " << _port << " aborted" << endl;
                        return;
                    } if ( x == 0 && inShutdown() ){
                        return;   // socket closed
                    }
                    log() << "Listener: accept() returns " << s << " " << OUTPUT_ERRNOX(x) << endl;
                    continue;
                }
                if (from.getType() != AF_UNIX)
                    disableNagle(s);
                if ( _logConnect && ! cmdLine.quiet ) 
                    log() << "connection accepted from " << from.toString() << " #" << ++connNumber << endl;
                accepted(s, from);
            }
        }
    }

    void Listener::accepted(int sock, const SockAddr& from){
        accepted( new MessagingPort(sock, from) );
    }

    /* messagingport -------------------------------------------------------------- */

    class PiggyBackData {
    public:
        PiggyBackData( MessagingPort * port ) {
            _port = port;
            _buf = new char[1300];
            _cur = _buf;
        }

        ~PiggyBackData() {
            DESTRUCTOR_GUARD (
                flush();
                delete( _cur );
            );
        }

        void append( Message& m ) {
            assert( m.data->len <= 1300 );

            if ( len() + m.data->len > 1300 )
                flush();

            memcpy( _cur , m.data , m.data->len );
            _cur += m.data->len;
        }

        void flush() {
            if ( _buf == _cur )
                return;

            _port->send( _buf , len(), "flush" );
            _cur = _buf;
        }

        int len() {
            return _cur - _buf;
        }

    private:

        MessagingPort* _port;

        char * _buf;
        char * _cur;
    };

    class Ports { 
        set<MessagingPort*>& ports;
        mongo::mutex m;
    public:
        // we "new" this so it is still be around when other automatic global vars
        // are being destructed during termination.
        Ports() : ports( *(new set<MessagingPort*>()) ) {}
        void closeAll() { \
            scoped_lock bl(m);
            for ( set<MessagingPort*>::iterator i = ports.begin(); i != ports.end(); i++ )
                (*i)->shutdown();
        }
        void insert(MessagingPort* p) { 
            scoped_lock bl(m);
            ports.insert(p);
        }
        void erase(MessagingPort* p) { 
            scoped_lock bl(m);
            ports.erase(p);
        }
    } ports;



    void closeAllSockets() {
        ports.closeAll();
    }

    MessagingPort::MessagingPort(int _sock, const SockAddr& _far) : sock(_sock), piggyBackData(0), farEnd(_far), _timeout() {
        ports.insert(this);
    }

    MessagingPort::MessagingPort( int timeout ) {
        ports.insert(this);
        sock = -1;
        piggyBackData = 0;
        _timeout = timeout;
    }

    void MessagingPort::shutdown() {
        if ( sock >= 0 ) {
            closesocket(sock);
            sock = -1;
        }
    }

    MessagingPort::~MessagingPort() {
        if ( piggyBackData )
            delete( piggyBackData );
        shutdown();
        ports.erase(this);
    }

    class ConnectBG : public BackgroundJob {
    public:
        int sock;
        int res;
        SockAddr farEnd;
        void run() {
            res = ::connect(sock, farEnd.raw(), farEnd.addressSize);
        }
    };

    bool MessagingPort::connect(SockAddr& _far)
    {
        farEnd = _far;

        sock = socket(farEnd.getType(), SOCK_STREAM, 0);
        if ( sock == INVALID_SOCKET ) {
            log() << "ERROR: connect(): invalid socket? " << OUTPUT_ERRNO << endl;
            return false;
        }

        if ( _timeout > 0 ) {
            setSockTimeouts( sock, _timeout );
        }
                
        ConnectBG bg;
        bg.sock = sock;
        bg.farEnd = farEnd;
        bg.go();

        if ( bg.wait(5000) ) {
            if ( bg.res ) {
                closesocket(sock);
                sock = -1;
                return false;
            }
        }
        else {
            // time out the connect
            closesocket(sock);
            sock = -1;
            bg.wait(); // so bg stays in scope until bg thread terminates
            return false;
        }

        if (farEnd.getType() != AF_UNIX)
            disableNagle(sock);

#ifdef SO_NOSIGPIPE
        // osx
        const int one = 1;
        setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(int));
#endif

        return true;
    }

    bool MessagingPort::recv(Message& m) {
        try {
        again:
            mmm( out() << "*  recv() sock:" << this->sock << endl; )
            int len = -1;
            
            char *lenbuf = (char *) &len;
            int lft = 4;
            recv( lenbuf, lft );
            
            if ( len < 0 || len > 16000000 ) {
                if ( len == -1 ) {
                    // Endian check from the database, after connecting, to see what mode server is running in.
                    unsigned foo = 0x10203040;
                    send( (char *) &foo, 4, "endian" );
                    goto again;
                }
                
                if ( len == 542393671 ){
                    // an http GET
                    log() << "looks like you're trying to access db over http on native driver port.  please add 1000 for webserver" << endl;
                    string msg = "You are trying to access MongoDB on the native driver port. For http diagnostic access, add 1000 to the port number\n";
                    stringstream ss;
                    ss << "HTTP/1.0 200 OK\r\nConnection: close\r\nContent-Type: text/plain\r\nContent-Length: " << msg.size() << "\r\n\r\n" << msg;
                    string s = ss.str();
                    send( s.c_str(), s.size(), "http" );
                    return false;
                }
                log() << "bad recv() len: " << len << '\n';
                return false;
            }
            
            int z = (len+1023)&0xfffffc00;
            assert(z>=len);
            MsgData *md = (MsgData *) malloc(z);
            assert(md);
            md->len = len;
            
            if ( len <= 0 ) {
                out() << "got a length of " << len << ", something is wrong" << endl;
                return false;
            }
            
            char *p = (char *) &md->id;
            int left = len -4;
            recv( p, left );
            
            m.setData(md, true);
            return true;

        } catch ( const SocketException & ) {
            m.reset();
            return false;
        }
    }
    
    void MessagingPort::reply(Message& received, Message& response) {
        say(/*received.from, */response, received.data->id);
    }

    void MessagingPort::reply(Message& received, Message& response, MSGID responseTo) {
        say(/*received.from, */response, responseTo);
    }

    bool MessagingPort::call(Message& toSend, Message& response) {
        mmm( out() << "*call()" << endl; )
        MSGID old = toSend.data->id;
        say(/*to,*/ toSend);
        while ( 1 ) {
            bool ok = recv(response);
            if ( !ok )
                return false;
            //out() << "got response: " << response.data->responseTo << endl;
            if ( response.data->responseTo == toSend.data->id )
                break;
            out() << "********************" << endl;
            out() << "ERROR: MessagingPort::call() wrong id got:" << (unsigned)response.data->responseTo << " expect:" << (unsigned)toSend.data->id << endl;
            out() << "  toSend op: " << toSend.data->operation() << " old id:" << (unsigned)old << endl;
            out() << "  response msgid:" << (unsigned)response.data->id << endl;
            out() << "  response len:  " << (unsigned)response.data->len << endl;
            out() << "  response op:  " << response.data->operation() << endl;
            out() << "  farEnd: " << farEnd << endl;
            assert(false);
            response.reset();
        }
        mmm( out() << "*call() end" << endl; )
        return true;
    }

    void MessagingPort::say(Message& toSend, int responseTo) {
        assert( toSend.data );
        mmm( out() << "*  say() sock:" << this->sock << " thr:" << GetCurrentThreadId() << endl; )
        toSend.data->id = nextMessageId();
        toSend.data->responseTo = responseTo;

        if ( piggyBackData && piggyBackData->len() ) {
            mmm( out() << "*     have piggy back" << endl; )
            if ( ( piggyBackData->len() + toSend.data->len ) > 1300 ) {
                // won't fit in a packet - so just send it off
                piggyBackData->flush();
            }
            else {
                piggyBackData->append( toSend );
                piggyBackData->flush();
                return;
            }
        }

        send( (char*)toSend.data, toSend.data->len, "say" );
    }

    // sends all data or throws an exception
    void MessagingPort::send( const char * data , int len, const char *context ){
        while( len > 0 ) {
            int ret = ::send( sock , data , len , portSendFlags );
            if ( ret == -1 ) {
                if ( errno != EAGAIN || _timeout == 0 ) {
                    log() << "MessagingPort " << context << " send() " << OUTPUT_ERRNO << ' ' << farEnd.toString() << endl;
                    throw SocketException();                    
                } else {
                    if ( !serverAlive( farEnd.toString() ) ) {
                        log() << "MessagingPort " << context << " send() remote dead " << farEnd.toString() << endl;
                        throw SocketException();                        
                    }
                }
            } else {
                assert( ret <= len );
                len -= ret;
                data += ret;
            }
        }
    }
    
    void MessagingPort::recv( char * buf , int len ){
        while( len > 0 ) {
            int ret = ::recv( sock , buf , len , portRecvFlags );
            if ( ret == 0 ) {
                DEV out() << "MessagingPort recv() conn closed? " << farEnd.toString() << endl;
                throw SocketException();
            }
            if ( ret == -1 ) {
                if ( errno != EAGAIN || _timeout == 0 ) {                
                    log() << "MessagingPort recv() " << OUTPUT_ERRNO << " " << farEnd.toString()<<endl;
                    throw SocketException();
                } else {
                    if ( !serverAlive( farEnd.toString() ) ) {
                        log() << "MessagingPort recv() remote dead " << farEnd.toString() << endl;
                        throw SocketException();                        
                    }
                }
            } else {
                if ( len <= 4 && ret != len )
                    log() << "MessagingPort recv() got " << ret << " bytes wanted len=" << len << endl;
                assert( ret <= len );
                len -= ret;
                buf += ret;
            }
        }
    }

    int MessagingPort::unsafe_recv( char *buf, int max ) {
        return ::recv( sock , buf , max , portRecvFlags );        
    }
    
    void MessagingPort::piggyBack( Message& toSend , int responseTo ) {

        if ( toSend.data->len > 1300 ) {
            // not worth saving because its almost an entire packet
            say( toSend );
            return;
        }

        // we're going to be storing this, so need to set it up
        toSend.data->id = nextMessageId();
        toSend.data->responseTo = responseTo;

        if ( ! piggyBackData )
            piggyBackData = new PiggyBackData( this );

        piggyBackData->append( toSend );
    }

    unsigned MessagingPort::remotePort(){
        return farEnd.getPort();
    }

    MSGID NextMsgId;
    bool usingClientIds = 0;
    ThreadLocalValue<int> clientId;

    struct MsgStart {
        MsgStart() {
            NextMsgId = (((unsigned) time(0)) << 16) ^ curTimeMillis();
            assert(MsgDataHeaderSize == 16);
        }
    } msgstart;
    
    MSGID nextMessageId(){
        MSGID msgid = NextMsgId++;
        
        if ( usingClientIds ){
            msgid = msgid & 0xFFFF;
            msgid = msgid | clientId.get();
        }

        return msgid;
    }

    bool doesOpGetAResponse( int op ){
        return op == dbQuery || op == dbGetMore;
    }
    
    void setClientId( int id ){
        usingClientIds = true;
        id = id & 0xFFFF0000;
        massert( 10445 ,  "invalid id" , id );
        clientId.set( id );
    }
    
    int getClientId(){
        return clientId.get();
    }
    
} // namespace mongo
