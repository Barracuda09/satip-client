/*
 * satip: RTSP processing
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

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <poll.h>

#include <sstream> // std::ostringstream
#include <cstring>
#include <string>
#include <string_view>

#include "config.h"
#include "rtsp.h"
#include "timer.h"
#include "log.h"

static const std::string user_agent("satip-client");

static std::string_view findRTSPResponse(
		const std::string_view msg,
		std::string_view::size_type& begin) {
	begin = msg.find("RTSP/", 0);
	if (begin == std::string::npos) {
		return "";
	}
	std::string::size_type end = msg.find("\r\n\r\n", begin);
	if (end == std::string::npos) {
		return "";
	}
	return msg.substr(begin, end - begin + 4);
}

static std::string findParameter(
		const std::string& msg,
		const std::string& param,
		const char sep) {
	std::string::size_type p = 0;
	std::string::size_type e = 0;

	// get requested param
	p = msg.find(param, 0);
	if (p == std::string::npos) {
		return "";
	}
	// find requested seperator
	p = msg.find_first_of(sep, p);
	if (p == std::string::npos) {
		return "";
	}
	// find end
	e = msg.find_first_of(";\r", p + 1);
	if (e == std::string::npos) {
		return "";
	}
	std::string value = msg.substr(p + 1, e - p - 1);
	// Remove leading and trailing spaces
	while (value[0] == ' ') {
		value.erase(0, 1);
	}
	while (value.back() == ' ') {
		value.pop_back();
	}
	return value;
}

satipRTSP::satipRTSP(satipConfig* satip_config,
	const char* host,
	const char* rtsp_port,
	satipRTP *rtp):
		m_host(host),
		m_port(rtsp_port),
		m_rtp(rtp),
		m_satip_config(satip_config),
		m_timer_reset_connect(NULL),
		m_timer_keep_alive(NULL),
		m_fd(-1),
		m_rx_data_wpos(0),
		m_rtsp_status(RTSP_STATUS_CONFIG_WAITING),
		m_rtsp_request(RTSP_REQUEST_NONE),
		m_wait_response(false),
		m_channel_changed(false)
{
	if (satip_config->isTcpData()) {
		DEBUG(MSG_MAIN,"Create RTSP. (host : %s, port : %s, TCP data mode)\n", m_host.c_str(), m_port.c_str());
		m_rx_data_len = 256*1024;
	} else {
		DEBUG(MSG_MAIN,"Create RTSP. (host : %s, port : %s, rtp_port : %d)\n", m_host.c_str(), m_port.c_str(), m_rtp->get_rtp_port());
		m_rx_data_len = 2048;
	}
	m_rx_data = std::make_unique<char[]>(m_rx_data_len);

	m_timer_reset_connect = m_satip_timer.create(timeoutConnect, static_cast<void *>(this), "reset connect");
	m_timer_keep_alive = m_satip_timer.create(timeoutKeepAlive, static_cast<void *>(this), "keep alive message");

	resetConnect();
}

satipRTSP::~satipRTSP() = default;

void satipRTSP::resetConnect()
{
	DEBUG(MSG_MAIN, "resetConnect\n");
	m_rtsp_status = RTSP_STATUS_CONFIG_WAITING;
	m_rtsp_request = RTSP_REQUEST_NONE;
	m_rtsp_cseq = 1;
	m_rtsp_session_id.clear();
	m_rtsp_stream_id = -1;
	m_rtsp_timeout = 60;

	m_rx_data_wpos = 0;

	m_wait_response = false;
	m_channel_changed = false;

	if (m_fd != -1)
	{
		close(m_fd);
		m_fd = -1;
	}

	stopTimerResetConnect();
	stopTimerKeepAliveMessage();
}

void satipRTSP::timeoutConnect(void *ptr)
{
	DEBUG(MSG_MAIN, "timeoutConnect\n");
	satipRTSP* _this = static_cast<satipRTSP*>(ptr);
	_this->resetConnect();
}

void satipRTSP::timeoutKeepAlive(void *ptr)
{
	DEBUG(MSG_MAIN, "timeoutKeepAlive\n");
	satipRTSP* _this = static_cast<satipRTSP*>(ptr);
	_this->sendRequest(RTSP_REQUEST_OPTION);
}

void satipRTSP::timeoutStreamInfo(void *ptr)
{
	DEBUG(MSG_MAIN, "timeoutStreamInfo\n");
	satipRTSP* _this = static_cast<satipRTSP*>(ptr);
	_this->sendRequest(RTSP_REQUEST_DESCRIBE);
}

int satipRTSP::connectToServer()
{
	int fd;
	int flags;
	int error;
	struct addrinfo hints;
	struct addrinfo *result;
	struct addrinfo *rp;

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;    /* IPv4 or IPv6 */
	hints.ai_socktype = SOCK_STREAM; 
	hints.ai_flags = 0;
	hints.ai_protocol = 0;

	error = getaddrinfo(m_host.c_str(), m_port.c_str(), &hints, &result);
	if (error) 
	{
		if (error == EAI_SYSTEM)
		{
			ERROR(MSG_NET, "getaddrinfo: %s\n", strerror(error));
		}
		else
		{
			ERROR(MSG_NET, "getaddrinfo: %s\n", gai_strerror(error));
		}
		return RTSP_ERROR;
	}

	for (rp = result; rp != NULL; rp = rp->ai_next) 
	{
		fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (fd == -1)
			continue; /* error, try next..*/

		flags=fcntl(fd,F_GETFL,0);
		if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1 ) /* set non blocking mode */
		{
			close(fd);
			continue;
		}

		if (connect(fd, rp->ai_addr, rp->ai_addrlen) != -1)
		{
			break; /* connect ok */
		}
		else if (errno == EINPROGRESS)
		{
			break; /* connecting */
		}

		close(fd); /* connect fail */
	}

	if (rp == NULL) {           
		DEBUG(MSG_NET, "Could not connect\n");
		freeaddrinfo(result);   
		return RTSP_ERROR;
	}

	if (m_satip_config->isTcpData()) {
		int len = m_satip_config->getRtpNetBufferSizeMB() * 1024 * 1024;
		if (setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &len, sizeof(len)))
			WARN(MSG_MAIN, "unable to set TCP buffer (force) size to %d\n", len);

		if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &len, sizeof(len)))
			WARN(MSG_MAIN, "unable to set TCP buffer size to %d\n", len);

		socklen_t sl = sizeof(int);
		if (!getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &len, &sl))
			DEBUG(MSG_DATA, "TCP buffer size is %d bytes\n", len);
	}

	freeaddrinfo(result);

	m_fd = fd;

	return RTSP_OK;
}

