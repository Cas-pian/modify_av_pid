/*****************************************************************************
 * decode_mpeg.c: MPEG decoder example
 *----------------------------------------------------------------------------
 * Copyright (C) 2001-2011 VideoLAN
 * $Id: modify_av_pid.c,v 1.1 2014/06/30 08:10:47 wanggm Exp $
 *
 * Authors: Jean-Paul Saman <jpsaman #_at_# m2x dot nl>
 *          Arnaud de Bossoreille de Ribou <bozo@via.ecp.fr>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *----------------------------------------------------------------------------
 *
 *****************************************************************************/

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#ifdef HAVE_SYS_TIME_H
#define HAVE_GETTIMEOFDAY 1
#include <sys/time.h>
#endif
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>

#include <getopt.h>

#include "connect.h"

#if defined(HAVE_INTTYPES_H)
#include <inttypes.h>
#elif defined(HAVE_STDINT_H)
#include <stdint.h>
#endif

/* The libdvbpsi distribution defines DVBPSI_DIST */
//#ifdef DVBPSI_DIST
//#include "../src/dvbpsi.h"
//#include "../src/psi.h"
//#include "../src/tables/pat.h"
//#include "../src/descriptor.h"
//#include "../src/tables/pmt.h"
//#include "../src/descriptors/dr.h"
//#else
#include <dvbpsi/dvbpsi.h>
#include <dvbpsi/psi.h>
#include <dvbpsi/pat.h>
#include <dvbpsi/descriptor.h>
#include <dvbpsi/pmt.h>
#include <dvbpsi/dr.h>
//#endif

#define SYSTEM_CLOCK_DR 0x0B
#define MAX_BITRATE_DR 0x0E
#define STREAM_IDENTIFIER_DR 0x52
#define SUBTITLING_DR 0x59

/*
 * dvbpsi_pat_t* p_npat = NULL;
dvbpsi_pmt_t* p_npmt = NULL;
dvbpsi_psi_section_t * pat_section = NULL;
*/

/*****************************************************************************
 * General typdefs
 *****************************************************************************/
typedef int vlc_bool_t;
#define VLC_FALSE 0
#define VLC_TRUE  1

typedef int64_t mtime_t;

#define REPORT_PCR 0 /* report PCR arrival and values */
#define REPORT_UDP 1 /* report UDP packet arrival (7 ts packets) */

/*****************************************************************************
 * TS stream structures
 *----------------------------------------------------------------------------
 * PAT pid=0
 * - refers to N PMT's with pids known PAT
 *  PMT 0 pid=X stream_type
 *  PMT 1 pid=Y stream_type
 *  PMT 2 pid=Z stream_type
 *  - a PMT refers to N program_streams with pids known from PMT
 *   PID A type audio
 *   PID B type audio
 *   PID C type audio .. etc
 *   PID D type video
 *   PID E type teletext
 *****************************************************************************/

typedef struct
{
    dvbpsi_t * handle;

    int i_pat_version;
    int i_ts_id;
} ts_pat_t;

typedef struct ts_pid_s
{
    int         i_pid;

    vlc_bool_t  b_seen;
    int         i_cc;   /* countinuity counter */

	unsigned char	i_type; /* According to ISO/IEC 13818-1 Table 2-29 stream type */

    vlc_bool_t  b_pcr;  /* this PID is the PCR_PID */
    mtime_t     i_pcr;  /* last know PCR value */

	struct ts_pid_s	*next;
} ts_pid_t;

typedef struct ts_pmt_s
{
    dvbpsi_t * handle;

    int         i_number; /* i_number = 0 is actually a NIT */
    int         i_pmt_version;

	int			i_es_cnt; /* number of ES streams in this program */
	ts_pid_t	*es_pid;

    ts_pid_t    *pid_pmt;
    ts_pid_t    *pid_pcr;
} ts_pmt_t;

typedef struct
{
    ts_pat_t    pat;

    int         i_pmt;
    ts_pmt_t    pmt;

    ts_pid_t    pid[8192];
} ts_stream_t;


/*
 *Global variable
 */

ts_stream_t *old_stream = NULL;
FILE *output = NULL;
unsigned short video_old_pid = 0x1FFF;
unsigned short audio_old_pid = 0x1FFF;
unsigned short video_new_pid = 0x1FFF;
unsigned short audio_new_pid = 0x1FFF;
int is_patpmt_completed = 0;
unsigned short pcr_pid = 0x1FFF;

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static void DumpPMT(void* p_data, dvbpsi_pmt_t* p_pmt);

/*****************************************************************************
 * ReadPacket
 *****************************************************************************/
static int ReadPacket( int i_fd, uint8_t* p_dst )
{
    int i = 187;
    int i_rc = 1;

    p_dst[0] = 0;

    while((p_dst[0] != 0x47) && (i_rc > 0))
    {
        i_rc = read(i_fd, p_dst, 1);
    }

    while((i != 0) && (i_rc > 0))
    {
        i_rc = read(i_fd, p_dst + 188 - i, i);
        if(i_rc >= 0)
            i -= i_rc;
    }
    return (i_rc <= 0) ? i_rc : 188;
}

#ifdef HAVE_SYS_SOCKET_H
static int ReadPacketFromSocket( int i_socket, uint8_t* p_dst, size_t i_size)
{
    int i_rc = -1;

    memset( p_dst, 0, i_size );
    i_rc = read( i_socket, p_dst, i_size );
    if( i_rc < 0 ) fprintf( stderr, "READ INTERRUPTED BY SIGNAL\n" );
    if( i_rc == 0 ) fprintf( stderr, "READ RETURNS 0\n" );
    return i_rc;
}

/*****************************************************************************
 * report_PrintHeader
 *****************************************************************************/
/*
static void report_Header( int i_report )
{
#ifndef HAVE_GETTIMEOFDAY
    printf("*** WARNING: !!! no gettimeofday support found !!! timing information will not be given !!! ***\n");
#endif

    switch(i_report)
    {
        case REPORT_PCR:
            printf( "# seqno, PID PCR, network arrival (ms), PCR value (ms), PCR prev (ms), delta (ms), bytes since last pcr, bitrate (bits/delta) Kbps since last pcr\n" );
            break;
        case REPORT_UDP:
            printf( "# seq no, packet arrival (ms), delta (ms), bytes since last\n");
            break;
    }
}
*/
#endif

/*****************************************************************************
 * PrintPacketTiming for REPORT_UDP
 *****************************************************************************/
/*
#ifdef HAVE_SYS_SOCKET_H
#ifdef HAVE_GETTIMEOFDAY
static mtime_t report_UDPPacketTiming( int32_t i_seqno, int32_t bytes, mtime_t time_prev, mtime_t *time_base )
{
    mtime_t time_current;
    mtime_t tv_delta;
    struct timeval tv;
    // arrival time of packet 
    gettimeofday( &tv, NULL );
    time_current = ((mtime_t)tv.tv_sec*1000) + ((mtime_t)tv.tv_usec/1000);
    if( time_prev == 0 ) // probably the first one 
        tv_delta = (mtime_t) 0;
    else
        tv_delta = (mtime_t)(time_current - time_prev);

    if( *time_base == 0 )
        *time_base = time_current;

    printf( "%.2d %"PRId64" %"PRId64" ",
            i_seqno, time_current - *time_base,
            tv_delta );
    time_prev = time_current;
    printf( "%d\n", bytes );
    return time_prev;
}
#else
static void report_UDPPacketTiming( int32_t i_seqno, int32_t bytes )
{
    printf( "%.2d %"PRId64" %"PRId64" ", i_seqno, 0UL, 0UL );
    printf( "%d\n", bytes );
}
#endif
#endif
*/

