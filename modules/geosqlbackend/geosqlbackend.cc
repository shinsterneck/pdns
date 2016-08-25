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
GeoSqlBackend::GeoSqlBackend(const string &suffix) {
    // load configuration
    setArgPrefix("geosql" + suffix);
    rrs = new vector<DNSResourceRecord>();
    try {
        // GeoIP Database Connectivity
        geoip_db = new OpenDBX::Conn(getArg("geo-backend"), getArg("geo-host"), "");
        geoip_db->bind(getArg("geo-database"), getArg("geo-username"), getArg("geo-password"), ODBX_BIND_SIMPLE);
        logEntry(Logger::Notice, "Connection successful for GeoIP data. Connected to database '" + getArg("geo-database") + "' on '" + getArg("geo-host") + "'");

        // PowerDNS Database Connectivity
        pdns_db = new OpenDBX::Conn(getArg("pdns-backend"), getArg("pdns-host"), "");
        pdns_db->bind(getArg("pdns-database"), getArg("pdns-username"), getArg("pdns-password"), ODBX_BIND_SIMPLE);
        logEntry(Logger::Notice, "Connection successful for zone data. Connected to database '" + getArg("pdns-database") + "' on '" + getArg("pdns-host") + "'");
    } catch (OpenDBX::Exception &e) {
        ostringstream oss;
        oss << e.getCode();
        throw PDNSException("Connection to database server could not be established! ODBX Error code: " + oss.str());
    }
}

/**
 * @brief Used by PowerDNS for zone transfer purposes
 * @param target Stores the DNS name
 * @param domain_id The Domain ID for the domain in question
 * @param include_disabled Specified whether disabled domains should be included in the response
 * @return false (no support for zone transfers in this backend, use main zone for this purpose for now)
 */
bool GeoSqlBackend::list(const DNSName &target, int domain_id, bool include_disabled) {    
    return false;
}

/**
 * @brief Function for looking up the DNS records and storing the result into a vector.
 * @param qtype The DNS query type
 * @param qdomain The DNS domain name
 * @param pkt_p The DNS Packet
 * @param zoneId The Zone ID
 */
void GeoSqlBackend::lookup(const QType &qtype, const DNSName &qdomain, DNSPacket *pkt_p, int zoneId) {
    const boost::regex regexFilter = boost::regex(getArg("regex-filter"));
    ComboAddress remoteIp;
    
	//check if regex filter is configured and matches the qdomain, if not skip this backend
    if (boost::equals(regexFilter, ".*") ||  boost::regex_search(qdomain.toStringNoDot(), regexFilter)) {
        logEntry(Logger::Debug, "Handling Query Request: '" + string(qdomain.toStringNoDot()) + ":" + string(qtype.getName()) + "'");

        //check for ECS data and use it if found
        if (pkt_p->hasEDNSSubnet()) {
            logEntry(Logger::Debug, "EDNS0 Client-Subnet Field Found!");
            remoteIp = pkt_p->getRealRemote().getNetwork();
        } else {
            remoteIp = pkt_p->getRemote();
        }

        // get region and dns records for that region
        sqlregion region;
        if (getRegionForIP(remoteIp, region)) {
            getGeoDnsRecords(qtype, qdomain.toStringNoDot(), region);
        } else {
            return;
        }
    } else {
        logEntry(Logger::Debug, "Skipping Query request: '" + qdomain.toStringNoDot() + "' not matching regex-filter.");
    }
}

/**
 * @brief Function used by PowerDNS to retrieve the records
 * @param rr Reference containing the individual DNSResourceRecord
 * @return true as long as there are records left in the vector filled by the lookup() function
 */