int satipRTSP::handleResponse()
{
	static bool overrun = false;
	const size_t availableSize = m_rx_data_len - m_rx_data_wpos;
	int res = RTSP_ERROR;

	if (availableSize > 0) {
		if (overrun) {
			DEBUG(MSG_NET,"RTSP Recovered from buffer overrun: len %d  wpos %d\n", m_rx_data_len, m_rx_data_wpos);
			overrun = false;
		}
		const ssize_t read_data = recv(m_fd, m_rx_data.get() + m_rx_data_wpos, availableSize, 0);
		if (read_data == -1) {
			DEBUG(MSG_NET,"RTSP recv: %d\n", read_data);
			return RTSP_ERROR;
		}
		m_rx_data_wpos += read_data;
	} else if (!overrun) {
		DEBUG(MSG_NET,"RTSP buffer overrun: len %d  wpos %d\n", m_rx_data_len, m_rx_data_wpos);
		overrun = true;
	}

	// Are we expecting a response? then find it
	if (m_wait_response) {
		std::string_view::size_type begin = 0;
		std::string_view msg(m_rx_data.get(), m_rx_data_wpos);
		const std::string_view response_view = findRTSPResponse(msg, begin);
		if (!response_view.empty()) {
			const std::string response(response_view.data(), response_view.size());
			DEBUG(MSG_NET,"RTSP rx data: \n%s\n", response.c_str());
			if (m_satip_config->isTcpData()) {
				// Cut away RTSP response from embedded data
				char *beginPtr = m_rx_data.get() + begin;
				std::memmove(beginPtr, beginPtr + response.size(), m_rx_data_wpos - response.size());
				m_rx_data_wpos -= response.size();
			} else {
				m_rx_data_wpos = 0;
			}
			const int res_code = std::stoi(findParameter(response, "RTSP/", ' '));
			if (res_code == 200) {
				switch(m_rtsp_request) {
					case RTSP_REQUEST_NONE:
						DEBUG(MSG_NET,"response skip..\n");
						break;
					case RTSP_REQUEST_OPTION:
						res = handleResponseOption(response);
						break;
					case RTSP_REQUEST_SETUP:
						res = handleResponseSetup(response);
						if (m_channel_changed) {
							m_rx_data_wpos = 0;
							m_channel_changed = false;
						}
						break;
					case RTSP_REQUEST_PLAY:
						res = handleResponsePlay(response);
						if (m_channel_changed) {
							m_rx_data_wpos = 0;
							m_channel_changed = false;
						}
						break;
					case RTSP_REQUEST_TEARDOWN:
						res = handleResponseTeardown(response);
						break;
					default:
						break;
				}
			} else {
				DEBUG(MSG_MAIN, "No RTSP Response code 200\n");
				res = RTSP_ERROR;
			}
			switch(res) {
				case RTSP_RESPONSE_COMPLETE:
					DEBUG(MSG_MAIN, "RTSP_RESPONSE_COMPLETE\n");
					stopTimerResetConnect();
					break;
				case RTSP_ERROR:
					DEBUG(MSG_MAIN, "RTSP_ERROR\n");
					resetConnect();
					return res;
				default:
					DEBUG(MSG_MAIN, "RTSP_DEFAULT (%d)!!\n", res);
					break;
			}
			m_wait_response = false;
			m_rtsp_request = RTSP_REQUEST_NONE;
		}
	}

	// Are we expecting embedded RTP data? then extract it
	if (m_satip_config->isTcpData() && !m_channel_changed) {
		// extract embedded data
		size_t dataSize = m_rx_data_wpos;
		if (dataSize >= 4) {
			auto *ptr = reinterpret_cast<unsigned char *>(m_rx_data.get());
			if (!(ptr[0] == '$' && ptr[4] == 0x80 && (ptr[1] == 0x00 || ptr[1] == 0x01))) {
				DEBUG(MSG_NET,"RTP/TCP not aligned correctly.. maybe try to find it?\n");
				// try to recover here?
			}
			while (ptr[0] == '$' && ptr[4] == 0x80 && (ptr[1] == 0x00 || ptr[1] == 0x01)) {
				if (dataSize < 4) {
					res = RTSP_OK;
					break;
				}
				const size_t packetSize = 4 + ((ptr[2] << 8) + ptr[3]);
				if (dataSize < packetSize) {
					res = RTSP_OK;
					break;
				}
				m_rtp->rtpTcpData(ptr, packetSize);
				ptr += packetSize;
				dataSize -= packetSize;
			}
			if (reinterpret_cast<unsigned char *>(m_rx_data.get()) != ptr) {
				startTimerResetConnect(4000);
				m_rx_data_wpos = dataSize;
				if (dataSize > 0) {
					memmove(m_rx_data.get(), ptr, dataSize);
				}
			}
		}
	}
	return res;
}

