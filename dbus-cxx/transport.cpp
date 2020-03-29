/***************************************************************************
 *   Copyright (C) 2020 by Robert Middleton                                *
 *   robert.middleton@rm5248.com                                           *
 *                                                                         *
 *   This file is part of the dbus-cxx library.                            *
 *                                                                         *
 *   The dbus-cxx library is free software; you can redistribute it and/or *
 *   modify it under the terms of the GNU General Public License           *
 *   version 3 as published by the Free Software Foundation.               *
 *                                                                         *
 *   The dbus-cxx library is distributed in the hope that it will be       *
 *   useful, but WITHOUT ANY WARRANTY; without even the implied warranty   *
 *   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU   *
 *   General Public License for more details.                              *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this software. If not see <http://www.gnu.org/licenses/>.  *
 ***************************************************************************/
#include "transport.h"
#include <vector>
#include <string>
#include <map>
#include <fcntl.h>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "dbus-cxx-private.h"

#include "simpletransport.h"
#include "sasl.h"

using DBus::priv::Transport;

class ParsedTransport {
public:
    std::string m_transportName;
    std::map<std::string,std::string> m_config;
};

enum class ParsingState {
    Parsing_Transport_Name,
    Parsing_Key,
    Parsing_Value
};

static std::vector<ParsedTransport> parseTransports( std::string address_str ){
    std::string tmpTransportName;
    std::string tmpKey;
    std::string tmpValue;
    std::map<std::string,std::string> tmpConfig;
    ParsedTransport tmpTransport;
    std::vector<ParsedTransport> retval;
    ParsingState state = ParsingState::Parsing_Transport_Name;

    for( const char& c : address_str ){
        if( state == ParsingState::Parsing_Transport_Name ){
            if( c == ':' ){
                state = ParsingState::Parsing_Key;
                continue;
            }
            tmpTransportName += c;
        }

        if( state == ParsingState::Parsing_Key ){
            if( c == '=' ){
                state = ParsingState::Parsing_Value;
                continue;
            }
            tmpKey += c;
        }

        if( state == ParsingState::Parsing_Value ){
            if( c == ',' ){
                state = ParsingState::Parsing_Key;
                tmpConfig[ tmpKey ] = tmpValue;
                tmpKey = "";
                tmpValue = "";
                continue;
            }

            if( c == ';' ){
                state = ParsingState::Parsing_Transport_Name;
                tmpConfig[ tmpKey ] = tmpValue;

                tmpTransport.m_transportName = tmpTransportName;
                tmpTransport.m_config = tmpConfig;

                retval.push_back( tmpTransport );

                tmpKey = "";
                tmpValue = "";
                tmpConfig.clear();
                tmpTransportName = "";

                continue;
            }

            tmpValue += c;
        }
    }

    tmpConfig[ tmpKey ] = tmpValue;

    if( !tmpTransportName.empty() ){
        tmpTransport.m_transportName = tmpTransportName;
        tmpTransport.m_config = tmpConfig;

        retval.push_back( tmpTransport );
    }

    return retval;
}

static int open_unix_socket( std::string socketAddress ){
    struct sockaddr_un addr;
    int fd;
    int stat;
    int passcred = 1;

    memset( &addr, 0, sizeof( struct sockaddr_un ) );
    fd = ::socket( AF_UNIX, SOCK_STREAM, 0 );
    if( fd < 0 ){
        std::string errmsg = strerror( errno );
        SIMPLELOGGER_DEBUG("dbus.Transport", "Unable to create socket: " + errmsg );
        return fd;
    }

    addr.sun_family = AF_UNIX;
    memcpy( addr.sun_path, socketAddress.c_str(), socketAddress.size() );

    stat = ::connect( fd, (struct sockaddr*)&addr, sizeof( addr ) );
    if( stat < 0 ){
        int my_errno = errno;
        std::string errmsg = strerror( errno );
        SIMPLELOGGER_DEBUG("dbus.Transport", "Unable to connect: " + errmsg );
        errno = my_errno;
        return stat;
    }

    SIMPLELOGGER_DEBUG("dbus.Transport", "Opened dbus connection to " + socketAddress );

    stat = ::setsockopt( fd, SOL_SOCKET, SO_PASSCRED, &passcred, sizeof( int ) );
    if( stat < 0 ){
        int my_errno = errno;
        std::string errmsg = strerror( errno );
        SIMPLELOGGER_DEBUG("dbus.Transport", "Unable to set passcred: " + errmsg );
        errno = my_errno;
        return stat;
    }

    // Turn the FD into non-blocking
    {
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    return fd;
}

Transport::~Transport(){}

std::shared_ptr<Transport> Transport::open_transport( std::string address ){
    std::vector<ParsedTransport> transports = parseTransports( address );
    std::shared_ptr<Transport> retTransport;
    bool negotiateFD = false;

    for( ParsedTransport param : transports ){
        if( param.m_transportName == "unix" ){
            std::string path = param.m_config[ "path" ];
            int fd;

            if( !path.empty() ){
                fd = open_unix_socket( path );
                if( fd >= 0 ){
                    retTransport = SimpleTransport::create( fd, true );
                    if( !retTransport->is_valid() ){
                        retTransport.reset();
                        continue;
                    }
                    negotiateFD = true;
                    break;
                }
            }
        }
    }

    if( retTransport ){
        priv::SASL saslAuth( retTransport->fd(), negotiateFD );
        std::tuple<bool,bool,std::vector<uint8_t>> resp =
                saslAuth.authenticate();

        retTransport->m_serverAddress = std::get<2>( resp );

        if( std::get<0>( resp ) == false ){
            SIMPLELOGGER_DEBUG("dbus.Transport", "Did not authenticate with server" );
            retTransport.reset();
        }
    }

    return retTransport;
}
