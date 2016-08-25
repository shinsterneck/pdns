/*
 * File: geosqlbackend.cpp
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

#include "geosqlbackend.hh"

/**
 * @brief GeoSqlBackend Constructor
 * @param suffix Specifies configuration suffix for PowerDNS
 */
GeoSqlBackend::GeoSqlBackend ( const string &suffix )
{
    // load configuration
    setArgPrefix ( "geosql" + suffix );
    rrs = new vector<DNSResourceRecord>();
    geosqlRrs = new set<string>();

    try {
        // GeoIP Database Connectivity
        geoip_db = new SMySQL ( getArg ( "geo-database" ),
                                getArg ( "geo-host" ),
                                getArgAsNum ( "geo-port" ),
                                getArg ( "geo-socket" ),
                                getArg ( "geo-username" ),
                                getArg ( "geo-password" ),
                                getArg ( "geo-group" ),
                                mustDo ( "geo-innodb-read-committed" ),
                                getArgAsNum ( "geo-timeout" ) );

        // PowerDNS Database Connectivity
        pdns_db = new SMySQL ( getArg ( "pdns-database" ),
                               getArg ( "pdns-host" ),
                               getArgAsNum ( "pdns-port" ),
                               getArg ( "pdns-socket" ),
                               getArg ( "pdns-username" ),
                               getArg ( "pdns-password" ),
                               getArg ( "pdns-group" ),
                               mustDo ( "pdns-innodb-read-committed" ),
                               getArgAsNum ( "pdns-timeout" ) );

        // Preload geosql enabled records
        try {
            SSqlStatement::result_t result;
            pdns_db->prepare ( getArg ( "sql-pdns-lookup-geosqlenabled" ), 0 )
            ->execute()
            ->getResult ( result );

            if ( !result.empty() ) {
                boost::regex re ( "^(.*)\\..*\\." + getArg ( "domain-suffix" ) + "$" );

                // remove geosql country/region and suffix and store in simple cache set
                // sets will ensure uniquenes
                for ( unsigned int i = 0; i < result.size(); i++ ) {
                    boost::smatch matches;

                    if ( boost::regex_match ( result[i][0], matches, re ) ) {
                        geosqlRrs->insert ( string ( matches[1] ) );
                    }
                }
            }

            L << Logger::Alert << "geosql Discovered unique geosql enabled records: " << geosqlRrs->size() << endl;

        } catch ( std::exception &e ) {
            ostringstream oss;
            oss << e.what();
            throw PDNSException ( "geosql record preloading error: " + oss.str() );
        }

    } catch ( SSqlException &e ) {
        L << Logger::Error << "geosql DB Connection failed: " << e.txtReason() << endl;
    }
}

/**
 * @brief Used by PowerDNS for zone transfer purposes
 * @param target Stores the DNS name
 * @param domain_id The Domain ID for the domain in question
 * @param include_disabled Specified whether disabled domains should be included in the response
 * @return false (no support for zone transfers in this backend, use main zone for this purpose for now)
 */
bool GeoSqlBackend::list ( const DNSName &target, int domain_id, bool include_disabled )
{
    return false;
}

/**
 * @brief Function for looking up the DNS records and storing the result into a vector.
 * @param qtype The DNS query type
 * @param qdomain The DNS domain name
 * @param pkt_p The DNS Packet
 * @param zoneId The Zone ID
 */
void GeoSqlBackend::lookup ( const QType &qtype, const DNSName &qdomain, DNSPacket *pkt_p, int zoneId )
{
    ComboAddress remoteIp;

    //check if qdomain is a registered geosql enabled record, if not skip the whole backend
    if ( geosqlRrs->find ( qdomain.toStringNoDot() ) != geosqlRrs->end() ) {
        logEntry ( Logger::Debug, "Handling Query Request: '" + string ( qdomain.toStringNoDot() ) + ":" + string ( qtype.getName() ) + "'" );

        //check for ECS data and use it if found
        if ( pkt_p->hasEDNSSubnet() ) {
            logEntry ( Logger::Debug, "EDNS0 Client-Subnet Field Found!" );
            remoteIp = pkt_p->getRealRemote().getNetwork();

        } else {
            remoteIp = pkt_p->getRemote();
        }

        // get region and dns records for that region
        sqlregion region;

        if ( getRegionForIP ( remoteIp, region ) ) {
            getGeoDnsRecords ( qtype, qdomain.toStringNoDot(), region );

        } else {
            return;
        }

    } else {
        logEntry ( Logger::Debug, "Skipping Query request: '" + qdomain.toStringNoDot() + "' not a geosql enabled record" );
    }
}