int satipRTSP::handleResponseSetup(const std::string& msg)
{
	/*
	RTSP/1.0 200 OK
	CSeq: 1
	Session: 12345678;timeout=60
	Transport: RTP/AVP;unicast;client_port=1400-1401;source=192.168.128.5;server_port=1528-1529
	com.ses.streamID: 1
	<CRLF>
	*/

	/*
	RTSP/1.0 200 OK
	CSeq: 1
	Session: 0521595368;timeout=60
	Transport: RTP/AVP;unicast;client_port=46938-46939;server_port=8000-8001
	com.ses.streamID: 1
	*/
	m_rtsp_session_id = findParameter(msg, "Session", ':');
	if (m_rtsp_session_id.empty()) {
		return RTSP_ERROR;
	}

	// get timeout
	std::string timeout = findParameter(msg, "timeout", '=');
	if (!timeout.empty()) {
		m_rtsp_timeout = std::stoi(timeout);
	}

	// get stream id
	std::string id = findParameter(msg, "com.ses.streamID", ':');
	if (id.empty()) {
		return RTSP_ERROR;
	}
	m_rtsp_stream_id = std::stoi(id);

	DEBUG(MSG_MAIN, "Session ID : %s\n", m_rtsp_session_id.c_str());
	DEBUG(MSG_MAIN, "Timeout : %d\n", m_rtsp_timeout);
	DEBUG(MSG_MAIN, "Stream ID : %d\n", m_rtsp_stream_id);
	return RTSP_RESPONSE_COMPLETE;
}

