/* 
 * File: geosqlbackend.h
 * 
 * Description: This file is part of the GeoSQL backend for PowerDNS
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

#ifndef GEOSQLBACKEND_H
#define	GEOSQLBACKEND_H

#define SQL_RESP_TYPE_REGION        0
#define SQL_RESP_TYPE_DNSRR         1
#define SQL_REST_TYPE_TEST          2

#include <opendbx/api>
#include <pdns/utility.hh>
#include <pdns/dnsbackend.hh>
#include <pdns/dns.hh>
#include <pdns/dnspacket.hh>
#include <pdns/logger.hh>
#include <boost/regex.hpp>
#include <boost/any.hpp> 

using std::string;

class GeoSqlBackend : public DNSBackend {
    
public:
    GeoSqlBackend(const string &suffix);
    virtual ~GeoSqlBackend();

    bool list(const DNSName &target, int domain_id, bool include_disabled=false);
    void lookup(const QType &qtype, const DNSName &qdomain, DNSPacket *pkt_p=0, int zoneId=-1); 
    bool get(DNSResourceRecord &rr);
    bool getSOA(const DNSName &name, SOAData &soadata, DNSPacket *p=0);

private:    
    struct sqlregion {
        string regionname;
        string countrycode;
    };
    
    bool getRegionForIP(string &ip, sqlregion &returned_countryID);
    bool getGeoDnsRecords(const QType &type, const string &qdomain, const sqlregion &region);
    bool getSqlData(OpenDBX::Conn *&conn, string &sqlStatement, std::vector<boost::any> &sqlResponseData, int sqlResponseType);
    inline void logEntry(Logger::Urgency urgency, string message);
    
    OpenDBX::Conn *geoip_db;
    OpenDBX::Conn *pdns_db;
    vector<DNSResourceRecord> *rrs;
};

#endif	/* GEOSQLBACKEND_H */