/**
 * @brief Function used by PowerDNS to retrieve the records
 * @param rr Reference containing the individual DNSResourceRecord
 * @return true as long as there are records left in the vector filled by the lookup() function
 */
bool GeoSqlBackend::get ( DNSResourceRecord &rr )
{
    if ( rrs->size() > 0 ) {
        rr = rrs->at ( rrs->size() - 1 );
        rrs->pop_back();
        return true;
    }

    return false;
}

/**
 * @brief Function used by PowerDNS to retrieve the SOA record for a domain. In this backend we do not support SOA records.
 * @param name The DNS domain name
 * @param soadata reference to the SOA data
 * @param p the DNS packet
 */
bool GeoSqlBackend::getSOA ( const DNSName &name, SOAData &soadata, DNSPacket *p )
{
    return false;
}

/**
 * @brief Function to get region
 * @param ip The source IP address from the request packet
 * @param returned_region contains the identifed region information
 * @return bool success or failure indicator
 */
bool GeoSqlBackend::getRegionForIP ( ComboAddress &ip, sqlregion &returned_region )
{
    bool foundCountry = false;
    string sqlstmt = getArg ( "sql-geo-lookup-region" );
    boost::replace_all ( sqlstmt, "{{S-IP}}", ip.toString() );
    std::vector<boost::any> sqlResponseData;

    try {
        if ( getSqlData ( geoip_db->prepare ( sqlstmt, 0 ), sqlResponseData, SQL_RESP_TYPE_REGION )
             && !sqlResponseData.empty() ) {
            sqlregion region = boost::any_cast<sqlregion> ( sqlResponseData.at ( 0 ) );
            boost::to_lower ( region.regionname );
            boost::to_lower ( region.countrycode );
            returned_region = region;
            foundCountry = true;

        }

    } catch ( std::exception &e ) {
        logEntry ( Logger::Critical, "Error while parsing SQL response data for GeoIP region! " + string ( e.what() ) );
    }

    if ( foundCountry ) {
        string logentry = "Identified as: '" + returned_region.countrycode;

        if ( !returned_region.regionname.empty() ) {
            logentry.append ( "|" + returned_region.regionname + "'" );

        } else {
            logentry.append ( "'" );
        }

        logEntry ( Logger::Debug, logentry );

    } else {
        logEntry ( Logger::Debug, "No Region Found" );
    }

    return foundCountry;
}

/**
 * @brief Function to retrieve the DNS records according to the geographic location (assigned by region field)
 * @param type the DNS query type
 * @param qdomain the DNS domain name
 * @param region Specifies the records for the supplied region
 * @return bool success of failure indicator
 */
bool GeoSqlBackend::getGeoDnsRecords ( const QType &type, const string &qdomain, const sqlregion &region )
{
    bool foundRecords = false;
    string sqlstmt;

    if ( type.getCode() == QType::ANY || type.getCode() == QType::SOA ) {
        sqlstmt = getArg ( "sql-pdns-lookuptype-any" );

    } else {
        sqlstmt = getArg ( "sql-pdns-lookuptype" );
    }

    boost::replace_all ( sqlstmt, "{{DOMAIN-SUFFIX}}", getArg ( "domain-suffix" ) );
    boost::replace_all ( sqlstmt, "{{QDOMAIN}}", qdomain );
    boost::replace_all ( sqlstmt, "{{QTYPE}}", type.getName() );

    string sqlstmt_region = sqlstmt;
    string sqlstmt_cc = sqlstmt;

    std::vector<boost::any> sqlResponseData;

    if ( !region.countrycode.empty() ) {
        boost::replace_all ( sqlstmt_region, "{{REGION}}", region.countrycode );

        DNSResourceRecord row;

        if ( getSqlData ( pdns_db->prepare ( sqlstmt_region, 0 ), sqlResponseData, SQL_RESP_TYPE_DNSRR ) ) {
            foundRecords = true;
        }

    } else if ( !region.regionname.empty() ) {
        boost::replace_all ( sqlstmt_cc, "{{REGION}}", region.regionname );

        if ( getSqlData ( pdns_db->prepare ( sqlstmt_cc, 0 ), sqlResponseData, SQL_RESP_TYPE_DNSRR ) ) {
            foundRecords = true;
        }
    }

    if ( foundRecords ) {
        DNSResourceRecord record;
        record.auth = 1;

        try {
            for ( int i = 0; i < sqlResponseData.size(); i++ ) {
                record = boost::any_cast<DNSResourceRecord> ( sqlResponseData.at ( i ) );
                rrs->push_back ( record );
            }

        } catch ( std::exception &e ) {
            logEntry ( Logger::Alert, "Error while parsing SQL response data for DNS Records: " + string ( e.what() ) );
        }
    }

    return foundRecords;
}