int satipRTSP::handleResponsePlay(const std::string& /*msg*/)
{
	return RTSP_RESPONSE_COMPLETE;
}

int satipRTSP::handleResponseOption(const std::string& /*msg*/)
{
	return RTSP_RESPONSE_COMPLETE;
}

int satipRTSP::handleResponseTeardown(const std::string& /*msg*/)
{
	return RTSP_RESPONSE_COMPLETE;
}

int satipRTSP::handleResponseDescribe(const std::string& /*msg*/)
{
	return RTSP_RESPONSE_COMPLETE;
}

int satipRTSP::sendRequest(int request)
{
	if (m_wait_response)
	{
		//DEBUG(MSG_MAIN, "Now waitng response, skip sendRequest(%d)\n", request);
		return RTSP_FAILED;
	}

	stopTimerKeepAliveMessage(); // before send request, stop keep alive message timer.

	int res = RTSP_ERROR;
	switch(request)
	{
		case RTSP_REQUEST_OPTION:
			res = sendOption();
			break;

		case RTSP_REQUEST_SETUP:
			res = sendSetup();
			break;

		case RTSP_REQUEST_PLAY:
			res = sendPlay();
			break;

		case RTSP_REQUEST_TEARDOWN:
			res = sendTearDown();
			break;

		case RTSP_REQUEST_DESCRIBE:
			res = sendDescribe();
			break;

		default:
			DEBUG(MSG_MAIN, "Unknown request!\n");
			break;
	}

	if (res == RTSP_OK)
	{
		m_wait_response = true;
		m_rtsp_request = request;
		startTimerResetConnect(6000); // server connect timer start
	}
	else
	{
		switch(request)
		{
			case RTSP_REQUEST_OPTION:
				DEBUG(MSG_MAIN, "RTSP SEND OPTION is failed! try reconnect.\n");
				break;

			case RTSP_REQUEST_SETUP:
				DEBUG(MSG_MAIN, "RTSP SEND SETUP is failed! try reconnect.\n");
				break;

			case RTSP_REQUEST_PLAY:
				DEBUG(MSG_MAIN, "RTSP SEND PLAY is failed! try reconnect.\n");
				break;

			case RTSP_REQUEST_TEARDOWN:
				DEBUG(MSG_MAIN, "RTSP SEND TEARDOWN is failed! try reconnect.\n");
				break;

			case RTSP_REQUEST_DESCRIBE:
				DEBUG(MSG_MAIN, "RTSP SEND DESCRIBE is failed! try reconnect.\n");
				break;
			default:
				break;
		}
		resetConnect();
	}

	return res;
}