#ifdef HAVE_SYS_SOCKET_H
#ifdef HAVE_GETTIMEOFDAY
static mtime_t report_PCRPacketTiming( int i_cc, ts_pid_t *ts_pid,
                    mtime_t i_prev_pcr, mtime_t time_prev, int32_t i_bytes )
#else
static void report_PCRPacketTiming( int i_cc, ts_pid_t *ts_pid,
                                    mtime_t i_prev_pcr, int32_t i_bytes )
#endif
{
    mtime_t i_delta = 0;

    /* sequence number and program_id */
    printf( "%.2d %d ", i_cc, ts_pid->i_pid );

#ifdef HAVE_GETTIMEOFDAY
    mtime_t time_current;
    mtime_t tv_delta;
    struct timeval tv;

    /* arrival time of packet */
    gettimeofday( &tv, NULL );
    time_current = ((mtime_t)tv.tv_sec*1000) + ((mtime_t)tv.tv_usec/1000);
    if( time_prev == 0 ) /* probably the first one */
        tv_delta = (mtime_t) 0;
    else
        tv_delta = (mtime_t)(time_current - time_prev);

    printf( "%"PRId64" ", tv_delta );
    time_prev = time_current;
#else
    printf( "0 " );
#endif
    /* pcr value, previous pcr, delta between pcr and previous, bytes since last pcr */
    if( i_prev_pcr == 0 )
        i_delta = 0;
    else
        i_delta = ts_pid->i_pcr - i_prev_pcr;

    printf( "%"PRId64" %"PRId64" %"PRId64" ",
            ts_pid->i_pcr, i_prev_pcr, i_delta );

    /* bitrate since last pcr */
    if( (i_delta > 0) )
        printf( "%d %"PRId64"", i_bytes, (long int)(i_bytes*8)/i_delta/1000 );
    else
        printf( "%d 0", i_bytes );

#ifdef HAVE_GETTIMEOFDAY
    printf( "\n" );
    return time_prev;
#else
    printf( "\n" );
#endif
}
#endif

static void message(dvbpsi_t *handle, const dvbpsi_msg_level_t level, const char* msg)
{
    switch(level)
    {
        case DVBPSI_MSG_ERROR: fprintf(stderr, "Error: "); break;
        case DVBPSI_MSG_WARN:  fprintf(stderr, "Warning: "); break;
        case DVBPSI_MSG_DEBUG: fprintf(stderr, "Debug: "); break;
        default: /* do nothing */
            return;
    }
    fprintf(stderr, "%s\n", msg);
}

/*****************************************************************************
 * writePSI
 *****************************************************************************/
static void writePSI(uint8_t* p_packet, dvbpsi_psi_section_t* p_section)
{
  p_packet[0] = 0x47;

  while(p_section)
  {
    size_t i_bytes_written = 0;
    uint8_t* p_pos_in_ts;
    uint8_t* p_byte = p_section->p_data;
    uint8_t* p_end =   p_section->p_payload_end
                     + (p_section->b_syntax_indicator ? 4 : 0);

    p_packet[1] |= 0x40;
    p_packet[3] = (p_packet[3] & 0x0f) | 0x10;

    p_packet[4] = 0x00; /* pointer_field */
    p_pos_in_ts = p_packet + 5;

    while((p_pos_in_ts < p_packet + 188) && (p_byte < p_end))
      *(p_pos_in_ts++) = *(p_byte++);
    while(p_pos_in_ts < p_packet + 188)
      *(p_pos_in_ts++) = 0xff;

    i_bytes_written = fwrite(p_packet, 1, 188, output);
    if(i_bytes_written < 188)
    {
        fprintf(stderr,"write packet failed! errno=%d error info:%s\n", errno, strerror(errno));
		exit(EXIT_FAILURE);
    }
    p_packet[3] = (p_packet[3] + 1) & 0x0f;

    while(p_byte < p_end)
    {
      p_packet[1] &= 0xbf;
      p_packet[3] = (p_packet[3] & 0x0f) | 0x10;

      p_pos_in_ts = p_packet + 4;

      while((p_pos_in_ts < p_packet + 188) && (p_byte < p_end))
        *(p_pos_in_ts++) = *(p_byte++);
      while(p_pos_in_ts < p_packet + 188)
        *(p_pos_in_ts++) = 0xff;

      i_bytes_written = fwrite(p_packet, 1, 188, output);
      if(i_bytes_written < 188)
      {
        fprintf(stderr,"write packet failed! errno=%d error info:%s\n", errno, strerror(errno));
		exit(EXIT_FAILURE);
      }

      p_packet[3] = (p_packet[3] + 1) & 0x0f;
    }

    p_section = p_section->p_next;
  }

}

//static int generate_pat_pmt(void)
static int output_PSI(int is_pat)
{
	ts_stream_t  *p_stream = old_stream;
	static int first_time = 0;
	static dvbpsi_psi_section_t* pmt_section = NULL;
	static dvbpsi_psi_section_t* pat_section = NULL;
	static unsigned char pmt_packet[188];
	static unsigned char pat_packet[188];

	unsigned short pat_cc = (uint16_t) (pat_packet[3] & 0x0F);
	unsigned short pmt_cc = (uint16_t) (pmt_packet[3] & 0x0F);

	if( !first_time)
	{ 
		first_time = 1;
		p_stream->pid[video_new_pid].i_pid = video_new_pid;
		p_stream->pid[audio_new_pid].i_pid = audio_new_pid;

		dvbpsi_pat_t pat;
		dvbpsi_pmt_t pmt;

		dvbpsi_t *p_psi_pat = dvbpsi_new( &message, DVBPSI_MSG_DEBUG);
		dvbpsi_t *p_psi_pmt = dvbpsi_new( &message, DVBPSI_MSG_DEBUG);
		if ( !p_psi_pat || !p_psi_pmt)
		{
			exit(EXIT_FAILURE);
		}

		//generate PMT
		dvbpsi_pmt_init( &pmt, 1, 0, 1, pcr_pid);
		ts_pid_t *p_tmp = p_stream->pmt.es_pid;
		fprintf(stdout, "p_tmp=%d\n", ( NULL == p_tmp));
		while(p_tmp)
		{
			fprintf(stdout, "add PES stream pid=%d type=%d into PMT program id=%d\n", p_tmp->i_pid, p_tmp->i_type, p_stream->pmt.pid_pmt->i_pid);
			dvbpsi_pmt_es_add(&pmt, p_tmp->i_type, p_tmp->i_pid);
			p_tmp = p_tmp->next;
		}
		pmt_section = dvbpsi_pmt_sections_generate( p_psi_pmt, &pmt);
		pmt_packet[0] = 0x47;
		pmt_packet[1] = (p_stream->pmt.pid_pmt->i_pid >> 8) | 0x00;
		pmt_packet[2] = p_stream->pmt.pid_pmt->i_pid;
		pmt_packet[3] = 0x00;
		unsigned short pid = ( (uint16_t) (pmt_packet[1] & 0x1f) << 8) + pmt_packet[2];
		fprintf(stdout, "pmt packet pid=%d\n", pid);


		//generate PAT
		dvbpsi_pat_init(&pat, 1, 0, 1);
		dvbpsi_pat_program_add(&pat, 1, p_stream->pmt.pid_pmt->i_pid);
		//dvbpsi_pat_program_add(&pat, 2, 3);
		pat_section = dvbpsi_pat_sections_generate(p_psi_pat, &pat, 253);
		pat_packet[0] = 0x47;
		pat_packet[1] = pat_packet[2] = pat_packet[3] = 0x00;

	}
	//write into output file
	if( is_pat)
	{ 
		fprintf(stdout, "pat packet cc=%d\n", pat_cc);
		writePSI(pat_packet, pat_section);
	}else{ 
		fprintf(stdout, "\t\tpmt packet cc=%d\n", pmt_cc);
		writePSI(pmt_packet, pmt_section);
	}
	return 0;
}

