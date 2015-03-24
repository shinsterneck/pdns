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

#include "geosqlbackend.h"

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
        throw PDNSException("Connection to database server could not be established! ODBX Error code: " + e.getCode());
    }
}

// no support for zone transfers in this backend, use main zone for this purpose for now
bool GeoSqlBackend::list(const string &target, int id, bool include_disabled) {    
    return false;
}

// Function for looking up the DNS records and storing the result into a vector.
void GeoSqlBackend::lookup(const QType &type, const string &qdomain, DNSPacket *p, int zoneId) {
    const string qdomainLower = boost::to_lower_copy<string>(qdomain);
    const boost::regex regexFilter = boost::regex(getArg("regex-filter"));    
    string remoteIp;
    
	//check for ECS data and use it if found
    if (p->hasEDNSSubnet()) {
        logEntry(Logger::Debug, "EDNS0 Client-Subnet Field Found!");
        remoteIp = p->getRealRemote().toStringNoMask();
    } else {
        remoteIp = p->getRemote();
    }

	//check if regex filter is configured and matches the qdomain, if not skip this backend
    if (boost::equals(regexFilter, ".*") ||  boost::regex_search(qdomainLower, regexFilter)) {
        logEntry(Logger::Notice, "Handling Query Request: '" + string(qdomainLower) + ":" + string(type.getName()) + "'");
        sqlregion region;
        if (getRegionForIP(remoteIp, region)) {
            getGeoDnsRecords(type, qdomainLower, region);
        } else {
            return;
        }
    } else {
        logEntry(Logger::Debug, "Skipping Query request: '" + qdomainLower + "' not matching regex-filter.");
    }
}

bool GeoSqlBackend::get(DNSResourceRecord &rr) {
    if (rrs->size() > 0) {
        rr = rrs->at(rrs->size() - 1);
        rrs->pop_back();
        return true;
    }

    return false;
}

bool GeoSqlBackend::getSOA(const string &name, SOAData &soadata, DNSPacket *p) {
    return false;
}

// Function to get region
bool GeoSqlBackend::getRegionForIP(string &ip, sqlregion &returned_region) {
    bool foundCountry = false;
    string sqlstmt = getArg("sql-geo-lookup-region");    
    boost::replace_all(sqlstmt, "{{S-IP}}", ip);
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
        logEntry(Logger::Notice, logentry);
    } else {
        logEntry(Logger::Notice, "No Region Found");
    }
    return foundCountry;
}

// retrieve the DNS records according to the geographic location (assigned by region field)
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

//unified way of handling sql queries and storing the result into a vector
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
                                    std::map<string,string> column_map;
                                    const string dbcolumns[] = {"name" , "type", "content", "ttl", "prio"};

                                    for ( unsigned int i = 0 ; i < sizeof(dbcolumns) / sizeof(dbcolumns[0]) ; i++ ) {
                                        if ( result.fieldLength(result.columnPos(dbcolumns[i])) > 0 ) {
                                            column_map[dbcolumns[i]] = ostringstream(result.fieldValue(result.columnPos(dbcolumns[i]))).str();
                                        }
                                    }

                                    row.qname = column_map["name"];
                                    row.qtype = column_map["type"];
                                    row.ttl = lexical_cast<uint32_t>(column_map["ttl"]);
                                    if (row.qtype == QType::MX || row.qtype == QType::SRV) {
                                        row.content = (column_map["prio"] + " " + column_map["content"]);
                                    } else {
                                        row.content = column_map["content"];
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
                                        row.regionname = ostringstream(result.fieldValue(col_region)).str();
                                        dataAvailable = true;
                                    }

                                    if (result.fieldLength(col_cc > 0)) {
                                        row.countrycode = ostringstream(result.fieldValue(col_cc)).str();
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

// simple function to handle unified way of logging
inline void GeoSqlBackend::logEntry(Logger::Urgency urgency, string message) {
    L << urgency << "geosql " << message << endl;
}

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

class GeoSqlFactory : public BackendFactory {
public:

    GeoSqlFactory() : BackendFactory("geosql") {
    }

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

    DNSBackend *make(const string &suffix) {
        return new GeoSqlBackend(suffix);
    }
};

class GeoSqlLoader {
public:

    GeoSqlLoader() {
        BackendMakers().report(new GeoSqlFactory);
        L << Logger::Debug << "Starting new GeoSQL Backend!" << endl;
    }

};

static GeoSqlLoader geosqlloader;