int satipRTSP::sendSetup()
{
	std::string tx_data;
	std::ostringstream oss_tx_data;
	/* 
	str = SETUP rtsp://192.168.100.101/?src=1&freq=10202&pol=v&msys=dvbs&sr=27500&fec=34&pids=0,16,25,104 RTSP/1.0
	CSeq: 1
	Transport: RTP/AVP;unicast;client_port=1400-1401
	<CRLF>
	*/

	oss_tx_data << "SETUP rtsp://" << m_host << ":" << m_port << "/";
	if (m_rtsp_stream_id != -1)
		oss_tx_data << "stream=" << m_rtsp_stream_id;

	const auto [data, channelChanged] = m_satip_config->getSetupData();
	m_channel_changed = channelChanged;
	oss_tx_data << data << " RTSP/1.0\r\n";
	oss_tx_data << "CSeq: " << m_rtsp_cseq++ << "\r\n";
	if (!m_rtsp_session_id.empty())
		oss_tx_data << "Session: " << m_rtsp_session_id << "\r\n";

	if (m_satip_config->isTcpData()) {
		oss_tx_data << "Transport: RTP/AVP/TCP;interleaved=0-1\r\n";
	} else {
		int rtp_port = m_rtp->get_rtp_port();
		oss_tx_data << "Transport: RTP/AVP;unicast;client_port=" << rtp_port << "-" << rtp_port+1 << "\r\n";
	}
	oss_tx_data << "User-Agent: " << user_agent << "\r\n";
	oss_tx_data << "\r\n";

	tx_data = oss_tx_data.str();

	DEBUG(MSG_MAIN, "SETUP DATA : \n%s\n", tx_data.c_str());

	if (send(m_fd, tx_data.c_str(), tx_data.size(), 0) < 0) {
		return RTSP_ERROR;
	}

	return RTSP_OK;
}

int satipRTSP::sendPlay()
{
	std::string tx_data;
	std::ostringstream oss_tx_data;
	/*
	PLAY rtsp://192.168.128.5/stream=1 RTSP/1.0
	CSeq: 2
	Session: 12345678
	<CRLF>
	*/

	/*
	PLAY rtsp://192.168.178.57:554/stream=5?src=1&freq=11538&pol=v&ro=0.35&msys=dvbs&mtype=qpsk&plts=off&sr=22000
	&fec=56&pids=0,611,621,631 RTSP/1.0
	CSeq:5
	Session:21a15c02c1ee244
	*/

	if (m_rtsp_stream_id == -1 || m_rtsp_session_id.empty())
	{
		ERROR(MSG_MAIN, "PLAY : stream_id and session_id are required..\n");
		return RTSP_ERROR;
	}
	const auto [data, channelChanged] = m_satip_config->getPlayData();
	m_channel_changed = channelChanged;
	oss_tx_data << "PLAY rtsp://" << m_host << ":" << m_port << "/" << "stream=" << m_rtsp_stream_id;
	oss_tx_data << data << " RTSP/1.0\r\n";
	oss_tx_data << "CSeq: " << m_rtsp_cseq++ << "\r\n";
	oss_tx_data << "Session: " << m_rtsp_session_id << "\r\n";
	oss_tx_data << "User-Agent: " << user_agent << "\r\n";
	oss_tx_data << "\r\n";

	tx_data = oss_tx_data.str();

	DEBUG(MSG_MAIN, "PLAY DATA : \n%s\n", tx_data.c_str());

	if (send(m_fd, tx_data.c_str(), tx_data.size(), 0) < 0) {
		return RTSP_ERROR;
	}

	return RTSP_OK;
}