/*****************************************************************************
 * DumpPAT
 *****************************************************************************/
static void DumpPAT(void* p_data, dvbpsi_pat_t* p_pat)
{
    dvbpsi_pat_program_t* p_program = p_pat->p_first_program;
    ts_stream_t* p_stream = (ts_stream_t*) p_data;

    if (p_stream->pmt.handle)
    {
        fprintf(stderr, "freeing old PMT\n");
        dvbpsi_pmt_detach(p_stream->pmt.handle);
        dvbpsi_delete(p_stream->pmt.handle);
        p_stream->pmt.handle = NULL;
    }

    p_stream->pat.i_pat_version = p_pat->i_version;
    p_stream->pat.i_ts_id = p_pat->i_ts_id;

    fprintf( stderr, "\n");
    fprintf( stderr, "New PAT\n");
    fprintf( stderr, "  transport_stream_id : %d\n", p_pat->i_ts_id);
    fprintf( stderr, "  version_number      : %d\n", p_pat->i_version);
    fprintf( stderr, "    | program_number @ [NIT|PMT]_PID\n");
    while( p_program )
    {
            if (p_stream->pmt.handle)
            {
                dvbpsi_pmt_detach(p_stream->pmt.handle);
                dvbpsi_delete(p_stream->pmt.handle);
                p_stream->pmt.handle = NULL;
            }
            p_stream->i_pmt++;
            p_stream->pmt.i_number = p_program->i_number;
            p_stream->pmt.pid_pmt = &p_stream->pid[p_program->i_pid];
            p_stream->pmt.pid_pmt->i_pid = p_program->i_pid;
            p_stream->pmt.handle  = dvbpsi_new(&message, DVBPSI_MSG_DEBUG);
            if (p_stream->pmt.handle == NULL)
            {
                fprintf(stderr, "could not allocate new dvbpsi_t handle\n");
                break;
            }
            if (!dvbpsi_pmt_attach(p_stream->pmt.handle, p_program->i_number, DumpPMT, p_stream ))
            {
                dvbpsi_delete(p_stream->pmt.handle);
                fprintf(stderr, "could not attach PMT\n");
                break;
            }
            fprintf( stderr, "    | %14d @ 0x%x (%d)\n",
                p_program->i_number, p_program->i_pid, p_program->i_pid);
            p_program = p_program->p_next;
    }
    fprintf( stderr, "  active              : %d\n", p_pat->b_current_next);


	if(p_stream->i_pmt > 1)
	{
		fprintf(stderr, "\033[01;0;31mMulti program in 1 stream is not supported!\033[0m\n");
		exit(EXIT_FAILURE);
	}

	if( is_patpmt_completed)
	{
		output_PSI(1);
	}

    dvbpsi_pat_delete(p_pat);
}

/*****************************************************************************
 * GetTypeName
 *****************************************************************************/
static char const* GetTypeName(uint8_t type)
{
  switch (type)
    {
    case 0x00:
      return "Reserved";
    case 0x01:
      return "ISO/IEC 11172 Video";
    case 0x02:
      return "ISO/IEC 13818-2 Video";
    case 0x03:
      return "ISO/IEC 11172 Audio";
    case 0x04:
      return "ISO/IEC 13818-3 Audio";
    case 0x05:
      return "ISO/IEC 13818-1 Private Section";
    case 0x06:
      return "ISO/IEC 13818-1 Private PES data packets";
    case 0x07:
      return "ISO/IEC 13522 MHEG";
    case 0x08:
      return "ISO/IEC 13818-1 Annex A DSM CC";
    case 0x09:
      return "H222.1";
    case 0x0A:
      return "ISO/IEC 13818-6 type A";
    case 0x0B:
      return "ISO/IEC 13818-6 type B";
    case 0x0C:
      return "ISO/IEC 13818-6 type C";
    case 0x0D:
      return "ISO/IEC 13818-6 type D";
    case 0x0E:
      return "ISO/IEC 13818-1 auxillary";
	case 0x0F:
	  return "ISO/IEC 13818-7 AAC Audio(Audio with ADTS transport syntax)";
	case 0x11:
	  return "ISO/IEC 14496-3 MPEG-4 Audio";
	case 0x1b:
	  return "MPEG-4 part 10 H.264 Video";
    default:
      if (type < 0x80)
    return "ISO/IEC 13818-1 reserved";
      else
    return "User Private";
    }
}

//return	0: audio
//			1: video
//			-1:other
static int getType(uint8_t type)
{
	switch(type)
	{ 
		case 0x01:
		case 0x02:
		case 0x1b:
		case 0x42:
		  return 1;
		case 0x03:
		case 0x04:
		case 0x0F:
		case 0x11:
		case 0x81:
		  return 0;
		default:
		  return -1;
	}
}


/*****************************************************************************
 * DumpMaxBitrateDescriptor
 *****************************************************************************/
static void DumpMaxBitrateDescriptor(dvbpsi_max_bitrate_dr_t* bitrate_descriptor)
{
  fprintf( stderr, "Bitrate: %d\n", bitrate_descriptor->i_max_bitrate);
}

/*****************************************************************************
 * DumpSystemClockDescriptor
 *****************************************************************************/
static void DumpSystemClockDescriptor(dvbpsi_system_clock_dr_t* p_clock_descriptor)
{
  fprintf( stderr, "External clock: %s, Accuracy: %E\n",
     p_clock_descriptor->b_external_clock_ref ? "Yes" : "No",
     p_clock_descriptor->i_clock_accuracy_integer *
     pow(10.0, -(double)p_clock_descriptor->i_clock_accuracy_exponent));
}

