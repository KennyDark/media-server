#include "rtsp-client.h"
#include "rtsp-parser.h"
#include "sys/sock.h"
#include "aio-socket.h"
#include "url.h"
#include "sdp.h"

#define USER_AGENT "Netposa RTSP Lib"
#define RTP_PORT_BASE 30000

struct rtsp_context
{
	unsigned int cseq;
	char* session;
	socket_t sock;

	socket_t rtp[2];
	int server_port[2];

	char req[1024];
	char *ptr;
	int bytes;

	char ip[64];
	int port;

	void* parser;
	void *sdp; // sdp parser
};

static int rtsp_connect(struct rtsp_context* ctx)
{
	if(socket_invalid != ctx->sock && 0!=socket_readable(ctx->sock))
	{
		socket_close(ctx->sock);
		ctx->sock = socket_invalid;
	}

	if(socket_invalid == ctx->sock)
	{
		return socket_connect_ipv4_by_time(ctx->sock, ctx->ip, ctx->port, 5000);
	}

	return 0;
}

static int rtsp_disconnect(struct rtsp_context* ctx)
{
	if(socket_invalid != ctx->sock)
	{
		socket_close(ctx->sock);
		ctx->sock = socket_invalid;
	}
	return 0;
}

static int rtsp_send(struct rtsp_context* ctx, const void* p, int n)
{
	int r;
	r = rtsp_connect(ctx);
	if(0 != r)
		return r;

	r = socket_send(ctx->sock, p, n, 0);
	if(r != n)
		return -1;

	return 0;
}

static int rtsp_send_v(struct rtsp_context* ctx, const socket_bufvec_t* v, int n)
{
	int r;
	r = rtsp_connect(ctx);
	if(0 != r)
		return r;

	r = socket_send_v(ctx->sock, v, n, 0);
	if(r != n)
		return -1;

	return 0;
}

static int rtsp_recv(struct rtsp_context* ctx)
{
	int r, s;
	char p[1024];
	
	r = rtsp_connect(ctx);
	if(0 != r)
		return r;

	rtsp_parser_clear(ctx->parser);

	s = 1;
	do
	{
		r = socket_recv(ctx->sock, p, sizeof(p), 0);
		if(r <= 0)
			break;

		s = rtsp_parser_input(ctx->parser, p, r);
	} while(0 != s);

	if(0 == s)
	{
		return 200 == rtsp_get_status_code(ctx->parser) ? 0 : -1;
	}
	return -1;
}

static int rtsp_request(struct rtsp_context* ctx, const void* p, int n)
{
	int r = rtsp_send(ctx, p, n);
	if(0 != r)
		return r;

	r = rtsp_recv(ctx);
	if(0 != r)
		return r;

	return 0;
}

static int rtsp_request_v(struct rtsp_context* ctx, const socket_bufvec_t *vec, int n)
{
	int r = rtsp_send_v(ctx, vec, n);
	if(0 != r)
		return r;

	r = rtsp_recv(ctx);
	if(0 != r)
		return r;

	return 0;
}

static socket_t rtp_udp_socket(int port)
{
	socket_t s;
	s = socket_udp();
	if(socket_invalid == s)
		return s;

	if(0 == socket_bind_any(s, port))
		return s;

	socket_close(s);
	return socket_invalid;
}

static int rtsp_create_rtp_socket(socket_t *rtp, socket_t *rtcp, int *port)
{
	int i;
	socket_t sock[2];
	assert(0 == RTP_PORT_BASE % 2);
	srand((unsigned int)time(NULL));

	do
	{
		i = rand() % 30000;
		i = i/2*2 + RTP_PORT_BASE;

		sock[0] = rtp_udp_socket(i);
		if(socket_invalid == sock[0])
			continue;

		sock[1] = rtp_udp_socket(i + 1);
		if(socket_invalid == sock[1])
		{
			socket_close(sock[0]);
			continue;
		}

		*rtp = sock[0];
		*rtcp = sock[1];
		*port = i;
		return 0;

	} while(socket_invalid!=sock[0] && socket_invalid!=sock[1]);

	return -1;
}