int satipRTSP::sendOption()
{
	std::string tx_data;
	std::ostringstream oss_tx_data;
	/*
	OPTIONS rtsp://192.168.178.57:554/ RTSP/1.0
	CSeq:5
	Session:2180f601c42957d
	<CRLF>
	*/

	if (m_rtsp_stream_id == -1 || m_rtsp_session_id.empty())
	{
		ERROR(MSG_MAIN, "OPTIONS : stream_id and session_id are required..");
		return RTSP_ERROR;
	}

	oss_tx_data << "OPTIONS rtsp://" << m_host << ":" << m_port << "/";
//	if (m_rtsp_stream_id != -1)
//	oss_tx_data << "stream=" << m_rtsp_stream_id;
	oss_tx_data << " RTSP/1.0\r\n";

	oss_tx_data << "CSeq: " << m_rtsp_cseq++ << "\r\n";
//	if (!m_rtsp_session_id.empty())
	oss_tx_data << "Session: " << m_rtsp_session_id << "\r\n";
	oss_tx_data << "User-Agent: " << user_agent << "\r\n";
	oss_tx_data << "\r\n";

	tx_data = oss_tx_data.str();

	DEBUG(MSG_MAIN, "OPTIONS DATA : \n%s\n", tx_data.c_str());

	if (send(m_fd, tx_data.c_str(), tx_data.size(), 0) < 0)
		return RTSP_ERROR;

	return RTSP_OK;
}

int satipRTSP::sendTearDown()
{
	std::string tx_data;
	std::ostringstream oss_tx_data;

	/*
	TEARDOWN rtsp://192.168.178.57:554/stream=2 RTSP/1.0
	CSeq:5
	Session:2180f601c42957d
	<CRLF>
	*/

	if (m_rtsp_stream_id == -1 || m_rtsp_session_id.empty())
	{
		ERROR(MSG_MAIN, "TEARDOWN : stream_id and session_id are required..");
		return RTSP_ERROR;
	}

	oss_tx_data << "TEARDOWN rtsp://" << m_host << ":" << m_port << "/stream=" << m_rtsp_stream_id << " RTSP/1.0\r\n";
	oss_tx_data << "CSeq: " << m_rtsp_cseq++ << "\r\n";
	oss_tx_data << "Session: " << m_rtsp_session_id << "\r\n";
	oss_tx_data << "User-Agent: " << user_agent << "\r\n";
	oss_tx_data << "\r\n";

	tx_data = oss_tx_data.str();

	if (send(m_fd, tx_data.c_str(), tx_data.size(), 0) < 0)
		return RTSP_ERROR;

	return RTSP_OK;
}

int satipRTSP::sendDescribe()
{
	std::string tx_data;
	std::ostringstream oss_tx_data;

	/*
	DESCRIBE rtsp://192.168.128.5/
	CSeq: 5
	Accept: application/sdp
	<CRLF>
	*/

	oss_tx_data << "DESCRIBE rtsp://" << m_host << ":" << m_port << "/";
	if (m_rtsp_stream_id != -1)
		oss_tx_data << "stream=" << m_rtsp_stream_id;
	oss_tx_data << " RTSP/1.0\r\n";
	oss_tx_data << "CSeq: " << m_rtsp_cseq++ << "\r\n";
	oss_tx_data << "Accept: application/sdp" << "\r\n";
	oss_tx_data << "User-Agent: " << user_agent << "\r\n";
	oss_tx_data << "\r\n";

	tx_data = oss_tx_data.str();

	if (send(m_fd, tx_data.c_str(), tx_data.size(), 0) < 0)
		return RTSP_ERROR;

	return RTSP_OK;
}