/*****************************************************************************
 * DumpStreamIdentifierDescriptor
 *****************************************************************************/
static void DumpStreamIdentifierDescriptor(dvbpsi_stream_identifier_dr_t* p_si_descriptor)
{
  fprintf( stderr, "Component tag: %d\n",
     p_si_descriptor->i_component_tag);
}

/*****************************************************************************
 * DumpSubtitleDescriptor
 *****************************************************************************/
static void DumpSubtitleDescriptor(dvbpsi_subtitling_dr_t* p_subtitle_descriptor)
{
  int a;

  fprintf( stderr, "%d subtitles,\n", p_subtitle_descriptor->i_subtitles_number);
  for (a = 0; a < p_subtitle_descriptor->i_subtitles_number; ++a)
    {
      fprintf( stderr, "       | %d - lang: %c%c%c, type: %d, cpid: %d, apid: %d\n", a,
         p_subtitle_descriptor->p_subtitle[a].i_iso6392_language_code[0],
         p_subtitle_descriptor->p_subtitle[a].i_iso6392_language_code[1],
         p_subtitle_descriptor->p_subtitle[a].i_iso6392_language_code[2],
         p_subtitle_descriptor->p_subtitle[a].i_subtitling_type,
         p_subtitle_descriptor->p_subtitle[a].i_composition_page_id,
         p_subtitle_descriptor->p_subtitle[a].i_ancillary_page_id);
    }
}

/*****************************************************************************
 * DumpDescriptors
 *****************************************************************************/
static void DumpDescriptors(const char* str, dvbpsi_descriptor_t* p_descriptor)
{
    int i;

    while(p_descriptor)
    {
        fprintf( stderr, "%s 0x%02x : ", str, p_descriptor->i_tag);
        switch (p_descriptor->i_tag)
        {
        case SYSTEM_CLOCK_DR:
            DumpSystemClockDescriptor(dvbpsi_DecodeSystemClockDr(p_descriptor));
            break;
        case MAX_BITRATE_DR:
            DumpMaxBitrateDescriptor(dvbpsi_DecodeMaxBitrateDr(p_descriptor));
            break;
        case STREAM_IDENTIFIER_DR:
            DumpStreamIdentifierDescriptor(dvbpsi_DecodeStreamIdentifierDr(p_descriptor));
            break;
        case SUBTITLING_DR:
            DumpSubtitleDescriptor(dvbpsi_DecodeSubtitlingDr(p_descriptor));
            break;
        default:
            fprintf( stderr, "\"");
            for(i = 0; i < p_descriptor->i_length; i++)
                fprintf( stderr, "%c", p_descriptor->p_data[i]);
            fprintf( stderr, "\"\n");
        }
        p_descriptor = p_descriptor->p_next;
    }
}

static int update_av_pid(ts_stream_t *p_stream, unsigned short vpid, unsigned short apid)
{
	if( !(p_stream) || !(p_stream->pmt.es_pid))
	{
		return -1;
	}

	if(p_stream->pmt.i_es_cnt > 3)
	{
		fprintf(stderr, "Error!There are more then 3 ES stream in program %d pid=%d\n"
				,p_stream->pmt.i_number,p_stream->pmt.pid_pmt->i_pid);
		exit(EXIT_FAILURE);
	}
	
	ts_pid_t v_pid, a_pid;
	v_pid.i_pid = vpid;
	v_pid.b_pcr = false;
	v_pid.b_seen = true;
	a_pid.i_pid = apid;
	a_pid.b_pcr = false;
	a_pid.b_seen = true;

	int cnt = p_stream->pmt.i_es_cnt;
	ts_pid_t *pid_tmp = p_stream->pmt.es_pid;

	while(cnt > 0 && NULL != pid_tmp)
	{
		if(1 == getType(pid_tmp->i_type))
		{	//video
			v_pid.i_type = pid_tmp->i_type;
			v_pid.b_pcr = pid_tmp->b_pcr;
			v_pid.i_pcr = pid_tmp->i_pcr;
			video_old_pid = pid_tmp->i_pid;
		}else if(0 == getType(pid_tmp->i_type))
		{	//audio
			a_pid.i_type = pid_tmp->i_type;
			a_pid.b_pcr = pid_tmp->b_pcr;
			a_pid.i_pcr = pid_tmp->i_pcr;
			audio_old_pid = pid_tmp->i_pid;
		}
		pid_tmp = pid_tmp->next;
		--cnt;
	}

	pcr_pid = (v_pid.b_pcr) ? v_pid.i_pid :a_pid.i_pid;
	fprintf(stdout, "stream pid=%d(0x%x) contatins PCR information.\n", pcr_pid, pcr_pid);

	p_stream->pid[vpid].i_type = v_pid.i_type;
	p_stream->pid[vpid].b_pcr = v_pid.b_pcr;
	p_stream->pid[vpid].i_pcr = v_pid.i_pcr;
	p_stream->pid[apid].i_type = a_pid.i_type;
	p_stream->pid[apid].b_pcr = a_pid.b_pcr;
	p_stream->pid[apid].i_pcr = a_pid.i_pcr;

	p_stream->pmt.es_pid = &(p_stream->pid[vpid]);
	p_stream->pid[vpid].next = &(p_stream->pid[apid]);
	p_stream->pid[apid].next = NULL;



	return 0;
}


/*****************************************************************************
 * DumpPMT
 *****************************************************************************/
static void DumpPMT(void* p_data, dvbpsi_pmt_t* p_pmt)
{
    dvbpsi_pmt_es_t* p_es = p_pmt->p_first_es;
    ts_stream_t* p_stream = (ts_stream_t*) p_data;

    p_stream->pmt.i_pmt_version = p_pmt->i_version;
    p_stream->pmt.pid_pcr = &p_stream->pid[p_pmt->i_pcr_pid];
    p_stream->pid[p_pmt->i_pcr_pid].b_pcr = VLC_TRUE;

    fprintf( stderr, "\n" );
    fprintf( stderr, "New active PMT\n" );
    fprintf( stderr, "  program_number : %d\n", p_pmt->i_program_number );
    fprintf( stderr, "  version_number : %d\n", p_pmt->i_version );
    fprintf( stderr, "  PCR_PID        : 0x%x (%d)\n", p_pmt->i_pcr_pid, p_pmt->i_pcr_pid);
    DumpDescriptors("    ]", p_pmt->p_first_descriptor);
    fprintf( stderr, "    | elementary_PID   type\n");

	ts_pid_t *pid_tmp = p_stream->pmt.es_pid;
	//ts_pid_t *pid_tmp = p_stream->pmt->es_pid;
	int old_pid = 0x1FFF;
    while(p_es)
    {
		fprintf(stdout, "processing PID=%d\n", p_es->i_pid);
		if( !(p_stream->pmt.es_pid))
		//if( !pid_tmp)
		{ 
			p_stream->pmt.es_pid = &(p_stream->pid[p_es->i_pid]);
			p_stream->pid[p_es->i_pid].i_type = p_es->i_type;
			p_stream->pid[p_es->i_pid].i_pid = p_es->i_pid;
			old_pid = p_es->i_pid;
			fprintf(stdout, "if_processing PID=%d pid_tmp=%d pmt.es_pid=%d type=%d\n", p_es->i_pid, NULL == pid_tmp, (NULL == p_stream->pmt.es_pid), p_es->i_type);
		}else{
			p_stream->pid[old_pid].next = &(p_stream->pid[p_es->i_pid]);
			p_stream->pid[p_es->i_pid].i_type = p_es->i_type;
			p_stream->pid[p_es->i_pid].i_pid = p_es->i_pid;
			p_stream->pid[p_es->i_pid].next = NULL;
			fprintf(stdout, "else_processing PID=%d pid_tmp=%d pmt.es_pid=%d type=%d\n", p_es->i_pid, NULL == pid_tmp, (NULL == p_stream->pmt.es_pid), p_es->i_type);
		}

		++(p_stream->pmt.i_es_cnt);
        fprintf( stderr, "    | 0x%x (%d) @ 0x%02x (%s)\n", p_es->i_pid, p_es->i_pid
				, p_es->i_type, GetTypeName(p_es->i_type));
        DumpDescriptors("    |  ]", p_es->p_first_descriptor);
        p_es = p_es->p_next;
    }
	if(p_stream->pmt.i_es_cnt > 2 )
	{
		fprintf(stderr, "\033[0;31m only 1 video stream and 1 audio stream is supported in a PMT!\033[0m\n");
	}

	if( is_patpmt_completed)
	{
		output_PSI(0);
	}
	//marked probe work finished.
	is_patpmt_completed = 1;

    dvbpsi_pmt_delete(p_pmt);
}


