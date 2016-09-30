/*
 * File: enumbackend.hh
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

#ifndef ENUMBACKEND_HH
#define	ENUMBACKEND_HH

#include <pdns/utility.hh>
#include <pdns/dnsbackend.hh>
#include <pdns/dns.hh>
#include <pdns/dnspacket.hh>
#include <pdns/logger.hh>
#include <modules/ldapbackend/powerldap.hh>
#include <boost/regex.hpp>

using std::string;

static const char* ldap_attr[] = {
    "distinguishedName"
};

class EnumBackend : public DNSBackend
{

    public:

        EnumBackend ( const string &suffix );
        virtual ~EnumBackend();

        virtual bool getSOA ( const DNSName &name, SOAData &soadata, DNSPacket *p = 0 );
        virtual void lookup ( const QType &qtype, const DNSName &qdomain, DNSPacket *pkt_p = 0, int zoneId = -1 );
        virtual bool list ( const DNSName &target, int domain_id, bool include_disabled = false );
        virtual bool get ( DNSResourceRecord &r );

private:

        PowerLDAP* ldap;
        vector<DNSResourceRecord> *rrs;
        int ldap_msgid;
        PowerLDAP::sentry_t ldap_result;

};

#endif	/* ENUMBACKEND_HH */