void satipRTSP::handleRTSPStatus()
{
	switch(m_rtsp_status)
	{
		case RTSP_STATUS_CONFIG_WAITING:
			//DEBUG(MSG_MAIN, "RTSP STATUS : RTSP_STATUS_CONFIG_WAITING\n");
			if (m_satip_config->getChannelStatus() == CONFIG_STATUS_CHANNEL_CHANGED)
			{
				if (connectToServer() == RTSP_OK)
				{
					m_rtsp_status = RTSP_STATUS_SERVER_CONNECTING;
					startTimerResetConnect(5000);
				}
				else
				{
					DEBUG(MSG_MAIN, "Connect to server failed!\n");
				}
			}
			break;

		case RTSP_STATUS_SERVER_CONNECTING: // connected to serverm check if server ready to send RTSP requests.
			DEBUG(MSG_MAIN, "RTSP STATUS : RTSP_STATUS_SERVER_CONNECTING\n");
			break;

		case RTSP_STATUS_SESSION_ESTABLISHING: // SETUP request sended, wait POLLIN event to receive SETUP response.
			DEBUG(MSG_MAIN, "RTSP STATUS : RTSP_STATUS_SESSION_ESTABLISHING\n");
			if (sendRequest(RTSP_REQUEST_SETUP) == RTSP_OK) // send ok
			{
				;// rtp thread start;
			}
			break;

		case RTSP_STATUS_SESSION_PLAYING: // PLAY request sended, wait POLLIN event to receive PLAY response.
			sendRequest(RTSP_REQUEST_PLAY);
			break;

		case RTSP_STATUS_SESSION_TRANSMITTING:
			{
				t_channel_status channel_status = m_satip_config->getChannelStatus();
				t_pid_status pid_status = m_satip_config->getPidStatus();

				if (channel_status == CONFIG_STATUS_CHANNEL_CHANGED) {
					DEBUG(MSG_MAIN, "CHANNEL STATUS : CONFIG_STATUS_CHANNEL_CHANGED\n");
				}

				if (pid_status == CONFIG_STATUS_PID_CHANGED) {
					DEBUG(MSG_MAIN, "PID STATUS : CONFIG_STATUS_PID_CHANGED\n");
				}

				if ((channel_status == CONFIG_STATUS_CHANNEL_CHANGED) || (pid_status == CONFIG_STATUS_PID_CHANGED))
				{
					if (sendRequest(RTSP_REQUEST_PLAY) == RTSP_OK) // send ok
					{
						m_rtsp_status = RTSP_STATUS_SESSION_PLAYING;
					}			
				}
				else if (channel_status == CONFIG_STATUS_CHANNEL_INVALID)
				{
					if (sendRequest(RTSP_REQUEST_TEARDOWN) == RTSP_OK) // send ok
					{
						m_rtsp_status = RTSP_STATUS_SESSION_TEARDOWNING;
					}
				}
				else // (channel_status == CONFIG_STATUS_CHANNEL_STABLE) || (pid_status == CONFIG_STATUS_PID_STATIONARY)
				{
					if (!m_timer_keep_alive->isActive())
						startTimerKeepAliveMessage();
				}
				break;
			}

		case RTSP_STATUS_SESSION_TEARDOWNING: // TEARDOWN request sended, wait POLLIN event to receive TEARDOWN response.
			DEBUG(MSG_MAIN, "RTSP STATUS : RTSP_STATUS_SESSION_TEARDOWNING\n");
			break;

		default:
			break;
	}
}

short satipRTSP::getPollEvent()
{
	short events = 0;
	switch(m_rtsp_status)
	{
		case RTSP_STATUS_CONFIG_WAITING:
			if (m_satip_config->isTcpData()) // TCP data mode
				events = POLLIN | POLLHUP;
			else
				events = 0; // no poll
			break;

		case RTSP_STATUS_SERVER_CONNECTING: // connected to serverm check if server ready to send RTSP requests.
			events = POLLOUT | POLLHUP;
			break;

		case RTSP_STATUS_SESSION_ESTABLISHING: // SETUP request sended, check read to receive SETUP response.
			events = POLLIN | POLLHUP;
			break;

		case RTSP_STATUS_SESSION_PLAYING: // PLAY request sended, check read to receive PLAY response.
			events = POLLIN | POLLHUP;
			break;

		case RTSP_STATUS_SESSION_TRANSMITTING:
			if (m_rtsp_request == RTSP_REQUEST_OPTION ||  // keep alive message
			    m_satip_config->isTcpData()) {            // or TCP data mode
				events = POLLIN | POLLHUP;
			} else {
				events = 0; // no poll
			}
			break;

		case RTSP_STATUS_SESSION_TEARDOWNING: // TEARDOWN request sended, check read to receive TEARDOWN response.
			events = POLLIN | POLLHUP;
			break;

		default:
			break;
	}

//	DEBUG(MSG_MAIN, "getPollEvent return %d (RTSP STATUS : %d)\n", (int)events, m_rtsp_request);

	return events;
}