/**
 * @brief unified way of handling sql queries and storing the result into a vector
 * @param conn OpenDBX database connection object
 * @param sqlStatement SQL Satement to be executed
 * @param sqlResponseData Vector containing the database response records
 * @param sqlResponseType Specified what type of response is expected (SQL_RESP_TYPE_REGION or SQL_RESP_TYPE_DNSRR)
 * @return bool sucess of failure indicator
 */
bool GeoSqlBackend::getSqlData ( SSqlStatement *sqlStatement, std::vector<boost::any> &sqlResponseData, int sqlResponseType )
{

    bool dataAvailable = false;
    sqlResponseData.clear();
    logEntry ( Logger::Debug, "Preparing SQL Statement: " + sqlStatement->getQuery() );

    switch ( sqlResponseType ) {
        case SQL_RESP_TYPE_DNSRR: {

                DNSResourceRecord row;
                SSqlStatement::result_t result;
                sqlStatement->execute()->getResult ( result );

                if ( !result.empty() ) {
                    dataAvailable = true;

                    for ( int i = 0 ; i < result.size(); i++ ) {
                        row.qname = DNSName ( result[i][0] );
                        row.qtype = string ( result[i][1] );

                        if ( row.qtype == QType::MX || row.qtype == QType::SRV ) {
                            row.content = string ( result[i][4]  + " " + string ( result[i][2] ) );

                        } else {
                            row.content = string ( result[i][2] );
                        }

                        row.ttl = pdns_stou ( result[i][3] );

                        sqlResponseData.push_back ( row );
                    }

                    break;
                }
            }

        case SQL_RESP_TYPE_REGION: {
                sqlregion row;
                row.countrycode = "";
                row.regionname = "";

                SSqlStatement::result_t result;
                sqlStatement->execute()->getResult ( result );

                if ( !result.empty() ) {
                    dataAvailable = true;
                    row.countrycode = string ( result[0][0] );
                    row.regionname = string ( result[0][1] );
                    sqlResponseData.push_back ( row );
                }

                break;
            }
    }

    return dataAvailable;
}

/**
 * @brief simple function to handle unified way of logging
 * @param urgency indicate the urgency of this message
 * @param message The text to log
 */
inline void GeoSqlBackend::logEntry ( Logger::Urgency urgency, string message )
{
    L << urgency << "geosql " << message << endl;
}

/**
 * @brief GeoSQL class destructor
 */
GeoSqlBackend::~GeoSqlBackend()
{
    delete geoip_db;
    delete pdns_db;
    delete rrs;
    delete geosqlRrs;
    geoip_db = NULL;
    pdns_db = NULL;
    rrs = NULL;
    geosqlRrs = NULL;
}

/**
 * @class GeoSqlFactory
 * @author Shin Sterneck
 * @date 2013
 * @file geosqlbackend.cpp
 * @brief The main BackendFactory for GeoSqlBackend
 */
class GeoSqlFactory : public BackendFactory
{
    public:

        GeoSqlFactory() : BackendFactory ( "geosql" ) {
        }

