GeoSQL Backend for PowerDNS is copyright Ⓒ 2013-2015 Shin Sterneck

This README is writting in MarkDown format and is currently under 
work-in-progress. Last Update: Jan, 2015


Table of Contents
-----------------

1. [Introduction](#introduction])
    * [How does it work](#how-does-it-work)
    * [Overlay Concept](#overlay-concept)
    * [Special RR Records](#special-rr-records)
2. [Basic Setup](#basic-setup)
    * [Requirements](#requirements)
    * [Preparing the GeoIP Database](#preparing-the-geoip-database)
    * [DNS Database](#dns-database)
4. [Database Schema Requirements](#database-schema-requirements)
5. [Feature and Change Requests](#feature-and-change-requests)
6. [ToDo](#todo)
7. [Other Notes](#other-notes)


Introduction
============

The GeoSQL backend for PowerDNS enables you to distribute your services
globally and direct your users the nearest server. This is achieved by 
adding location awareness to PowerDNS and maintaining regional DNS 'views'.

One goal for the GeoSQL backend module is to keep the administration and maintenance as 
simple as possible by centralizing the data into an SQL database and thus allow the domain/zone maintainers to use existing powerdns 
administration tools, such as [PowerAdmin](http://www.poweradmin.org) to 
manage GeoSQL enabled domains without the need to maintain local mapping files on the
powerdns server. The GeoSQL module uses "IP Address to Location" mapping data stored inside an SQL Database.

GeoIP enabled DNS records are stored in the same database table and format as PowerDNS's gmysql backend,
and basically appear exactly like any other domain.

Support for "EDNS0 Client-Subnet Extension" helps best identify the client region even when behind a
CDN or other DNS servers (if supported by the provider), such as Google's Public DNS service.

###How does it work?

Since the domain information is stored in the same SQL table (just like any other DNS
domain), an identifier is added to the domain name's that are GeoSQL enabled:


1. **The administrator, groups countries together into "regions"**

    For example, if you want to create a zone specific for asian countries you 
    could group all asian countries together and assign them to the region "asia".
    Alternatively the country itself can be used directly without adding it to groups.

2. **All GeoIP enabled zones have a suffix, which is by default "geosql"**

    All records/zones/domains that end with ".geosql" are handled by
    the GeoSQL backend module. This effectively creates "views" for each region.


As such, GeoIP enabled domains or views have the following format:


|           Format                  |
|:---------------------------------:|
| [domain-name].[REGION].[SUFFIX]   |


**When PowerDNS receives a query from a client:**

**1.** It forwards the request to the GeoSQL module.

**2.** GeoSQL tries to identify the visitors geographic location and appends 
       the identified REGION and SUFFIX text to the request. This results in a new 
       "temporary" request: [requested-domain-name].[REGION].[SUFFIX]

**3.** GeoSQL then retrieves the DNS records database for such records.

**4.** When GeoSQL receives a response from the database, it removes the 
       [REGION] and [SUFFIX] from the result.

**5.** Finally it returns the "real" DNS response back to PowerDNS for further 
       processing.

All you need in the PowerDNS database is a domain entry for each REGION,
with its own records just like in any other domain.

---

As an example, lets say your domain name is called 'example.com' and you
created 'europe', 'asia' and 'northamerica' REGIONs:

| Actual Domain Name | [REGION]        | [SUFFIX] |
|:-------------------|:----------------|:---------|
|example.com         | europe          | geosql   |
|example.com         | asia            | geosql   |
|example.com         | northamerica    | geosql   |

Your domain within PowerDNS would then look like this:

| Zone Name in PowerDNS's SQL DB   | Description                    |
|:---------------------------------|:-------------------------------|
| example.com                      | main or default domain         |
| example.com.europe.geosql        | view for european region       |
| example.com.asia.geosql          | view for asian region          |
| example.com.northamerica.geosql  | view for north american region |

The first 'example.com' domain would act as the default domain and is actually not
part of the GeoSQL backend. The remaining domains, containing the REGION's text, are
geographic specific views, containig DNS records specific to the geographic 
location such as A, CNAME, TXT, MX records or even NS records.

---

Once you have these views, you can simple maintain DNS records within them, just
like with any other domains within PowerDNS. The only important issue to
take care of is that the [SUFFIX] is not a TLD (Top Level Domain) such as 
'com', 'org' or 'net' to prevent conflicts with real domains,
however this is not enforced and if you wanted to, you could.


###Overlay Concept

GeoSQL is designed to be used as an overlay module, this means that GeoSQL needs to be used 
together with the gmysql backend or another backend that stores its data within an SQL Database.

This also means that you only need to create DNS records, which you want to be 
different depending on the geographic location of the visitor.
All other records are handled by other PowerDNS backends, such as the 
gmysql backend.

If you have DNS records that are different in each geographic location and 
other records that are same across all geographic regions, there is no need to
create the common ones again in all GeoSQL views. Simple create them once in 
the main domain and the differing entries in the .geosql suffixed view.
This makes maintenance extremly easy and allows for any DNS resource record to
be used with GeoSQL. It therefore eliminates the use of CNAME redirects or other
types of tricks.


---

**Example:**

Lets say you have the following A records in the 'example.com' zone/domain:

Domain **"example.com"** for **all** visitors worldwide

| Host Entry       | A record       |
|:-----------------|:-------------- |
| www.example.com  | 203.0.113.100  |
| test.example.com | 203.0.113.200  |

Domain **"example.com.asia.geosql"** specific for **asian** visitors (asian view):

| Host Entry       | A record       |
|:-----------------|:-------------- |
| www.example.com  | 192.0.2.100    |

Domain **"example.com.europe.geosql"** specific for **european** visitors (european view):

| Host Entry       | A record       |
|:-----------------|:-------------- |
| www.example.com  | 198.51.100.100 |

**The final combined result would look as follows:**


**Asian** visitors would see the following records:

| Host Entry       | Response       | Handled by     |
|:-----------------|:---------------|:---------------|
| www.example.com  | 192.0.2.100    | GeoSQL backend |
| test.example.com | 203.0.113.200  | Other backend  |

**European** visitors would see the following records:

| Request          | Response       | Handled by     |
|:-----------------|:---------------|:---------------|
| www.example.com  | 198.51.100.100 | GeoSQL backend |
| test.example.com | 203.0.113.200  | Other backend  |


In the above example you can see that the 'test.example.com' entry is shown even though the asian or
european views do not include this entry. You can think of this as a type of
inheritance. The 'test.example.com' entry is inherited by the geoip enabled domains
and can be used there or overwritten. But what actually happens, is that after
PowerDNS has processed the GeoSQL response, it continues on with the next
backend, until all relevant backends have been queried (this logic
is handled by PowerDNS). Once all data is available, PowerDNS responds to the 
client with the combined DNS response.

---

> Note that in the event the visitor can't be mapped to a REGION, such as in the case 
> that no region has been configured for the identified location, GeoSQL will
> assign the actual country code it found for that IP. If you have a domain configured with the country code
> (e.g.: example.com.us.geosql) where 'US' is the country code in the GeoIP database, it will be used instead.
> If GeoSQL can not find a region or a country code, it will simply report that it could
> not find any records back to PowerDNS, which in turn may process the request with other backends.

Note that GeoSQL does not support slave operations, but you can overlay a 
slave zone with GeoSQL enabled records. 

You can use any type of DNS RR records supported by PowerDNS in GeoSQL.


Special RR records
------------------

The main DNS dommain will be used for SOA record queries! The GeoSQL module will
skip any SOA inside a .geosql domain. This is because it is intended as 
an overlay zone on top of an already existing 'main' domain. This may change in the future.


Setup
=====

The following instruction can be used a a basic setup to get GeoSQL running: 

GeoSQL depends on two data sources:

1. The GeoIP Database containing IP Address Ranges and their assigned Location
2. The PowerDNS DNS Records Database, containing your DNS records.


Requirements
------------

GeoSQL currently requires the following libraries:

+ OpenDBX Library (build with MySQL support)
+ Boost ( including Regex Library)


###Preparing the GeoIP Database


1. To prepare the database, we will first need to download the GeoIP Data.
There are several free and commercial ready to download collections available,
however for this tutorial we will be using the MaxMind GeoLite Database in CSV
format, which is freely available on their 
[homepage](http://dev.maxmind.com/geoip/legacy/geolite) and is regularly 
updated.

2. Once you have downloaded the GeoIP data, we'll have to convert or import 
it into our MySQL server. Vincent de Lau has written up a nice 
[how-to](http://vincent.delau.net/php/geoip.html) on how to accomplish this.
We will use this as reference with a slight addition to handle our 'region' grouping:


Create Temporary import table:

```
CREATE TABLE csv (
        start_ip CHAR(15) NOT NULL,
        end_ip CHAR(15) NOT NULL,
        start INT UNSIGNED NOT NULL,
        end INT UNSIGNED NOT NULL,
        cc CHAR(2) NOT NULL,
        cn VARCHAR(50) NOT NULL
);
```

Create "CountryCode" table and "IP Address" table as well as our view to combine the two:

```
CREATE TABLE cc (
  ci TINYINT UNSIGNED NOT NULL PRIMARY KEY AUTO_INCREMENT,
  cc CHAR(2) NOT NULL,
  cn VARCHAR(50) NOT NULL,
  region VARCHAR(50)
);

CREATE TABLE ip (
  start INT UNSIGNED NOT NULL,
  end INT UNSIGNED NOT NULL,
  ci TINYINT UNSIGNED NOT NULL
);

CREATE VIEW maxmind AS SELECT
ip.start AS 'start',
   ip.end AS 'end',
   cc.cc AS 'cc',
   cc.region AS 'region' 
   FROM
      ip JOIN cc ON ip.ci = cc.ci;

```

###DNS Database

GeoSQL by default uses the standard powerdns gmysql database schema as 
documented in the gmysql module documentation online.


###PowerDNS Configuration Requirements

Todo: Add PowerDNS Configuration Requirements


Feature and Change Requests
---------------------------

Please send change and feature request to "geosql at sterneck dot asia".


TODO
----

Todo: Add PowerDNS Configuration Requirements


Other Notes
-----------

Please drop me an email or leave some feedback if this backend is useful to 
you.


Thank you!

Shin Sterneck (shin st sterneck dot asia)