void satipRTSP::handlePollEvents(short events)
{
//	DEBUG(MSG_MAIN, "handlePollEvents.\n");
	if (events & POLLHUP)
	{
		DEBUG(MSG_MAIN, "RTSP socket disconnedted, retry connection.\n");
		resetConnect();
		return;
	}

	switch(m_rtsp_status)
	{
		case RTSP_STATUS_CONFIG_WAITING:
			DEBUG(MSG_MAIN, "RTSP STATUS : RTSP_STATUS_CONFIG_WAITING\n");
			DEBUG(MSG_MAIN, "BUG, this message muse not be shown!!!\n"); // no poll
			break;

		case RTSP_STATUS_SERVER_CONNECTING: // connected to server, check if server ready to send RTSP requests.
			DEBUG(MSG_MAIN, "RTSP STATUS : RTSP_STATUS_SERVER_CONNECTING\n");
			if (events & POLLOUT)
			{
				stopTimerResetConnect();
				m_rtsp_status = RTSP_STATUS_SESSION_ESTABLISHING;
			}
			break;

		case RTSP_STATUS_SESSION_ESTABLISHING: // SETUP request sended, check read to receive SETUP response and send PLAY.
			DEBUG(MSG_MAIN, "RTSP STATUS : RTSP_STATUS_SESSION_ESTABLISHING\n");
			if ((events & POLLIN))
			{
				int res = handleResponse();
				if (res == RTSP_RESPONSE_COMPLETE) // handle response SETUP
				{
					m_rtsp_status = RTSP_STATUS_SESSION_PLAYING;
				}
			}
			break;

		case RTSP_STATUS_SESSION_PLAYING: // PLAY request sended, check read to receive PLAY response.
			if ((events & POLLIN))
			{
				int res = handleResponse();
				if (res == RTSP_RESPONSE_COMPLETE) // handle response PLAY
				{
					m_rtsp_status = RTSP_STATUS_SESSION_TRANSMITTING;
				}
			}
			break;

		case RTSP_STATUS_SESSION_TRANSMITTING:
			if ((events & POLLIN))
			{
				handleResponse(); // handle response OPTION
			}
			break;

		case RTSP_STATUS_SESSION_TEARDOWNING: // TEARDOWN request sended, check read to receive TEARDOWN response.
			DEBUG(MSG_MAIN, "RTSP STATUS : RTSP_STATUS_SESSION_TEARDOWNING\n");
			if ((events & POLLIN))
			{
				int res = handleResponse(); // handle response TEARDOWN
				if (res == RTSP_RESPONSE_COMPLETE)
				{
					resetConnect();
				}
			}
			break;

		default:
			break;
	}
}

int satipRTSP::getPollTimeout() 
{ 
	return m_satip_timer.getNextTimerBegin(); 
}

void satipRTSP::handleNextTimer()
{
	m_satip_timer.callNextTimer();
}

void satipRTSP::startTimerResetConnect(long timeout)
{
//	DEBUG(MSG_MAIN, "startTimerResetConnect (%ld)\n", timeout);
	m_timer_reset_connect->start(timeout, true);
}

void satipRTSP::stopTimerResetConnect()
{
	DEBUG(MSG_MAIN, "stopTimerResetConnect\n");
	m_timer_reset_connect->stop();
}

void satipRTSP::startTimerKeepAliveMessage()
{
	const long timeout = (m_rtsp_timeout - 5) * 1000;
	m_timer_keep_alive->start(timeout, true);
	DEBUG(MSG_MAIN, "startTimerKeepAliveMessage (%ld)\n", timeout);
}

void satipRTSP::stopTimerKeepAliveMessage()
{
	DEBUG(MSG_MAIN, "stopTimerKeepAliveMessage\n");
	m_timer_keep_alive->stop();
}

int satipRTSP::getRtspSocketFd()
{
	return m_fd;
}