/*
C->S: 
DESCRIBE rtsp://server.example.com/fizzle/foo RTSP/1.0
CSeq: 312
Accept: application/sdp, application/rtsl, application/mheg

S->C: 
RTSP/1.0 200 OK
CSeq: 312
Date: 23 Jan 1997 15:35:06 GMT
Content-Type: application/sdp
Content-Length: 376

v=0
o=mhandley 2890844526 2890842807 IN IP4 126.16.64.4
s=SDP Seminar
i=A Seminar on the session description protocol
u=http://www.cs.ucl.ac.uk/staff/M.Handley/sdp.03.ps
e=mjh@isi.edu (Mark Handley)
c=IN IP4 224.2.17.12/127
t=2873397496 2873404696
a=recvonly
m=audio 3456 RTP/AVP 0
m=video 2232 RTP/AVP 31
m=whiteboard 32416 UDP WB
a=orient:portrait
*/
static int rtsp_describe(struct rtsp_context* ctx)
{
	int r;
	snprintf(ctx->req, sizeof(ctx->req), 
		"DESCRIBE %s RTSP/1.0\r\n"
		"CSeq: %u\r\n"
		"Accept: application/sdp\r\n"
		"User-Agent: %s\r\n"
		"\r\n", 
		ctx->uri, ctx->cseq++, USER_AGENT);

	r = rtsp_request(ctx, ctx->req, strlen(ctx->req));
	if(0 == r)
	{
		if(ctx->sdp) sdp_destroy(ctx->sdp);
		ctx->sdp = sdp_parse(rtsp_get_content());
		if(!ctx->sdp)
			return -1;
	}
	return r;
}

/*
C->S:
SETUP rtsp://example.com/foo/bar/baz.rm RTSP/1.0
CSeq: 302
Transport: RTP/AVP;unicast;client_port=4588-4589

S->C: 
RTSP/1.0 200 OK
CSeq: 302
Date: 23 Jan 1997 15:35:06 GMT
Session: 47112344
Transport: RTP/AVP;unicast; client_port=4588-4589;server_port=6256-6257
*/

enum { 
	RTSP_TRANSPORT_UNKNOWN = 0, 
	RTSP_TRANSPORT_UNICAST, 
	RTSP_TRANSPORT_MULTICAST,
};

enum {
	RTSP_TRANSPORT_RTP,
	RTSP_TRANSPORT_RAW,
};

enum {
	RTSP_TRANSPORT_UDP, 
	RTSP_TRANSPORT_TCP
};

struct rtsp_header_transport
{
	int transport; // RTP/RAW
	int lower_transport; // TCP/UDP, RTP/AVP default UDP
	int multicast; // unicast/multicast, default multicast
	char* destination;
	char* source;
	int layer; // rtsp setup response only
	char* mode; // PLAY/RECORD, default PLAY, rtsp setup response only
	int append; // use with RECORD mode only, rtsp setup response only
	int interleaved1, interleaved2; // rtsp setup response only
	int ttl; // multicast only
	int port1, port2; // RTP only
	int client_port1, client_port2; // unicast RTP/RTCP port pair, RTP only
	int server_port1, server_port2; // unicast RTP/RTCP port pair, RTP only
	char* ssrc; // RTP only
};

static int rtsp_header_parse_transport(const char* s, struct rtsp_header_transport* t)
{
	t->transport = RTSP_TRANSPORT_RTP;
	t->lower_transport = RTSP_TRANSPORT_UDP;

	while(1)
	{
		if(0 == strnicmp("RTP/AVP/TCP;", s))
		{
			t->transport = RTSP_TRANSPORT_RTP;
			t->lower_transport = RTSP_TRANSPORT_TCP;
		}
		else if(0 == strnicmp("RAW/RAW/UDP;", s))
		{
			t->transport = RTSP_TRANSPORT_RAW;
			t->lower_transport = RTSP_TRANSPORT_UDP;
		}
		else if(0 == strnicmp("destination=", s, 12))
		{
			t->destination = strdup(s+12);
		}
		else if(1 == sscanf("ttl=%u", &t->ttl))
		{
		}
		else if(2 == sscanf("client_port=%hu-%hu", &t->client_port1, &t->client_port2))
		{
		}
		else if(1 == sscanf("client_port=%hu", &t->client_port1))
		{
		}
	}
}

