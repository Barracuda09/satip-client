/*
 * satip: RTP processing
 *
 * Copyright (C) 2014  mc.fishdish@gmail.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as 
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef _SATIP_RTP_H
#define _SATIP_RTP_H

#include <cstdint>

#include <pthread.h>

class satipRTP
{
	int m_vtuner_fd;;
	int m_rtp_port;
	int m_rtp_socket;
	int m_rtcp_port;
	int m_rtcp_socket;
	pthread_t m_thread;
	bool m_tcp_data;
	bool m_running;
	int m_rtp_net_buffer_size_mb;
	uint16_t m_rtp_pseq;

	/* rtcp data */
	bool m_hasLock;
	int m_signalStrength;
	int m_signalQuality;

	void parseRtcpAppPayload(char *buffer);
	void rtcpData(unsigned char* buffer, int rx);
	void* rtpDump();
	static void *thread_wrapper(void *ptr);
	
	bool m_openok;
	int openRTP();

	int Write(int fd, unsigned char *buffer, int size);
	ssize_t Read(int fd, unsigned char *buffer, int size);

public:
	satipRTP(int vtuner_fd, bool tcp_data, int rtp_net_buffer_size_mb);
	virtual ~satipRTP();
	void unset();
	int get_rtp_port() { return m_rtp_port; }
	int get_rtp_socket() { return m_rtp_socket; }
	int get_rtcp_port() { return m_rtcp_port; }
	int get_rtcp_socket() { return m_rtcp_socket; }
	bool isOpened() { return m_openok; }
	void rtpTcpData(unsigned char *data, int size);
	void run();
	void stop();

	int getHasLock() { return m_hasLock; }
	int getSignalStrength() { return m_signalStrength; }
	int getSignalQuality() { return m_signalQuality; }
};

#endif
