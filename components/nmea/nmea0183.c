/*
 * Copyright (C) 2015 - 2020, IBEROXARXA SERVICIOS INTEGRALES, S.L.
 * Copyright (C) 2015 - 2020, Jaume Olivé Petrus (jolive@whitecatboard.org)
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the <organization> nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *     * The WHITECAT logotype cannot be changed, you can remove it, but you
 *       cannot change it in any way. The WHITECAT logotype is:
 *
 *          /\       /\
 *         /  \_____/  \
 *        /_____________\
 *        W H I T E C A T
 *
 *     * Redistributions in binary form must retain all copyright notices printed
 *       to any local or remote output device. This include any reference to
 *       Lua RTOS, whitecatboard.org, Lua, and other copyright notices that may
 *       appear in the future.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Lua RTOS, NMEA parser
 *
 */

#include "sdkconfig.h"

#if CONFIG_LUA_RTOS_USE_SENSOR_GPS

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <sys/syslog.h>

static char *GPGGA = "GPGGA";  // GPGGA sentence string
static char *GNGGA = "GNGGA";  // GNGGA sentence string
//static char *GPRMC = "GPRMC";  // GPRMC sentence string
//static int  date_updated = 0;  // 0 = date is not updated yet, 1 = updated

// Last position data
static double lat, lon, height;
static int sats;
static int new_pos = 0;

// As parser computes checksum incrementally, this function computes
// the checksum that corresponds to the nma sentence string
static int nmea_initial_checksum(char *c) {
    int computed_checksum = 0;

    while (*c) {
        computed_checksum ^= *c;
        c++;
    }

    computed_checksum ^= ',';

    return computed_checksum;
}

// Converts a geolocation expressed in � ' '' to decimal
double nmea_geoloc_to_decimal(char *token) {
    double val 	   = 0.0;
    double degrees = 0.0;
    double minutes = 0.0;
    double seconds = 0.0;
    int num_decimals = 0;
    char *current = token;

    while (*current) {
        if (*current == '.') {
            // xxxx.xxxx
            //     |
            *current = 0; // Add end string for get minutes

            // Go to beginning of minutes
            // Get minutes value
            current--;
            current--;

            //minutes = atof(current);
            minutes = (double)atoi(current);

            // xxxx.xxxx
            //   |
            *current = 0; // Add end string for get degrees

            //degrees = atof(token);
            degrees = (double)atoi(token);

            // Get second value
            current++;
            *current = '0';  // Put 0 for convert seconds from ascci to double
            current++;	     // Put . for convert seconds from ascci to double
            *current = '.';
            current--;

            //seconds = atof(current);

            current++;
            current++;
            num_decimals = strlen(current);

            seconds = (double)atoi(current) / (double)pow((double)10, (double)num_decimals);

            // Do the conversion
            val = degrees + ((minutes + seconds) / 60);

            // Exit while
            break;
        }
        current++;
    }

    return val;
}

// Global Positioning System Fix Data
// $GXGGA,hhmmss.ss,llll.ll,a,yyyyy.yy,a,x,xx,x.x,x.x,M,x.x,M,x.x,xxxx*hh
//
// 0    = UTC of Position
// 1    = Latitude
// 2    = N or S
// 3    = Longitude
// 4    = E or W
// 5    = GPS quality indicator (0=invalid; 1=GPS fix; 2=Diff. GPS fix)
// 6    = Number of satellites in use [not those in view]
// 7    = Horizontal dilution of position
// 8    = Antenna altitude above/below mean sea level (geoid)
// 9    = Meters  (Antenna height unit)
// 10   = Geoidal separation (Diff. between WGS-84 earth ellipsoid and
//        mean sea level.  -=geoid is below WGS-84 ellipsoid)
// 11   = Meters  (Units of geoidal separation)
// 12   = Age in seconds since last update from diff. reference station
// 13   = Diff. reference station ID#
// 14   = Checksum
static void nmea_GXGGA(char *sentence, char * keyword) {
    int    valid = 1;

    int seq = 0;      // Current nmea sentence field (0 = first after nmea command)
    char *c;
    char *token;
    int checksum = 0;
    int computed_checksum = nmea_initial_checksum(keyword);

    new_pos = 0;

    token = c = sentence;
    while (*c) {
        if (*c == ',') {
            computed_checksum ^= *c;

            *c++ = 0;

            if (seq == 1) { // Latitude
                lat = nmea_geoloc_to_decimal(token);
            }

            if (seq == 2) { // N or S
                if (strcmp("S",token) == 0) {
                    lat = lat * -1;
                }
            }

            if (seq == 3) { // Longitude
                lon = nmea_geoloc_to_decimal(token);
            }

            if (seq == 4) { // E or W
                if (strcmp("W",token) == 0) {
                    lon = lon * -1;
                }
            }

            if (seq == 5) {	// GPS quality indicator (0=invalid; 1=GPS fix; 2=Diff. GPS fix)
                int quality = atoi(token);

                valid = ((quality == 1) || (quality == 2));
            }

            if (seq == 6) {	// Number of satellites in use [not those in view]
                sats = atoi(token);
            }
            
	    if (seq == 8) {	//height
                height = strtod(token, NULL);
            }


            token = c;
            seq++;
        } else {
            if (*c == '*') {
                c++;
                token = c;
                break;
            } else {
                computed_checksum ^= *c;
                c++;
            }
        }
    }

    checksum = (int)strtol(token, NULL, 16);

    if (checksum == computed_checksum) {
        if (valid) {
            new_pos = 1;
        }
    }
}