static int rtsp_setup(struct rtsp_context* ctx, const char* sdp)
{
	int r;
	int port;
	void *sdp;
	char ip[64];
	char *session;
	char *transport;
	assert(0 == port % 2);

	if(sdp)
	{
		if(ctx->sdp) sdp_destroy(ctx->sdp);
		ctx->sdp = sdp_parse(sdp);
	}
	if(!ctx->sdp)
	{
		return -1;
	}

	if(0 != sdp_connection_get_address(ctx->sdp, ip, sizeof(ip)))
	{
		sdp_destroy(ctx->sdp);
		return -1;
	}

	assert(sdp_connection_get_network(ctx->sdp)==SDP_C_NETWORK_IN);
	switch(sdp_connection_get_addrtype(ctx->sdp))
	{
	case SDP_C_ADDRESS_IP4:
		if(0 != stricmp(ip, ctx->ip))
		{
			rtsp_disconnect(ctx);
			strncpy(ctx->ip, ip, sizeof(ctx->ip)-1);
			// TODO: parse port
		}
		break;

	case SDP_C_ADDRESS_IP6:
		assert(0);
		break;

	default:
		assert(0);
	}

	if(0 != rtsp_create_rtp_socket(&ctx->rtp[0], &ctx->rtp[1], &port))
	{
		printf("rtsp_create_rtp_socket error.\n");
		return -1;
	}

	// 224.0.0.0 ~ 239.255.255.255
	snprintf(ctx->req, sizeof(ctx->req), 
		"SETUP %s RTSP/1.0\r\n"
		"CSeq: %u\r\n"
		"Transport: RTP/AVP;unicast;client_port=%u-%u\r\n"
		"User-Agent: %s\r\n"
		"\r\n", 
		ctx->uri, ctx->cseq++, port, port+1, USER_AGENT);

	r = rtsp_request(ctx, ctx->req, strlen(ctx->req));
	if(0 == r)
	{
		session = rtsp_get_header_by_name(ctx->parser, "Session");
		if(!session)
		{
			return -1;
		}

		transport = rtsp_get_header_by_name(ctx->parser, "Transport");
		if(!transport)
		{
			return -1;
		}

		if(0 != rtsp_header_parse_transport(transport))
		{
			return -1;
		}

		ctx->session = strdup(session);
	}
	return r;
}

/*
ANNOUNCE rtsp://server.example.com/fizzle/foo RTSP/1.0
CSeq: 312
Date: 23 Jan 1997 15:35:06 GMT
Session: 47112344
Content-Type: application/sdp
Content-Length: 332

v=0
o=mhandley 2890844526 2890845468 IN IP4 126.16.64.4
s=SDP Seminar
i=A Seminar on the session description protocol
u=http://www.cs.ucl.ac.uk/staff/M.Handley/sdp.03.ps
e=mjh@isi.edu (Mark Handley)
c=IN IP4 224.2.17.12/127
t=2873397496 2873404696
a=recvonly
m=audio 3456 RTP/AVP 0
m=video 2232 RTP/AVP 31
*/
static int rtsp_announce(struct rtsp_context* ctx, const char* sdp)
{
	snprintf(ctx->req, sizeof(ctx->req), 
		"ANNOUNCE %s RTSP/1.0\r\n"
		"CSeq: %u\r\n"
		"Session: %s\r\n"
		"Content-Type: application/sdp\r\n"
		"Content-Length: %d\r\n"
		"\r\n", 
		ctx->uri, ctx->cseq++, ctx->session, strlen(sdp));

	socket_bufvec_t vec[2];
	socket_setbufvec(vec, 0, ctx->req, strlen(ctx->req));
	socket_setbufvec(vec, 1, sdp, strlen(sdp));
	return rtsp_request_v(ctx, vec, 2);
}