/*****************************************************************************
 * usage
 *****************************************************************************/
static void usage( char *name )
{
	printf( "Usage:\n  %s -f <filename> -o <filename> -w <video_pid> -a <audio_pid>\n\n", name );
	printf( "eg:\n  %s -f in.ts -o out.ts -w 8000 -a 8001\n", name );
	/*
#ifdef HAVE_SYS_SOCKET_H
    printf( "Usage: %s [--file <filename>|--udp <ipaddress> --port <port> --mtu <mtu>|--help]\n", name );
    printf( "       %s [-f <filename>|-u <ipaddress> -p <port> -m <mtu>|-h]\n", name );
#else
    printf( "Usage: %s [--file <filename>|--help]\n", name );
    printf( "       %s [-f <filename>|-h]\n", name );
#endif
    printf( "\n" );
    printf( "       %s --help\n", name );
    printf( "       %s --file <filename>\n", name );
#ifdef HAVE_SYS_SOCKET_H
    printf( "       %s --udp <ipaddres> --port <port> --mtu <mtu>\n", name );
#endif
    printf( "Arguments:\n" );
    printf( "file   : read MPEG2-TS stream from file\n" );
#ifdef HAVE_SYS_SOCKET_H
    printf( "udp    : read MPEG2-TS stream from network using UDP protocol\n" );
    printf( "port   : read MPEG2-TS stream from this port number\n" );
    printf( "mtu    : read MPEG2-TS stream from network using maximum transfer unit (mtu) = 1316\n" );
    printf( "report : report type udp, pcr (default udp)\n" );
#endif
    printf( "help   : print this help message\n" );
*/
}