        /**
         * @brief declares configuration options
         * @param suffix specified the configuration suffix used by PowerDNS
         */
        void declareArguments ( const string &suffix ) {
            // GeoSQL configuration part
            declare ( suffix, "domain-suffix", "Set the domain suffix for GeoSQL zones without prefixed 'dot' character", "geosql" );

            // GeoDB DB Connection part
            declare ( suffix, "geo-host", "The GeoIP Database server IP/FQDN", "localhost" );
            declare ( suffix, "geo-port", "The GeoIP Database server Port", "3306" );
            declare ( suffix, "geo-socket", "The GeoIP Database server socket", "" );
            declare ( suffix, "geo-database", "The GeoIP Database name", "geoip" );
            declare ( suffix, "geo-username", "The GeoIP Database username", "geoip" );
            declare ( suffix, "geo-password", "The GeoIP Database password", "geoip" );
            declare ( suffix, "geo-group", "The GeoIP Database MySQL 'group' to connect as", "client" );
            declare ( suffix, "geo-timeout", "The GeoIP Database transaction timeout in seconds", "10" );
            declare ( suffix, "geo-innodb-read-committed", "Use InnoDB READ-COMMITTED transaction isolation level for the GeoIP Database", "true" );

            // PowerDNS DB Connection part
            declare ( suffix, "pdns-host", "The PowerDNS Database server IP/FQDN", "localhost" );
            declare ( suffix, "pdns-port", "The PowerDNS Database server Port", "3306" );
            declare ( suffix, "pdns-socket", "The PowerDNS Database server socket", "" );
            declare ( suffix, "pdns-database", "The PowerDNS Database name", "pdns" );
            declare ( suffix, "pdns-username", "The PowerDNS Database username", "pdns" );
            declare ( suffix, "pdns-password", "The PowerDNS Database password", "pdns" );
            declare ( suffix, "pdns-group", "The PowerDNS Database MySQL 'group' to connect as", "client" );
            declare ( suffix, "pdns-timeout", "The PowerDNS Database transaction timeout in seconds", "10" );
            declare ( suffix, "pdns-innodb-read-committed", "Use InnoDB READ-COMMITTED transaction isolation level for the PowerDNS Database", "true" );

            // SQL Statements
            declare ( suffix, "sql-pdns-lookuptype", "SQL Statement to retrieve RR types such as A,CNAME,TXT or MX records", "select replace(name, '.{{REGION}}.{{DOMAIN-SUFFIX}}',''), type , replace(content,'.{{REGION}}.{{DOMAIN-SUFFIX}}',''), ttl, prio from records where name='{{QDOMAIN}}.{{REGION}}.{{DOMAIN-SUFFIX}}' and type='{{QTYPE}}' and disabled=0;" );
            declare ( suffix, "sql-pdns-lookuptype-any", "SQL Statement to retrieve the ANY RR type requests", "select replace(name, '.{{REGION}}.{{DOMAIN-SUFFIX}}',''), type, replace(content,'.{{REGION}}.{{DOMAIN-SUFFIX}}',''), ttl, prio from records where name='{{QDOMAIN}}.{{REGION}}.{{DOMAIN-SUFFIX}}' and type != 'SOA' and disabled=0;" );
            declare ( suffix, "sql-geo-lookup-region", "SQL Statement to lookup the REGION and Country Code by source IP address", "select cc,regionname from lookup where MBRCONTAINS(ip_poly, POINTFROMWKB(POINT(INET_ATON('{{S-IP}}'), 0)));" );
            declare ( suffix, "sql-pdns-lookup-geosqlenabled", "SQL Statement to lookup domains, which are enabled for geosql.", "select distinct name from records where name like '%geosql';" );
        }

        /**
         * @brief function to make DNSBackend as documented by PowerDNS
         * @param suffix specified configuration suffix used by PowerDNS
         * @return GeoSqlBackend object
         */
        DNSBackend *make ( const string &suffix ) {
            return new GeoSqlBackend ( suffix );
        }
};

/**
 * @class GeoSqlLoader
 * @author Shin Sterneck
 * @date 2013
 * @file geosqlbackend.cpp
 * @brief The GeoSsqlLoader class to help load the backend itself
 */
class GeoSqlLoader
{
    public:

        /**
         * @brief The backend loader
         */
        GeoSqlLoader() {
            BackendMakers().report ( new GeoSqlFactory );
        }

};

static GeoSqlLoader geosqlloader;