/*
C->S: 
TEARDOWN rtsp://example.com/fizzle/foo RTSP/1.0
CSeq: 892
Session: 12345678

S->C: 
RTSP/1.0 200 OK
CSeq: 892
*/
static int rtsp_teardown(struct rtsp_context* ctx)
{
	int r;
	snprintf(ctx->req, sizeof(ctx->req), 
		"TEARDOWN %s RTSP/1.0\r\n"
		"CSeq: %u\r\n"
		"Session: %s\r\n"
		"\r\n", 
		ctx->uri, ctx->cseq++, ctx->session);

	r = rtsp_request(ctx, ctx->req, strlen(ctx->req));
	if(0 == r)
	{
	}
	return r;
}

static int rtsp_teardown_reply(struct rtsp_context* rtsp)
{
}

void* rtsp_open(const char* uri)
{
	int r;
	void *parser;
	struct rtsp_context *ctx;
	ctx = (struct rtsp_context*)malloc(sizeof(struct rtsp_context*));
	if(!ctx)
		return NULL;

	parser = url_parse(uri);
	if(!parser)
	{
		free(ctx);
		return NULL;
	}

	memset(ctx, 0, sizeof(struct rtsp_context));
	strncpy(ctx->ip, url_gethost(parser), sizeof(ctx->ip)-1);
	ctx->port = url_getport(parser);
	if(0 == ctx->port)
		ctx->port = 554; // default

	r = rtsp_describe(ctx);
	r = rtsp_setup(ctx, NULL);
	return ctx;
}

int rtsp_close(void* rtsp)
{
}

/*
PLAY rtsp://audio.example.com/audio RTSP/1.0
CSeq: 835
Session: 12345678
Range: npt=10-15

C->S: 
PLAY rtsp://audio.example.com/twister.en RTSP/1.0
CSeq: 833
Session: 12345678
Range: smpte=0:10:20-;time=19970123T153600Z

S->C: 
RTSP/1.0 200 OK
CSeq: 833
Date: 23 Jan 1997 15:35:06 GMT
Range: smpte=0:10:22-;time=19970123T153600Z

C->S: 
PLAY rtsp://audio.example.com/meeting.en RTSP/1.0
CSeq: 835
Session: 12345678
Range: clock=19961108T142300Z-19961108T143520Z

S->C: 
RTSP/1.0 200 OK
CSeq: 835
Date: 23 Jan 1997 15:35:06 GMT
*/
int rtsp_play(void* rtsp)
{
	int r, n;
	assert(0 == port % 2);
	snprintf(rtsp->req, sizeof(rtsp->req), 
		"PLAY %s RTSP/1.0\r\n"
		"CSeq: %u\r\n"
		"Session: %s\r\n"
		"Range: npt=%u-%u\r\n"
		"\r\n", 
		uri, rtsp->cseq++, port, port+1);

	n = strlen(rtsp->req);
	return rtsp->send(rtsp->req, n);
}

/*
C->S: 
PAUSE rtsp://example.com/fizzle/foo RTSP/1.0
CSeq: 834
Session: 12345678

S->C: 
RTSP/1.0 200 OK
CSeq: 834
Date: 23 Jan 1997 15:35:06 GMT
*/
// A PAUSE request discards all queued PLAY requests. However, the pause
// point in the media stream MUST be maintained. A subsequent PLAY
// request without Range header resumes from the pause point.
static int rtsp_pause_reply(void* rtsp)
{
}

int rtsp_pause(void* rtsp)
{
	int r, n;

	struct rtsp_context *ctx;
	ctx = (struct rtsp_context*)rtsp;
	snprintf(ctx->req, sizeof(ctx->req), 
		"PAUSE %s RTSP/1.0\r\n"
		"CSeq: %u\r\n"
		"Session: %s\r\n"
		"\r\n", 
		ctx->uri, ctx->cseq++, ctx->session);

	n = strlen(ctx->req);
	r = ctx->send(ctx->req, n);
	return r;
}