#if 0
// Recommended minimum specific GPS/Transit data
// $GPRMC,081836,A,3751.65,S,14507.36,E,000.0,360.0,130998,011.3,E*62
//
// 1    = UTC of position fix
// 2    = Data status (V=navigation receiver warning)
// 3    = Latitude of fix
// 4    = N or S
// 5    = Longitude of fix
// 6    = E or W
// 7    = Speed over ground in knots
// 8    = Track made good in degrees True
// 9    = UT date
// 10   = Magnetic variation degrees (Easterly var. subtracts from true course)
// 11   = E or W
// 12   = Checksum
static void nmea_GPRMC(char *sentence) {
    int seq = 0;
    char *c;
    char *token;

    int checksum = 0;
    int computed_checksum = nmea_initial_checksum(GPRMC);

    char  tmp[3];
    int  month,day,hours,minutes,seconds;
    uint16_t year;

    token = c = sentence;
    while (*c) {
        if (*c == ',') {
            computed_checksum ^= *c;

            *c++ = 0;

            if (seq == 0) {	// UTC of position fix
                tmp[0] = *(token + 0);
                tmp[1] = *(token + 1);
                tmp[2] = 0;
                hours = atoi(tmp);

                tmp[0] = *(token + 2);
                tmp[1] = *(token + 3);
                tmp[2] = 0;
                minutes = atoi(tmp);

                tmp[0] = *(token + 4);
                tmp[1] = *(token + 5);
                tmp[2] = 0;
                seconds = atoi(tmp);
            }

            if (seq == 8) {	// UT date
                tmp[0] = *(token + 0);
                tmp[1] = *(token + 1);
                tmp[2] = 0;
                day = atoi(tmp);

                tmp[0] = *(token + 2);
                tmp[1] = *(token + 3);
                tmp[2] = 0;
                month = atoi(tmp);

                tmp[0] = *(token + 4);
                tmp[1] = *(token + 5);
                tmp[2] = 0;
                year = atoi(tmp);
            }

            token = c;
            seq++;
        } else {
            if (*c == '*') {
                c++;
                token = c;
                break;
            } else {
                computed_checksum ^= *c;
                c++;
            }
        }
    }

    checksum = (int)strtol(token, NULL, 16);
    if (checksum == computed_checksum) {
        if (year >= 15) {
            date_updated = 1;

            time_t newtime;
            struct tm tms;

            memset(&tms, 0, sizeof(struct tm));

            tms.tm_hour = hours;
            tms.tm_min = minutes;
            tms.tm_sec = seconds;
            tms.tm_year = year;
            tms.tm_mon = month;
            tms.tm_mday = day;
            newtime = mktime(&tms);

            settimeofday((const time_t *)&newtime);
            syslog(LOG_DEBUG, "nmea01843 setting system time to %d-%d-%d %d:%d:%d",year,month,day,hours,minutes,seconds);
        }
    }
}
#endif

void nmea_parse(char *sentence) {
    char *c;
    char *token;

    token = c = sentence;
    while (*c) {
        if ((*c == ',') || (*c == '*')) {
            *c++ = 0;

            // Skip $
            if (*token == '$') {
                token++;
            }

            if (strncmp(GPGGA, token, 5) == 0) {
                nmea_GXGGA(c, GPGGA);
            //} else if ((strncmp(GPRMC, token, 5) == 0) && (!date_updated)) {
            //    nmea_GPRMC(c);
	    } else if (strncmp(GNGGA, token, 5) == 0) {
		    nmea_GXGGA(c, GNGGA);
            } else {
                break;
            }

            token = c;
        } else {
            c++;
        }
    }
}

int nmea_new_pos() {
    return new_pos;
}

double nmea_lon() {
    return lon;
}

double nmea_lat() {
    return lat;
}

int nmea_sats() {
    return sats;
}

double nmea_height() {
    return height;
}

#endif