bool GeoSqlBackend::get(DNSResourceRecord &rr) {
    if (rrs->size() > 0) {
        rr = rrs->at(rrs->size() - 1);
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
bool GeoSqlBackend::getSOA(const DNSName &name, SOAData &soadata, DNSPacket *p) {
    logEntry(Logger::Debug, "Ignoring SOA request: '" + name.toString());
    return false;
}

/**
 * @brief Function to get region
 * @param ip The source IP address from the request packet
 * @param returned_region contains the identifed region information
 * @return bool success or failure indicator
 */
bool GeoSqlBackend::getRegionForIP(ComboAddress &ip, sqlregion &returned_region) {
    bool foundCountry = false;
    string sqlstmt = getArg("sql-geo-lookup-region");    
    boost::replace_all(sqlstmt, "{{S-IP}}", ip.toString());
    std::vector<boost::any> sqlResponseData;
    try {
        if (getSqlData(geoip_db, sqlstmt, sqlResponseData, SQL_RESP_TYPE_REGION)
                && !sqlResponseData.empty()) {
            sqlregion region = boost::any_cast<sqlregion>(sqlResponseData.at(0));
            boost::to_lower(region.regionname);
            boost::to_lower(region.countrycode);
            returned_region = region;
            foundCountry = true;
        }
    } catch (std::exception &e) {
        logEntry(Logger::Critical, "Error while parsing SQL response data for GeoIP region! " + string(e.what()));
    }

    if (foundCountry) {
        string logentry = "Identified as: '" + returned_region.countrycode;
        if (!returned_region.regionname.empty()) {
            logentry.append("|" + returned_region.regionname + "'");        
        } else {
            logentry.append("'");
        }    
        logEntry(Logger::Debug, logentry);
    } else {
        logEntry(Logger::Debug, "No Region Found");
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
bool GeoSqlBackend::getGeoDnsRecords(const QType &type, const string &qdomain, const sqlregion &region) {
    bool foundRecords = false;
    string sqlstmt;

    if (type.getCode() == QType::ANY || type.getCode() == QType::SOA) {
        sqlstmt = getArg("sql-pdns-lookuptype-any");
    } else {
        sqlstmt = getArg("sql-pdns-lookuptype");
    }
    
    boost::replace_all(sqlstmt, "{{DOMAIN-SUFFIX}}", getArg("domain-suffix"));
    boost::replace_all(sqlstmt, "{{QDOMAIN}}", qdomain);
    boost::replace_all(sqlstmt, "{{QTYPE}}", type.getName());

    string sqlstmt_region = sqlstmt;
    string sqlstmt_cc = sqlstmt;

    std::vector<boost::any> sqlResponseData;

    if (!region.countrycode.empty()) {
        boost::replace_all(sqlstmt_region, "{{REGION}}", region.countrycode);
        if (getSqlData(pdns_db, sqlstmt_region, sqlResponseData, SQL_RESP_TYPE_DNSRR )) {    
            foundRecords = true;
        } else if (!region.regionname.empty()) {
            sqlResponseData.clear();
            boost::replace_all(sqlstmt_cc, "{{REGION}}", region.regionname);
            if (getSqlData(pdns_db, sqlstmt_cc, sqlResponseData, SQL_RESP_TYPE_DNSRR )) {
                foundRecords = true;
            }
        }
    }

    if (foundRecords) {
        DNSResourceRecord record;
        record.auth = 1;
        try {
            for (unsigned int i = 0; i < sqlResponseData.size(); i++) {
                record = boost::any_cast<DNSResourceRecord>(sqlResponseData.at(i));
                rrs->push_back(record);
            }
        } catch (std::exception &e) {
            logEntry(Logger::Alert, "Error while parsing SQL response data for DNS Records: " + string(e.what()));
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
bool GeoSqlBackend::getSqlData(OpenDBX::Conn *&conn, string &sqlStatement, std::vector<boost::any> &sqlResponseData, int sqlResponseType) {
    
    bool dataAvailable = false;
    sqlResponseData.clear();
    logEntry(Logger::Debug, "Executing SQL Statement: " + sqlStatement);
    if (!sqlStatement.empty()) {
        try {
            OpenDBX::Result result = conn->create(sqlStatement).execute();
            odbxres stat;

            while ((stat = result.getResult())) {
                switch (stat) {
                    case ODBX_RES_TIMEOUT:
                        throw;
                    case ODBX_RES_NOROWS:
                        logEntry(Logger::Debug, "ODBX_RES_NOROWS");
                        break;
                    case ODBX_RES_DONE:
                        logEntry(Logger::Debug, "ODBX_RES_DONE");
                        break;
                    case ODBX_RES_ROWS:
                        while (result.getRow() != ODBX_ROW_DONE) {
                            switch (sqlResponseType) {
                                case SQL_RESP_TYPE_DNSRR:
                                {
                                    DNSResourceRecord row;
                                                                          
                                    int col_name = result.columnPos("name");
                                    int col_type = result.columnPos("type");
                                    int col_ttl = result.columnPos("ttl");
                                    int col_prio = result.columnPos("prio");
                                    int col_content = result.columnPos("content");
                                    
                                    row.qname = DNSName(string(result.fieldValue(col_name)));
                                    row.qtype = string(result.fieldValue(col_type));
                                    row.ttl = stoul(string(result.fieldValue(col_ttl)), nullptr, 10);
                                    if (row.qtype == QType::MX || row.qtype == QType::SRV) {
                                        row.content = string(result.fieldValue(col_prio)) + " " + string(result.fieldValue(col_content));
                                    } else {
                                        row.content = string(result.fieldValue(col_content));
                                    }
                                    if (!row.qname.empty()) {
                                        dataAvailable = true;
                                        sqlResponseData.push_back(row);
                                    }
                                    break;
                                }
                                
                                case SQL_RESP_TYPE_REGION:
                                {
                                    sqlregion row;
                                    row.countrycode = "";
                                    row.regionname = "";
                                    int col_region = result.columnPos("regionname");
                                    int col_cc = result.columnPos("cc");
                                    if (result.fieldLength(col_region > 0)) {
                                        row.regionname = string(result.fieldValue(col_region));
                                        dataAvailable = true;
                                    }

                                    if (result.fieldLength(col_cc > 0)) {
                                        row.countrycode = string(result.fieldValue(col_cc));
                                        dataAvailable = true;
                                    }
                                    if (dataAvailable) sqlResponseData.push_back(row);
                                break;
                                }
                            }
                        }
                }
                continue;
            }

        } catch (OpenDBX::Exception &e) {
            throw PDNSException("geosql Caught OpenDBX exception during SQL Statement: " + string(e.what()));
        }
    }

    return dataAvailable;
}

/**
 * @brief simple function to handle unified way of logging
 * @param urgency indicate the urgency of this message
 * @param message The text to log
 */
inline void GeoSqlBackend::logEntry(Logger::Urgency urgency, string message) {
    L << urgency << "geosql " << message << endl;
}

/**
 * @brief GeoSQL class destructor
 */
GeoSqlBackend::~GeoSqlBackend() {
    geoip_db->unbind();
    geoip_db->finish();
    pdns_db->unbind();
    pdns_db->finish();
    delete geoip_db;
    delete pdns_db;
    delete rrs;
    geoip_db = NULL;
    pdns_db = NULL;
    rrs = NULL;
}

/**
 * @class GeoSqlFactory
 * @author Shin Sterneck
 * @date 2013
 * @file geosqlbackend.cpp
 * @brief The main BackendFactory for GeoSqlBackend
 */
class GeoSqlFactory : public BackendFactory {
public:

    GeoSqlFactory() : BackendFactory("geosql") {
    }

    /**
     * @brief declares configuration options
     * @param suffix specified the configuration suffix used by PowerDNS
     */
    void declareArguments(const string &suffix) {
        // GeoSQL configuration part
        declare(suffix, "regex-filter", "Regular expression filter to match against queried domain", ".*");
        declare(suffix, "domain-suffix", "Set the domain suffix for GeoSQL zones without prefixed 'dot' character", "geosql");

        // GeoDB DB Connection part
        declare(suffix, "geo-backend", "The backend is the name of the driver the OpenDBX library should use to connect to a database. The name must be one of the implemented and available drivers on the system and is case sensitive. All driver names are in lower case, e.g. mysql", "mysql");
        declare(suffix, "geo-host", "The GeoIP Database server IP/FQDN", "localhost");
        declare(suffix, "geo-database", "The GeoIP Database name", "geoip");
        declare(suffix, "geo-username", "The GeoIP Database username", "geoip");
        declare(suffix, "geo-password", "The GeoIP Database password", "geoip");

        // PowerDNS DB Connection part
        declare(suffix, "pdns-backend", "The backend is the name of the driver the OpenDBX library should use to connect to a database. The name must be one of the implemented and available drivers on the system and is case sensitive. All driver names are in lower case, e.g. mysql", "mysql");
        declare(suffix, "pdns-host", "The PowerDNS Database server IP/FQDN", "localhost");
        declare(suffix, "pdns-database", "The PowerDNS Database name", "pdns");
        declare(suffix, "pdns-username", "The PowerDNS Database username", "pdns");
        declare(suffix, "pdns-password", "The PowerDNS Database password", "pdns");

        // SQL Statements
        declare(suffix, "sql-pdns-lookuptype", "SQL Statement to retrieve RR types such as A,CNAME,TXT or MX records", "select replace(name, '.{{REGION}}.{{DOMAIN-SUFFIX}}','') as name, type , replace(content,'.{{REGION}}.{{DOMAIN-SUFFIX}}','') as content, ttl, prio from records where name='{{QDOMAIN}}.{{REGION}}.{{DOMAIN-SUFFIX}}' and type='{{QTYPE}}' and disabled=0;");       
        declare(suffix, "sql-pdns-lookuptype-any", "SQL Statement to retrieve the ANY RR type requests", "select replace(name, '.{{REGION}}.{{DOMAIN-SUFFIX}}','') as name, type, replace(content,'.{{REGION}}.{{DOMAIN-SUFFIX}}','') as content, ttl, prio from records where name='{{QDOMAIN}}.{{REGION}}.{{DOMAIN-SUFFIX}}' and type != 'SOA' and disabled=0;");
        declare(suffix, "sql-geo-lookup-region", "SQL Statement to lookup the REGION and Country Code by source IP address", "select cc,regionname from lookup where MBRCONTAINS(ip_poly, POINTFROMWKB(POINT(INET_ATON('{{S-IP}}'), 0)));");
    }

    /**
     * @brief function to make DNSBackend as documented by PowerDNS
     * @param suffix specified configuration suffix used by PowerDNS
     * @return GeoSqlBackend object
     */
    DNSBackend *make(const string &suffix) {
        return new GeoSqlBackend(suffix);
    }
};

/**
 * @class GeoSqlLoader
 * @author Shin Sterneck
 * @date 2013
 * @file geosqlbackend.cpp
 * @brief The GeoSsqlLoader class to help load the backend itself
 */
class GeoSqlLoader {
public:

    /**
     * @brief The backend loader
     */
    GeoSqlLoader() {
        BackendMakers().report(new GeoSqlFactory);
    }

};

static GeoSqlLoader geosqlloader;
