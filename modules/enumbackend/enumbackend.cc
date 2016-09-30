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

    L << Logger::Debug << "[enum] Creating new backend" << endl;
    rrs = new vector<DNSResourceRecord>();

    ldap = new PowerLDAP ( getArg ( "ldap-servers" ), LDAP_PORT, mustDo ( "ldap-starttls" ) );
    ldap->setOption( LDAP_OPT_DEREF, LDAP_DEREF_ALWAYS );
    ldap->bind ( getArg ( "ldap-binddn" ), getArg ( "ldap-password" ), LDAP_AUTH_SIMPLE, getArgAsNum ( "ldap-timeout" ) );

}

void EnumBackend::lookup ( const QType &qtype, const DNSName &qdomain, DNSPacket *pkt_p, int zoneId )
{

    if ( boost::algorithm::ends_with ( qdomain.toStringNoDot(), getArg ( "domain-suffix" ) ) ) {

        ComboAddress remoteIp;

        //check for ECS data and use it if found
        if ( pkt_p->hasEDNSSubnet() ) {
            remoteIp = pkt_p->getRealRemote().getNetwork();

        } else {
            remoteIp = pkt_p->getRemote();
        }

        L << Logger::Debug << "[enum] " << "Handling Query Request: '" << qdomain.toStringNoDot() << ":" << qtype.getName() << endl;

        // remove domain suffix
        string ds = getArg ( "domain-suffix" );
        string e164_tn = qdomain.toStringNoDot();

        if ( e164_tn.size() != ds.size() ) {

            std::stringstream ldap_searchstring;

            L << Logger::Debug << "[enum] Starting domain translation" << e164_tn << endl;
            e164_tn.erase ( e164_tn.size() - ds.size(), ds.size() );

            // create ldap search pattern ( this is temporary unindex way, slow! )
            ldap_searchstring << "+";

            // remove everything except numbers (pdns also adds *, which for this backend does not make sense)
            e164_tn.resize (
                remove_if (
            e164_tn.begin(), e164_tn.end(), [] ( char x ) {
                return !isdigit ( x );
            }
                ) - e164_tn.begin() );

            if ( e164_tn.size() != 0 ) {
                reverse ( e164_tn.begin(), e164_tn.end() );
                ldap_searchstring << e164_tn;
                L << Logger::Debug << "[enum] Translated Number: " << e164_tn << endl;

                ldap_msgid = ldap->search ( getArg ( "ldap-basedn" ), LDAP_SCOPE_SUB, "telephoneNumber=" + ldap_searchstring.str(), ( const char** ) ldap_attr );
                ldap->getSearchEntry ( ldap_msgid, ldap_result );

                // check if we found something
                if ( ldap_result.count ( "distinguishedName" ) && !ldap_result["distinguishedName"].empty() ) {
                    DNSResourceRecord record;
                    record.qname = qdomain;
                    record.qtype = QType::NAPTR;
                    string naptr_proto = getArg("naptr-proto");
                    record.content = "20 10 \"U\" \"E2U+" + naptr_proto + "\" \"\" " + naptr_proto + ":" + e164_tn + "@" + getArg("naptr-hostname");
                    record.auth = 1;
                    record.ttl = getArgAsNum("naptr-ttl");
                    record.domain_id = 1;

                    L << Logger::Debug << "[enum] Pushing: " << record.content << endl;
                    rrs->push_back ( record );

                    // add a TXT record for convinience purposes
                    record.qtype = QType::TXT;
                    record.content = ldap_result["distinguishedName"][0];
                    rrs->push_back ( record );
                }

            } else {
                L << Logger::Debug << "[enum] No number to translate, skipping query" << endl;
            }
        }
    }
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
    if ( mustDo ( "soa-enable" ) ) {
        L << Logger::Debug << "[enum] Generating SOA record" << endl;
        const string domainSuffix = getArg ( "domain-suffix" );

        if ( std::equal ( domainSuffix.rbegin(), domainSuffix.rend(), name.toStringNoDot().rbegin() ) ) {
            soadata.domain_id = 1;
            soadata.qname = DNSName ( domainSuffix );
            soadata.serial = getArgAsNum("soa-serial");
            soadata.refresh = getArgAsNum("soa-refrelsh");
            soadata.retry = getArgAsNum("soa-retry");
            soadata.expire = getArgAsNum("soa-expiry");
            soadata.ttl = getArgAsNum("soa-ttl");
            soadata.hostmaster = DNSName ( getArg("soa-hostmaster") );
            soadata.nameserver = DNSName ( getArg("soa-nameserver") );
            return true;
        }
    } else {
        L << Logger::Debug << "[enum] SOA record generation disabled" << endl;
    }

    return false;
}

bool EnumBackend::list ( const DNSName &target, int domain_id, bool include_disabled )
{
    return false;
}

EnumBackend::~EnumBackend()
{
    delete rrs;
    rrs = NULL;

    delete ldap;
    ldap = NULL;

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

            // LDAP Configuration
            declare ( suffix, "ldap-servers", "List of LDAP hosts (separated by spaces)" , "ldap://127.0.0.1:389/" );
            declare ( suffix, "ldap-starttls" , "Bind to LDAP Server using TLS" , "no" );
            declare ( suffix, "ldap-username", "Set the LDAP username" , "user" );
            declare ( suffix, "ldap-password", "Set the LDAP password" , "pass" );
            declare ( suffix, "ldap-basedn", "Search root in ldap tree (must be set)", "" );
            declare ( suffix, "ldap-binddn", "User dn for non anonymous binds", "" );
            declare ( suffix, "ldap-timeout", "Seconds before connecting to server fails", "5" );
            declare ( suffix, "ldap-method", "How to search entries (simple, strict or tree)", "simple" );

            // SOA Configuration
            declare ( suffix, "soa-enable" , "This backend should generate SOA record (yes or no)" , "no" );
            declare ( suffix, "soa-hostmaster" , "Define SOA hostmaster of this backend/zone" , "hostmaster.example.com" );
            declare ( suffix, "soa-nameserver" , "Define SOA nameserver of this backend/zone" , "ns1.example.com" );
            declare ( suffix, "soa-serial" , "Define SOA serial number" , "2016103001" );
            declare ( suffix, "soa-ttl" , "Define SOA TTL" , "300" );
            declare ( suffix, "soa-refresh" , "Define SOA refresh time" , "10800" );
            declare ( suffix, "soa-expiry" , "Define SOA expiry time" , "1209600" );
            declare ( suffix, "soa-retry" , "Define SOA retry time" , "3600" );

            // NAPTR Configuration
            declare ( suffix, "naptr-ttl" , "Define NAPTR TTL" , "300" );
            declare ( suffix, "naptr-proto" , "Define protocol as h323 or sip" , "h323" );
            declare ( suffix, "naptr-hostname" , "Define static hostname to use in record content" , "gw1.example.com");
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
