/*
 * File: enumbackend.cc
 *
 * Description: This file is part of the ENUM backend for PowerDNS
 *
 * Copyright (C) Shin Sterneck 2013-2015 (email: shin at sterneck dot asia)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "enumbackend.hh"

EnumBackend::EnumBackend ( const string &suffix )
{
    setArgPrefix ( "enum" + suffix );

    L << Logger::Debug << "Creating new ENUM backend" << endl;
    rrs = new vector<DNSResourceRecord>();

}

void EnumBackend::lookup ( const QType &qtype, const DNSName &qdomain, DNSPacket *pkt_p, int zoneId )
{
    ComboAddress remoteIp;

    //check for ECS data and use it if found
    if ( pkt_p->hasEDNSSubnet() ) {
        remoteIp = pkt_p->getRealRemote().getNetwork();

    } else {
        remoteIp = pkt_p->getRemote();
    }

    L << Logger::Debug << "enum " << "Handling Query Request: '" << qdomain.toStringNoDot() << ":" << qtype.getName();

    DNSResourceRecord record;
    record.qname = DNSName ( "1.2.3.4." + getArg ( "domain-suffix" ) );
    record.qtype = QType::NAPTR;
    record.content = "20 10 \"U\" \"E2U+h323\" \"\" h323:1.2.3.4@gw1.example.com";
    record.auth = 1;
    record.ttl = 100;
    record.domain_id = 1;

    rrs->push_back ( record );

}

bool EnumBackend::get ( DNSResourceRecord &rr )
{
    if ( rrs->size() > 0 ) {
        rr = rrs->at ( rrs->size() - 1 );
        rrs->pop_back();
        return true;
    }

    return false;
}

bool EnumBackend::getSOA ( const DNSName &name, SOAData &soadata, DNSPacket *p )
{

    L << Logger::Debug << "enum getSOA Function call : " << name.toStringNoDot() << endl;

    const string domainSuffix = getArg ( "domain-suffix" );

    if ( std::equal ( domainSuffix.rbegin(), domainSuffix.rend(), name.toStringNoDot().rbegin() ) ) {
        L << Logger::Debug << "Match Found in getSOA function" << endl;
        soadata.domain_id = 1;
        soadata.qname = DNSName (domainSuffix );
        soadata.serial = 2016092701;
        soadata.refresh = 10800;
        soadata.retry = 3600;
        soadata.expire = 1209600;
        soadata.ttl = 300;
        soadata.hostmaster = DNSName ( "postmaster.example.com" );
        soadata.nameserver = DNSName ( "enum-ns1.example.com" );
        return true;
    }

    return false;
}

bool EnumBackend::list ( const DNSName &target, int domain_id, bool include_disabled )
{
    L << Logger::Debug << "enum list Function call" << endl;
    return false;
}

EnumBackend::~EnumBackend()
{
    delete rrs;
    rrs = NULL;
}


class EnumFactory : public BackendFactory
{
    public:

        EnumFactory() : BackendFactory ( "enum" ) {
        }

        /**
         * @brief declares configuration options
         * @param suffix specified the configuration suffix used by PowerDNS
         */
        void declareArguments ( const string &suffix ) {
            // ENUM Configuration
            declare ( suffix, "domain-suffix", "Set the domain suffix of the ENUM RRs without the 'dot' character", "e164.arpa" );
            declare ( suffix, "ldap-server", "Set the LDAP server hostname" , "host" );
            declare ( suffix, "ldap-username", "Set the LDAP username" , "user" );
            declare ( suffix, "ldap-password", "Set the LDAP password" , "pass" );
        }

        /**
         * @brief function to make DNSBackend as documented by PowerDNS
         * @param suffix specified configuration suffix used by PowerDNS
         * @return EnumBackend object
         */
        DNSBackend *make ( const string &suffix ) {
            return new EnumBackend ( suffix );
        }
};


/**
 * @class EnumLoader
 * @author Shin Sterneck
 * @brief The EnumLoader class to help load the backend itself
 */
class EnumLoader
{
    public:

        /**
         * @brief The backend loader
         */
        EnumLoader() {
            BackendMakers().report ( new EnumFactory );
        }

};

static EnumLoader enumloader;