static int get_pat_pmt(char* pa_argv, char * filename, int i_mtu, int i_port, int i_report,char* ipaddress, bool b_verbose)
{ 
    int i_fd = -1;

#ifdef HAVE_SYS_SOCKET_H
#ifdef HAVE_GETTIMEOFDAY
	mtime_t time_prev = 0;
	//mtime_t time_base = 0;
#endif
	mtime_t i_prev_pcr = 0; /* 33 bits */
#endif

    int      i_old_cc = -1;
    uint32_t i_bytes = 0; /* bytes transmitted between PCR's */

    uint8_t *p_data = NULL;
    int i_len = 0;

    /* initialize */
    if( filename )
    {
        i_fd = open( filename, 0 );
        p_data = (uint8_t *) malloc( sizeof( uint8_t ) * 188 );
        if( !p_data )
            goto out_of_memory;
    }
#ifdef HAVE_SYS_SOCKET_H
    else if( ipaddress )
    {
        i_fd = create_udp_connection( ipaddress, i_port );
        p_data = (uint8_t *) malloc( sizeof( uint8_t ) * i_mtu );
        if( !p_data )
            goto out_of_memory;
    }
#endif
    else
    {
        usage( pa_argv);
        goto error;
    }

    if( i_fd < 0 )
    {
        fprintf( stderr, "no input selected\n" );
        usage( pa_argv );
        goto error;
    }

    ts_stream_t *p_stream = NULL;
	p_stream = (ts_stream_t *) malloc( sizeof(ts_stream_t) );
	old_stream = p_stream;
    if( !p_stream )
        goto out_of_memory;
    memset( p_stream, 0, sizeof(ts_stream_t) );

    /* Read first packet */
    if( filename )
        i_len = ReadPacket( i_fd, p_data );
#ifdef HAVE_SYS_SOCKET_H
    else
        i_len = ReadPacketFromSocket( i_fd, p_data, i_mtu );

    /* print the right report header */
    //report_Header( i_report );
#endif

    p_stream->pat.handle = dvbpsi_new(&message, DVBPSI_MSG_DEBUG);
    if (p_stream->pat.handle == NULL)
        goto dvbpsi_out;
    if (!dvbpsi_pat_attach(p_stream->pat.handle, DumpPAT, p_stream))
        goto dvbpsi_out;

    /* Enter infinite loop */
    while( i_len > 0 )
    {
		if( is_patpmt_completed)
		{
			/* clean up */
			if( filename )
				close( i_fd );
		#ifdef HAVE_SYS_SOCKET_H
			else
				close_connection( i_fd );
		#endif
			return 0;
		}
        int i = 0;
#ifdef HAVE_SYS_SOCKET_H
        vlc_bool_t b_first = VLC_FALSE;
#endif
        i_bytes += i_len;
        for( i = 0; i < i_len; i += 188 )
        {
            uint8_t   *p_tmp = &p_data[i];
            uint16_t   i_pid = ((uint16_t)(p_tmp[1] & 0x1f) << 8) + p_tmp[2];
            int        i_cc = (p_tmp[3] & 0x0f);
            vlc_bool_t b_adaptation = (p_tmp[3] & 0x20); /* adaptation field */
            vlc_bool_t b_discontinuity_seen = VLC_FALSE;

#ifdef HAVE_SYS_SOCKET_H
            if( i_report == REPORT_UDP && !b_first )
            {
				/*
#ifdef HAVE_GETTIMEOFDAY
                time_prev = report_UDPPacketTiming( i_cc, i_bytes, time_prev, &time_base );
#else
               report_UDPPacketTiming( i_cc, i_bytes );
#endif
			   */
                b_first = VLC_TRUE;
            }
#endif
            if (p_data[i] != 0x47) /* no sync skip this packet */
            {
                fprintf( stderr, "Missing TS sync word, skipping 188 bytes\n" );
                break;
            }

            if (i_pid == 0x1FFF) /* null packet - TS content undefined */
                continue;

            if( i_pid == 0x0 )
			{ 
				fprintf( stderr,"process PAT begin\n");
                dvbpsi_packet_push(p_stream->pat.handle, p_tmp);
				fprintf( stderr,"process PAT end\n");
			}
            else if( p_stream->pmt.pid_pmt && i_pid == p_stream->pmt.pid_pmt->i_pid )
			{ 
				fprintf( stderr,"process PMT begin\n");
                dvbpsi_packet_push(p_stream->pmt.handle, p_tmp);
				fprintf( stderr,"process PMT end\n");
			}

            /* Remember PID */
            if( !p_stream->pid[i_pid].b_seen )
            {
				fprintf( stderr,"Found new PID=%d\n",p_stream->pid[ i_pid].i_pid);
				p_stream->pid[i_pid].i_pid = i_pid;
                p_stream->pid[i_pid].b_seen = VLC_TRUE;
                i_old_cc = i_cc;
                p_stream->pid[i_pid].i_cc = i_cc;
            }
            else
            {
                /* Check continuity counter */
                int i_diff = 0;

                i_diff = i_cc - (p_stream->pid[i_pid].i_cc+1)%16;
                b_discontinuity_seen = ( i_diff != 0 );

                /* Update CC */
                i_old_cc = p_stream->pid[i_pid].i_cc;
                p_stream->pid[i_pid].i_cc = i_cc;
            }

            /* Handle discontinuities if they occurred,
             * according to ISO/IEC 13818-1: DIS pages 20-22 */
            if( b_adaptation )
            {
                vlc_bool_t b_discontinuity_indicator = (p_tmp[5]&0x80);
                vlc_bool_t b_random_access_indicator = (p_tmp[5]&0x40);
                vlc_bool_t b_pcr = (p_tmp[5]&0x10);  /* PCR flag */

                if( b_discontinuity_indicator )
                    fprintf( stderr, "Discontinuity indicator (pid %d)\n", i_pid );
                if( b_random_access_indicator )
                    fprintf( stderr, "Random access indicator (pid %d)\n", i_pid );

                /* Dump PCR */
                if( b_pcr && (p_tmp[4] >= 7) )
                {
                    mtime_t i_pcr;  /* 33 bits */

                    i_pcr = ( ( (mtime_t)p_tmp[6] << 25 ) |
                              ( (mtime_t)p_tmp[7] << 17 ) |
                              ( (mtime_t)p_tmp[8] << 9 ) |
                              ( (mtime_t)p_tmp[9] << 1 ) |
                              ( (mtime_t)p_tmp[10] >> 7 ) ) / 90;
                    p_stream->pid[i_pid].i_pcr = i_pcr;

#ifdef HAVE_SYS_SOCKET_H
                    i_prev_pcr = p_stream->pid[i_pid].i_pcr;

                    if( i_report == REPORT_PCR )
                    {
#ifdef HAVE_GETTIMEOFDAY
                        time_prev = report_PCRPacketTiming( i_cc, &(p_stream->pid[i_pid]), i_prev_pcr, time_prev, i_bytes );
#else
                        report_PCRPacketTiming( i_cc, &(p_stream->pid[i_pid]), i_prev_pcr, i_bytes );
#endif
                    }
#endif
                    i_bytes = 0; /* reset byte counter */

                    if( b_discontinuity_indicator )
                    {
                        /* cc discontinuity is expected */
                        fprintf( stderr, "Server signalled the continuity counter discontinuity\n" );
                        /* Discontinuity has been handled */
                        b_discontinuity_seen = VLC_FALSE;
                    }
                }
            }

            if( b_discontinuity_seen )
            {
                fprintf( stderr, "Continuity counter discontinuity (pid %d found %d expected %d)\n",
                    i_pid, p_stream->pid[i_pid].i_cc, i_old_cc+1 );
                /* Discontinuity has been handled */
                b_discontinuity_seen = VLC_FALSE;
            }
        }

#ifdef HAVE_SYS_SOCKET_H
        if( i_report == REPORT_UDP )
            i_bytes = 0; /* reset byte counter */
#endif
        /* Read next packet */
        if( filename )
            i_len = ReadPacket( i_fd, p_data );
#ifdef HAVE_SYS_SOCKET_H
        else
            i_len = ReadPacketFromSocket( i_fd, p_data, i_mtu );
#endif
    }//end of while

    if( p_stream->pmt.handle )
    {
        dvbpsi_pmt_detach( p_stream->pmt.handle );
        dvbpsi_delete( p_stream->pmt.handle );
    }
    if( p_stream->pat.handle )
    {
        dvbpsi_pat_detach( p_stream->pat.handle );
        dvbpsi_delete( p_stream->pat.handle );
    }

    /* clean up */
    if( filename )
        close( i_fd );
#ifdef HAVE_SYS_SOCKET_H
    else
        close_connection( i_fd );
#endif

    if( p_data )    free( p_data );
    if( filename )  free( filename );
#ifdef HAVE_SYS_SOCKET_H
    if( ipaddress ) free( ipaddress );
#endif

    /* free other stuff first ;-)*/
    if( p_stream )  free( p_stream );
    return EXIT_SUCCESS;

out_of_memory:
    fprintf( stderr, "Out of memory\n" );

dvbpsi_out:
    if( p_stream && p_stream->pat.handle )
    {
        dvbpsi_pat_detach( p_stream->pat.handle );
        dvbpsi_delete( p_stream->pat.handle );
    }

error:
    if( p_data )    free( p_data );
    if( filename )  free( filename );
#ifdef HAVE_SYS_SOCKET_H
    if( ipaddress ) free( ipaddress );
#endif

    /* free other stuff first ;-)*/
    if( p_stream )  free( p_stream );
    return EXIT_FAILURE;
}


/*****************************************************************************
 * main
 *****************************************************************************/
int main(int i_argc, char* pa_argv[])
{
#ifdef HAVE_SYS_SOCKET_H
    const char* const short_options = "hf:m:p:r:u:v:w:a:o:";
#else
    const char* const short_options = "hf:v:w:a:o:";
#endif
    const struct option long_options[] =
    {
        { "help",       0, NULL, 'h' },
        { "file",       1, NULL, 'f' },
#ifdef HAVE_SYS_SOCKET_H
        { "mtu",        1, NULL, 'm' },
        { "port",       1, NULL, 'p' },
        { "udp",        1, NULL, 'u' },
        { "report",     1, NULL, 'r' },
#endif
        { "verbose",    0, NULL, 'v' },
		{ "videopid",	1, NULL, 'w'},
		{ "audiopid",	1, NULL, 'a'},
		{ "outputfile", 1, NULL, 'o'},
        { NULL,         0, NULL, 0 },
    };
    int next_option = 0;

    int i_fd = -1;
#ifdef HAVE_SYS_SOCKET_H
    int i_mtu = 1316; /* (7 * 188) = 1316 < 1500 network MTU */
    int i_report = REPORT_UDP; /* REPORT_PCR REPORT_UDP */
    int i_port = 0;
    char *ipaddress = NULL;
#ifdef HAVE_GETTIMEOFDAY
    mtime_t  time_prev = 0;
    //mtime_t  time_base = 0;
#endif
    mtime_t  i_prev_pcr = 0;  /* 33 bits */
#endif
    int      i_old_cc = -1;
    uint32_t i_bytes = 0; /* bytes transmitted between PCR's */
    char *filename = NULL;
	char *outputfile = NULL;

    uint8_t *p_data = NULL;
    ts_stream_t *p_stream = NULL;
    int i_len = 0;
    bool b_verbose = false;

    /* parser commandline arguments */
    do {
        next_option = getopt_long( i_argc, pa_argv, short_options, long_options, NULL );
        switch( next_option )
        {
            case 'f':
                filename = strdup( optarg );
                break;
            case 'h':
                usage( pa_argv[0] );
                goto error;
                break;
#ifdef HAVE_SYS_SOCKET_H
            case 'm':
                i_mtu = atoi( optarg );
                if( i_mtu < 0 ) i_mtu = 1316;
            else i_mtu = (i_mtu / 188) * 188;
                break;
            case 'p':
                i_port = atoi( optarg );
                break;
            case 'r':
                if( strncasecmp( "udp", optarg, 3 ) == 0 )
                    i_report = REPORT_UDP;
                else if( strncasecmp( "pcr", optarg, 3 ) == 0)
                    i_report = REPORT_PCR;
                else goto error;
                break;
            case 'u':
                ipaddress = strdup( optarg );
                break;
#endif
            case 'v':
                b_verbose = true;
                break;
			case 'w':
				video_new_pid = atoi(optarg);
				if( video_new_pid < 16 || video_new_pid > 8190)
				{
					fprintf(stderr, "video/audio pid range: 16-8190 !\n");
					exit(EXIT_FAILURE);
				}
				break;
			case 'a':
				audio_new_pid = atoi(optarg);
				if( audio_new_pid < 16 || audio_new_pid > 8190)
				{
					fprintf(stderr, "video/audio pid range: 16-8190 !\n");
					exit(EXIT_FAILURE);
				}
				break;
			case 'o':
				outputfile = strdup(optarg);
				//outputfile = optarg;
				break;
            case -1:
                break;
            default:
                usage( pa_argv[0] );
                goto error;
        }
    } while( next_option != -1 );

	if( !filename || !outputfile || 0x1FFF == video_new_pid || 0x1FFF == audio_new_pid)
	{
		usage(pa_argv[0]);
		exit(EXIT_FAILURE);
	}

    if( b_verbose )
    {
#ifdef HAVE_SYS_SOCKET_H
        fprintf( stderr, "set mtu to %d\n", i_mtu );
#else
        fprintf( stderr, "using file: %s", filename);
#endif
    }
	
	if(strcmp(filename, outputfile) == 0)
	{
		fprintf(stderr, "input and output cannot be the same file!\n");
		exit(EXIT_FAILURE);
	}
	if(video_new_pid == audio_new_pid)
	{
		fprintf(stderr, "video pid and audio pid cannot be the same file!\n");
		exit(EXIT_FAILURE);
	}

	//probe and get pat and pmt
	if( get_pat_pmt(pa_argv[0], filename, i_mtu, i_port, i_report,ipaddress, b_verbose) != 0 )
	{
		exit(EXIT_FAILURE);
	}
	update_av_pid(old_stream, video_new_pid, audio_new_pid);
/*
	dvbpsi_psi_section_t* pmt_section = NULL;
	dvbpsi_psi_section_t* pat_section = NULL;
	unsigned char pmt_packet[188];
	unsigned char pat_packet[188];

	generate_pat_pmt(pmt)
	*/

    /* initialize */
    if( filename )
    {
        i_fd = open( filename, 0 );
        p_data = (uint8_t *) malloc( sizeof( uint8_t ) * 188 );
        if( !p_data )
            goto out_of_memory;
    }
#ifdef HAVE_SYS_SOCKET_H
    else if( ipaddress )
    {
        i_fd = create_udp_connection( ipaddress, i_port );
        p_data = (uint8_t *) malloc( sizeof( uint8_t ) * i_mtu );
        if( !p_data )
            goto out_of_memory;
    }
#endif
    else
    {
        usage( pa_argv[0] );
        goto error;
    }

    if( i_fd < 0 )
    {
        fprintf( stderr, "no input selected\n" );
        usage( pa_argv[0] );
        goto error;
    }

    p_stream = (ts_stream_t *) malloc( sizeof(ts_stream_t) );
    if( !p_stream )
        goto out_of_memory;
    memset( p_stream, 0, sizeof(ts_stream_t) );

    /* Read first packet */
    if( filename )
        i_len = ReadPacket( i_fd, p_data );
#ifdef HAVE_SYS_SOCKET_H
    else
        i_len = ReadPacketFromSocket( i_fd, p_data, i_mtu );

    /* print the right report header */
    //report_Header( i_report );
#endif

    p_stream->pat.handle = dvbpsi_new(&message, DVBPSI_MSG_DEBUG);
    if (p_stream->pat.handle == NULL)
        goto dvbpsi_out;
    if (!dvbpsi_pat_attach(p_stream->pat.handle, DumpPAT, p_stream))
        goto dvbpsi_out;

	fprintf(stdout, "outputfile=%s\n", outputfile);
	output = fopen(outputfile, "wb");
	if(!output)
	{
		fprintf(stderr, "open output file failed! errno=%d error info=%s\n", errno, strerror(errno));
		exit(EXIT_FAILURE);
	}

    /* Enter infinite loop */
    while( i_len > 0 )
    {
        int i = 0;
#ifdef HAVE_SYS_SOCKET_H
        vlc_bool_t b_first = VLC_FALSE;
#endif
        i_bytes += i_len;
        for( i = 0; i < i_len; i += 188 )
        {
            uint8_t   *p_tmp = &p_data[i];
            uint16_t   i_pid = ((uint16_t)(p_tmp[1] & 0x1f) << 8) + p_tmp[2];
            int        i_cc = (p_tmp[3] & 0x0f);
            vlc_bool_t b_adaptation = (p_tmp[3] & 0x20); /* adaptation field */
            vlc_bool_t b_discontinuity_seen = VLC_FALSE;

#ifdef HAVE_SYS_SOCKET_H
            if( i_report == REPORT_UDP && !b_first )
            {
				/*
#ifdef HAVE_GETTIMEOFDAY
                time_prev = report_UDPPacketTiming( i_cc, i_bytes, time_prev, &time_base );
#else
                report_UDPPacketTiming( i_cc, i_bytes );
#endif
*/
                b_first = VLC_TRUE;
            }
#endif
            if (p_data[i] != 0x47) /* no sync skip this packet */
            {
                fprintf( stderr, "Missing TS sync word, skipping 188 bytes\n" );
                break;
            }

            if (i_pid == 0x1FFF) /* null packet - TS content undefined */
                continue;
			//fprintf(stdout, "this packet pid=%d old_stream's pmtpid=%d\n", i_pid, old_stream->pmt.pid_pmt->i_pid);

            if( i_pid == 0x0 )
			{ 
				fprintf(stdout, "write new PAT table\n");
                //dvbpsi_packet_push(p_stream->pat.handle, p_tmp);
				//update PAT and generate it into ts stream
				output_PSI(1);
			}else if( old_stream->pmt.pid_pmt && i_pid == old_stream->pmt.pid_pmt->i_pid )
			{ 
				fprintf(stdout, "write new PMT table\n");
                //dvbpsi_packet_push(p_stream->pmt.handle, p_tmp);
				//update PMT and generate it into ts stream
				output_PSI(0);
			}else if(i_pid == video_old_pid )
			{	//video
				//fprintf(stdout, "modify video ES old pid=%d, new pid=%d\n", video_old_pid, video_new_pid);
				p_tmp[1] = ((video_new_pid >> 8) | 0x00 ) | (p_tmp[1] & 0xE0) ;
				p_tmp[2] = (video_new_pid | 0x00);
				fwrite(p_tmp, 188, 1, output);
				continue;
			}else if(i_pid == audio_old_pid)
			{	//audio
				//fprintf(stdout, "modify audio ES old pid=%d, new pid=%d\n", audio_old_pid, audio_new_pid);
				p_tmp[1] = ((video_new_pid >> 8) | 0x00 ) | (p_tmp[1] & 0xE0) ;
				p_tmp[2] = (audio_new_pid | 0x00);
				fwrite(p_tmp, 188, 1, output);
				continue;
			}else{
				//normal ts packet, write through
				fprintf(stdout, "copy other ES, pid=%d\n", i_pid);
				fwrite(p_tmp, 188, 1, output);
				continue;
			}

            /* Remember PID */
            if( !p_stream->pid[i_pid].b_seen )
            {
				p_stream->pid[i_pid].i_pid = i_pid;
                p_stream->pid[i_pid].b_seen = VLC_TRUE;
                i_old_cc = i_cc;
                p_stream->pid[i_pid].i_cc = i_cc;
            }
            else
            {
                /* Check continuity counter */
                int i_diff = 0;

                i_diff = i_cc - (p_stream->pid[i_pid].i_cc+1)%16;
                b_discontinuity_seen = ( i_diff != 0 );

                /* Update CC */
                i_old_cc = p_stream->pid[i_pid].i_cc;
                p_stream->pid[i_pid].i_cc = i_cc;
            }

            /* Handle discontinuities if they occurred,
             * according to ISO/IEC 13818-1: DIS pages 20-22 */
            if( b_adaptation )
            {
                vlc_bool_t b_discontinuity_indicator = (p_tmp[5]&0x80);
                vlc_bool_t b_random_access_indicator = (p_tmp[5]&0x40);
                vlc_bool_t b_pcr = (p_tmp[5]&0x10);  /* PCR flag */

                if( b_discontinuity_indicator )
                    fprintf( stderr, "Discontinuity indicator (pid %d)\n", i_pid );
                if( b_random_access_indicator )
                    fprintf( stderr, "Random access indicator (pid %d)\n", i_pid );

                /* Dump PCR */
                if( b_pcr && (p_tmp[4] >= 7) )
                {
                    mtime_t i_pcr;  /* 33 bits */

                    i_pcr = ( ( (mtime_t)p_tmp[6] << 25 ) |
                              ( (mtime_t)p_tmp[7] << 17 ) |
                              ( (mtime_t)p_tmp[8] << 9 ) |
                              ( (mtime_t)p_tmp[9] << 1 ) |
                              ( (mtime_t)p_tmp[10] >> 7 ) ) / 90;
                    p_stream->pid[i_pid].i_pcr = i_pcr;

#ifdef HAVE_SYS_SOCKET_H
                    i_prev_pcr = p_stream->pid[i_pid].i_pcr;

                    if( i_report == REPORT_PCR )
                    {
#ifdef HAVE_GETTIMEOFDAY
                        time_prev = report_PCRPacketTiming( i_cc, &(p_stream->pid[i_pid]), i_prev_pcr, time_prev, i_bytes );
#else
                        report_PCRPacketTiming( i_cc, &(p_stream->pid[i_pid]), i_prev_pcr, i_bytes );
#endif
                    }
#endif
                    i_bytes = 0; /* reset byte counter */

                    if( b_discontinuity_indicator )
                    {
                        /* cc discontinuity is expected */
                        fprintf( stderr, "Server signalled the continuity counter discontinuity\n" );
                        /* Discontinuity has been handled */
                        b_discontinuity_seen = VLC_FALSE;
                    }
                }
            }

            if( b_discontinuity_seen )
            {
                fprintf( stderr, "Continuity counter discontinuity (pid %d found %d expected %d)\n",
                    i_pid, p_stream->pid[i_pid].i_cc, i_old_cc+1 );
                /* Discontinuity has been handled */
                b_discontinuity_seen = VLC_FALSE;
            }
        }

#ifdef HAVE_SYS_SOCKET_H
        if( i_report == REPORT_UDP )
            i_bytes = 0; /* reset byte counter */
#endif
        /* Read next packet */
        if( filename )
            i_len = ReadPacket( i_fd, p_data );
#ifdef HAVE_SYS_SOCKET_H
        else
            i_len = ReadPacketFromSocket( i_fd, p_data, i_mtu );
#endif
    }
    if( p_stream->pmt.handle )
    {
        dvbpsi_pmt_detach( p_stream->pmt.handle );
        dvbpsi_delete( p_stream->pmt.handle );
    }
    if( p_stream->pat.handle )
    {
        dvbpsi_pat_detach( p_stream->pat.handle );
        dvbpsi_delete( p_stream->pat.handle );
    }

    /* clean up */
    if( filename )
        close( i_fd );
#ifdef HAVE_SYS_SOCKET_H
    else
        close_connection( i_fd );
#endif

    if( p_data )    free( p_data );
    if( filename )  free( filename );
#ifdef HAVE_SYS_SOCKET_H
    if( ipaddress ) free( ipaddress );
#endif

	//free space ,add by caspian
	if(old_stream) free(old_stream);
	//dvbpsi_DeletePSISections(pat_section);
	//dvbpsi_DeletePSISections(pmt_section);

    /* free other stuff first ;-)*/
    if( p_stream )  free( p_stream );

	fprintf(stdout, "\033[0;31m modify video and audio pid finished!\033[0m\n");
    return EXIT_SUCCESS;

out_of_memory:
    fprintf( stderr, "Out of memory\n" );

dvbpsi_out:
    if( p_stream && p_stream->pat.handle )
    {
        dvbpsi_pat_detach( p_stream->pat.handle );
        dvbpsi_delete( p_stream->pat.handle );
    }

error:
    if( p_data )    free( p_data );
    if( filename )  free( filename );
#ifdef HAVE_SYS_SOCKET_H
    if( ipaddress ) free( ipaddress );
#endif

    /* free other stuff first ;-)*/
    if( p_stream )  free( p_stream );
    return EXIT_FAILURE;
}
