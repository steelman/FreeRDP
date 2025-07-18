/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Remote Desktop Gateway (RDG)
 *
 * Copyright 2015 Denis Vincent <dvincent@devolutions.net>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdint.h>

#include <freerdp/config.h>

#include "../settings.h"

#include <winpr/assert.h>
#include <winpr/cast.h>

#include <winpr/crt.h>
#include <winpr/synch.h>
#include <winpr/print.h>
#include <winpr/stream.h>
#include <winpr/winsock.h>
#include <winpr/cred.h>

#include <freerdp/log.h>
#include <freerdp/error.h>
#include <freerdp/utils/ringbuffer.h>
#include <freerdp/utils/smartcardlogon.h>

#include "rdg.h"
#include "websocket.h"
#include "../credssp_auth.h"
#include "../proxy.h"
#include "../rdp.h"
#include "../../crypto/opensslcompat.h"
#include "rpc_fault.h"
#include "../utils.h"

#define TAG FREERDP_TAG("core.gateway.rdg")

#define AUTH_PKG NEGO_SSP_NAME

/* HTTP channel response fields present flags. */
#define HTTP_CHANNEL_RESPONSE_FIELD_CHANNELID 0x1
#define HTTP_CHANNEL_RESPONSE_OPTIONAL 0x2
#define HTTP_CHANNEL_RESPONSE_FIELD_UDPPORT 0x4

/* HTTP extended auth. */
#define HTTP_EXTENDED_AUTH_NONE 0x0
#define HTTP_EXTENDED_AUTH_SC 0x1         /* Smart card authentication. */
#define HTTP_EXTENDED_AUTH_PAA 0x02       /* Pluggable authentication. */
#define HTTP_EXTENDED_AUTH_SSPI_NTLM 0x04 /* NTLM extended authentication. */
#define HTTP_EXTENDED_AUTH_BEARER 0x08    /* HTTP Bearer authentication. */

/* HTTP packet types. */
#define PKT_TYPE_HANDSHAKE_REQUEST 0x1
#define PKT_TYPE_HANDSHAKE_RESPONSE 0x2
#define PKT_TYPE_EXTENDED_AUTH_MSG 0x3
#define PKT_TYPE_TUNNEL_CREATE 0x4
#define PKT_TYPE_TUNNEL_RESPONSE 0x5
#define PKT_TYPE_TUNNEL_AUTH 0x6
#define PKT_TYPE_TUNNEL_AUTH_RESPONSE 0x7
#define PKT_TYPE_CHANNEL_CREATE 0x8
#define PKT_TYPE_CHANNEL_RESPONSE 0x9
#define PKT_TYPE_DATA 0xA
#define PKT_TYPE_SERVICE_MESSAGE 0xB
#define PKT_TYPE_REAUTH_MESSAGE 0xC
#define PKT_TYPE_KEEPALIVE 0xD
#define PKT_TYPE_CLOSE_CHANNEL 0x10
#define PKT_TYPE_CLOSE_CHANNEL_RESPONSE 0x11

/* HTTP tunnel auth fields present flags. */
// #define HTTP_TUNNEL_AUTH_FIELD_SOH 0x1

/* HTTP tunnel auth response fields present flags. */
#define HTTP_TUNNEL_AUTH_RESPONSE_FIELD_REDIR_FLAGS 0x1
#define HTTP_TUNNEL_AUTH_RESPONSE_FIELD_IDLE_TIMEOUT 0x2
#define HTTP_TUNNEL_AUTH_RESPONSE_FIELD_SOH_RESPONSE 0x4

/* HTTP tunnel packet fields present flags. */
#define HTTP_TUNNEL_PACKET_FIELD_PAA_COOKIE 0x1
// #define HTTP_TUNNEL_PACKET_FIELD_REAUTH 0x2

/* HTTP tunnel response fields present flags. */
#define HTTP_TUNNEL_RESPONSE_FIELD_TUNNEL_ID 0x1
#define HTTP_TUNNEL_RESPONSE_FIELD_CAPS 0x2
#define HTTP_TUNNEL_RESPONSE_FIELD_SOH_REQ 0x4
#define HTTP_TUNNEL_RESPONSE_FIELD_CONSENT_MSG 0x10

/* HTTP capability type enumeration. */
#define HTTP_CAPABILITY_TYPE_QUAR_SOH 0x1
#define HTTP_CAPABILITY_IDLE_TIMEOUT 0x2
#define HTTP_CAPABILITY_MESSAGING_CONSENT_SIGN 0x4
#define HTTP_CAPABILITY_MESSAGING_SERVICE_MSG 0x8
#define HTTP_CAPABILITY_REAUTH 0x10
#define HTTP_CAPABILITY_UDP_TRANSPORT 0x20

typedef struct
{
	TRANSFER_ENCODING httpTransferEncoding;
	BOOL isWebsocketTransport;
	union context
	{
		http_encoding_chunked_context chunked;
		websocket_context* websocket;
	} context;
} rdg_http_encoding_context;

struct rdp_rdg
{
	rdpContext* context;
	BOOL attached;
	BIO* frontBio;
	rdpTls* tlsIn;
	rdpTls* tlsOut;
	rdpCredsspAuth* auth;
	HttpContext* http;
	CRITICAL_SECTION writeSection;

	UUID guid;

	int state;
	UINT16 packetRemainingCount;
	UINT16 reserved1;
	int timeout;
	UINT16 extAuth;
	UINT16 reserved2;
	rdg_http_encoding_context transferEncoding;

	SmartcardCertInfo* smartcard;
	wLog* log;
};

enum
{
	RDG_CLIENT_STATE_INITIAL,
	RDG_CLIENT_STATE_HANDSHAKE,
	RDG_CLIENT_STATE_TUNNEL_CREATE,
	RDG_CLIENT_STATE_TUNNEL_AUTHORIZE,
	RDG_CLIENT_STATE_CHANNEL_CREATE,
	RDG_CLIENT_STATE_OPENED,
};

#pragma pack(push, 1)

typedef struct rdg_packet_header
{
	UINT16 type;
	UINT16 reserved;
	UINT32 packetLength;
} RdgPacketHeader;

#pragma pack(pop)

typedef struct
{
	UINT32 code;
	const char* name;
} t_flag_mapping;

static const t_flag_mapping tunnel_response_fields_present[] = {
	{ HTTP_TUNNEL_RESPONSE_FIELD_TUNNEL_ID, "HTTP_TUNNEL_RESPONSE_FIELD_TUNNEL_ID" },
	{ HTTP_TUNNEL_RESPONSE_FIELD_CAPS, "HTTP_TUNNEL_RESPONSE_FIELD_CAPS" },
	{ HTTP_TUNNEL_RESPONSE_FIELD_SOH_REQ, "HTTP_TUNNEL_RESPONSE_FIELD_SOH_REQ" },
	{ HTTP_TUNNEL_RESPONSE_FIELD_CONSENT_MSG, "HTTP_TUNNEL_RESPONSE_FIELD_CONSENT_MSG" }
};

static const t_flag_mapping channel_response_fields_present[] = {
	{ HTTP_CHANNEL_RESPONSE_FIELD_CHANNELID, "HTTP_CHANNEL_RESPONSE_FIELD_CHANNELID" },
	{ HTTP_CHANNEL_RESPONSE_OPTIONAL, "HTTP_CHANNEL_RESPONSE_OPTIONAL" },
	{ HTTP_CHANNEL_RESPONSE_FIELD_UDPPORT, "HTTP_CHANNEL_RESPONSE_FIELD_UDPPORT" }
};

static const t_flag_mapping tunnel_authorization_response_fields_present[] = {
	{ HTTP_TUNNEL_AUTH_RESPONSE_FIELD_REDIR_FLAGS, "HTTP_TUNNEL_AUTH_RESPONSE_FIELD_REDIR_FLAGS" },
	{ HTTP_TUNNEL_AUTH_RESPONSE_FIELD_IDLE_TIMEOUT,
	  "HTTP_TUNNEL_AUTH_RESPONSE_FIELD_IDLE_TIMEOUT" },
	{ HTTP_TUNNEL_AUTH_RESPONSE_FIELD_SOH_RESPONSE,
	  "HTTP_TUNNEL_AUTH_RESPONSE_FIELD_SOH_RESPONSE" }
};

static const t_flag_mapping extended_auth[] = {
	{ HTTP_EXTENDED_AUTH_NONE, "HTTP_EXTENDED_AUTH_NONE" },
	{ HTTP_EXTENDED_AUTH_SC, "HTTP_EXTENDED_AUTH_SC" },
	{ HTTP_EXTENDED_AUTH_PAA, "HTTP_EXTENDED_AUTH_PAA" },
	{ HTTP_EXTENDED_AUTH_SSPI_NTLM, "HTTP_EXTENDED_AUTH_SSPI_NTLM" }
};

static const t_flag_mapping capabilities_enum[] = {
	{ HTTP_CAPABILITY_TYPE_QUAR_SOH, "HTTP_CAPABILITY_TYPE_QUAR_SOH" },
	{ HTTP_CAPABILITY_IDLE_TIMEOUT, "HTTP_CAPABILITY_IDLE_TIMEOUT" },
	{ HTTP_CAPABILITY_MESSAGING_CONSENT_SIGN, "HTTP_CAPABILITY_MESSAGING_CONSENT_SIGN" },
	{ HTTP_CAPABILITY_MESSAGING_SERVICE_MSG, "HTTP_CAPABILITY_MESSAGING_SERVICE_MSG" },
	{ HTTP_CAPABILITY_REAUTH, "HTTP_CAPABILITY_REAUTH" },
	{ HTTP_CAPABILITY_UDP_TRANSPORT, "HTTP_CAPABILITY_UDP_TRANSPORT" }
};

static const char* flags_to_string(UINT32 flags, const t_flag_mapping* map, size_t elements)
{
	static char buffer[1024] = { 0 };
	char fields[12] = { 0 };

	for (size_t x = 0; x < elements; x++)
	{
		const t_flag_mapping* cur = &map[x];

		if ((cur->code & flags) != 0)
			winpr_str_append(cur->name, buffer, sizeof(buffer), "|");
	}

	(void)sprintf_s(fields, ARRAYSIZE(fields), " [%04" PRIx32 "]", flags);
	winpr_str_append(fields, buffer, sizeof(buffer), NULL);
	return buffer;
}

static const char* channel_response_fields_present_to_string(UINT16 fieldsPresent)
{
	return flags_to_string(fieldsPresent, channel_response_fields_present,
	                       ARRAYSIZE(channel_response_fields_present));
}

static const char* tunnel_response_fields_present_to_string(UINT16 fieldsPresent)
{
	return flags_to_string(fieldsPresent, tunnel_response_fields_present,
	                       ARRAYSIZE(tunnel_response_fields_present));
}

static const char* tunnel_authorization_response_fields_present_to_string(UINT16 fieldsPresent)
{
	return flags_to_string(fieldsPresent, tunnel_authorization_response_fields_present,
	                       ARRAYSIZE(tunnel_authorization_response_fields_present));
}

static const char* extended_auth_to_string(UINT16 auth)
{
	if (auth == HTTP_EXTENDED_AUTH_NONE)
		return "HTTP_EXTENDED_AUTH_NONE [0x0000]";

	return flags_to_string(auth, extended_auth, ARRAYSIZE(extended_auth));
}

static const char* capabilities_enum_to_string(UINT32 capabilities)
{
	return flags_to_string(capabilities, capabilities_enum, ARRAYSIZE(capabilities_enum));
}

static BOOL rdg_read_http_unicode_string(wLog* log, wStream* s, const WCHAR** string,
                                         UINT16* lengthInBytes)
{
	UINT16 strLenBytes = 0;
	size_t rem = Stream_GetRemainingLength(s);

	/* Read length of the string */
	if (!Stream_CheckAndLogRequiredLengthWLog(log, s, 4))
	{
		WLog_Print(log, WLOG_ERROR, "Could not read stream length, only have %" PRIuz " bytes",
		           rem);
		return FALSE;
	}
	Stream_Read_UINT16(s, strLenBytes);

	/* Remember position of our string */
	const WCHAR* str = Stream_ConstPointer(s);

	/* seek past the string - if this fails something is wrong */
	if (!Stream_SafeSeek(s, strLenBytes))
	{
		WLog_Print(log, WLOG_ERROR,
		           "Could not read stream data, only have %" PRIuz " bytes, expected %" PRIu16,
		           rem - 4, strLenBytes);
		return FALSE;
	}

	/* return the string data (if wanted) */
	if (string)
		*string = str;
	if (lengthInBytes)
		*lengthInBytes = strLenBytes;

	return TRUE;
}

static BOOL rdg_write_chunked(BIO* bio, wStream* sPacket)
{
	size_t len = 0;
	int status = 0;
	wStream* sChunk = NULL;
	char chunkSize[11];
	(void)sprintf_s(chunkSize, sizeof(chunkSize), "%" PRIXz "\r\n", Stream_Length(sPacket));
	sChunk = Stream_New(NULL, strnlen(chunkSize, sizeof(chunkSize)) + Stream_Length(sPacket) + 2);

	if (!sChunk)
		return FALSE;

	Stream_Write(sChunk, chunkSize, strnlen(chunkSize, sizeof(chunkSize)));
	Stream_Write(sChunk, Stream_Buffer(sPacket), Stream_Length(sPacket));
	Stream_Write(sChunk, "\r\n", 2);
	Stream_SealLength(sChunk);
	len = Stream_Length(sChunk);

	if (len > INT_MAX)
	{
		Stream_Free(sChunk, TRUE);
		return FALSE;
	}

	ERR_clear_error();
	status = BIO_write(bio, Stream_Buffer(sChunk), (int)len);
	Stream_Free(sChunk, TRUE);

	if (status != (SSIZE_T)len)
		return FALSE;

	return TRUE;
}

static BOOL rdg_write_packet(rdpRdg* rdg, wStream* sPacket)
{
	if (rdg->transferEncoding.isWebsocketTransport)
		return websocket_context_write_wstream(rdg->transferEncoding.context.websocket,
		                                       rdg->tlsOut->bio, sPacket, WebsocketBinaryOpcode);

	return rdg_write_chunked(rdg->tlsIn->bio, sPacket);
}

static int rdg_socket_read(BIO* bio, BYTE* pBuffer, size_t size,
                           rdg_http_encoding_context* encodingContext)
{
	WINPR_ASSERT(encodingContext != NULL);
	if (size > INT32_MAX)
		return -1;

	if (encodingContext->isWebsocketTransport)
		return websocket_context_read(encodingContext->context.websocket, bio, pBuffer, size);

	switch (encodingContext->httpTransferEncoding)
	{
		case TransferEncodingIdentity:
			ERR_clear_error();
			return BIO_read(bio, pBuffer, (int)size);
		case TransferEncodingChunked:
			return http_chuncked_read(bio, pBuffer, size, &encodingContext->context.chunked);
		default:
			return -1;
	}
}

static BOOL rdg_shall_abort(rdpRdg* rdg)
{
	WINPR_ASSERT(rdg);
	return freerdp_shall_disconnect_context(rdg->context);
}

static BOOL rdg_read_all(rdpContext* context, rdpTls* tls, BYTE* buffer, size_t size,
                         rdg_http_encoding_context* transferEncoding)
{
	size_t readCount = 0;
	BYTE* pBuffer = buffer;

	while (readCount < size)
	{
		if (freerdp_shall_disconnect_context(context))
			return FALSE;

		int status = rdg_socket_read(tls->bio, pBuffer, size - readCount, transferEncoding);
		if (status <= 0)
		{
			if (!BIO_should_retry(tls->bio))
				return FALSE;

			Sleep(10);
			continue;
		}

		readCount += WINPR_ASSERTING_INT_CAST(uint32_t, status);
		pBuffer += WINPR_ASSERTING_INT_CAST(uint32_t, status);
	}

	return TRUE;
}

static wStream* rdg_receive_packet(rdpRdg* rdg)
{
	const size_t header = sizeof(RdgPacketHeader);
	size_t packetLength = 0;
	wStream* s = Stream_New(NULL, 1024);

	if (!s)
		return NULL;

	if (!rdg_read_all(rdg->context, rdg->tlsOut, Stream_Buffer(s), header, &rdg->transferEncoding))
	{
		Stream_Free(s, TRUE);
		return NULL;
	}

	Stream_Seek(s, 4);
	Stream_Read_UINT32(s, packetLength);

	if ((packetLength > INT_MAX) || !Stream_EnsureCapacity(s, packetLength) ||
	    (packetLength < header))
	{
		Stream_Free(s, TRUE);
		return NULL;
	}

	if (!rdg_read_all(rdg->context, rdg->tlsOut, Stream_Buffer(s) + header, packetLength - header,
	                  &rdg->transferEncoding))
	{
		Stream_Free(s, TRUE);
		return NULL;
	}

	Stream_SetLength(s, packetLength);
	return s;
}

static BOOL rdg_send_handshake(rdpRdg* rdg)
{
	BOOL status = FALSE;
	wStream* s = Stream_New(NULL, 14);

	if (!s)
		return FALSE;

	Stream_Write_UINT16(s, PKT_TYPE_HANDSHAKE_REQUEST); /* Type (2 bytes) */
	Stream_Write_UINT16(s, 0);                          /* Reserved (2 bytes) */
	Stream_Write_UINT32(s, 14);                         /* PacketLength (4 bytes) */
	Stream_Write_UINT8(s, 1);                           /* VersionMajor (1 byte) */
	Stream_Write_UINT8(s, 0);                           /* VersionMinor (1 byte) */
	Stream_Write_UINT16(s, 0);                          /* ClientVersion (2 bytes), must be 0 */
	Stream_Write_UINT16(s, rdg->extAuth);               /* ExtendedAuthentication (2 bytes) */
	Stream_SealLength(s);
	status = rdg_write_packet(rdg, s);
	Stream_Free(s, TRUE);

	if (status)
	{
		rdg->state = RDG_CLIENT_STATE_HANDSHAKE;
	}

	return status;
}

static BOOL rdg_send_extauth_sspi(rdpRdg* rdg)
{
	wStream* s = NULL;
	BOOL status = 0;
	UINT32 packetSize = 8 + 4 + 2;

	WINPR_ASSERT(rdg);

	const SecBuffer* authToken = credssp_auth_get_output_buffer(rdg->auth);
	if (!authToken)
		return FALSE;
	packetSize += authToken->cbBuffer;

	s = Stream_New(NULL, packetSize);

	if (!s)
		return FALSE;

	Stream_Write_UINT16(s, PKT_TYPE_EXTENDED_AUTH_MSG); /* Type (2 bytes) */
	Stream_Write_UINT16(s, 0);                          /* Reserved (2 bytes) */
	Stream_Write_UINT32(s, packetSize);                 /* PacketLength (4 bytes) */
	Stream_Write_UINT32(s, ERROR_SUCCESS);              /* Error code */
	Stream_Write_UINT16(s, (UINT16)authToken->cbBuffer);
	Stream_Write(s, authToken->pvBuffer, authToken->cbBuffer);

	Stream_SealLength(s);
	status = rdg_write_packet(rdg, s);
	Stream_Free(s, TRUE);

	return status;
}

static BOOL rdg_send_tunnel_request(rdpRdg* rdg)
{
	wStream* s = NULL;
	BOOL status = 0;
	UINT32 packetSize = 16;
	UINT16 fieldsPresent = 0;
	WCHAR* PAACookie = NULL;
	size_t PAACookieLen = 0;
	const UINT32 capabilities = HTTP_CAPABILITY_TYPE_QUAR_SOH |
	                            HTTP_CAPABILITY_MESSAGING_CONSENT_SIGN |
	                            HTTP_CAPABILITY_MESSAGING_SERVICE_MSG;

	if (rdg->extAuth == HTTP_EXTENDED_AUTH_PAA)
	{
		PAACookie =
		    ConvertUtf8ToWCharAlloc(rdg->context->settings->GatewayAccessToken, &PAACookieLen);

		if (!PAACookie || (PAACookieLen > UINT16_MAX / sizeof(WCHAR)))
		{
			free(PAACookie);
			return FALSE;
		}

		PAACookieLen += 1; /* include \0 */
		packetSize += 2 + (UINT32)(PAACookieLen) * sizeof(WCHAR);
		fieldsPresent = HTTP_TUNNEL_PACKET_FIELD_PAA_COOKIE;
	}

	s = Stream_New(NULL, packetSize);

	if (!s)
	{
		free(PAACookie);
		return FALSE;
	}

	Stream_Write_UINT16(s, PKT_TYPE_TUNNEL_CREATE); /* Type (2 bytes) */
	Stream_Write_UINT16(s, 0);                      /* Reserved (2 bytes) */
	Stream_Write_UINT32(s, packetSize);             /* PacketLength (4 bytes) */
	Stream_Write_UINT32(s, capabilities);           /* CapabilityFlags (4 bytes) */
	Stream_Write_UINT16(s, fieldsPresent);          /* FieldsPresent (2 bytes) */
	Stream_Write_UINT16(s, 0);                      /* Reserved (2 bytes), must be 0 */

	if (PAACookie)
	{
		Stream_Write_UINT16(s, (UINT16)PAACookieLen * sizeof(WCHAR)); /* PAA cookie string length */
		Stream_Write_UTF16_String(s, PAACookie, PAACookieLen);
	}

	Stream_SealLength(s);
	status = rdg_write_packet(rdg, s);
	Stream_Free(s, TRUE);
	free(PAACookie);

	if (status)
	{
		rdg->state = RDG_CLIENT_STATE_TUNNEL_CREATE;
	}

	return status;
}

static BOOL rdg_send_tunnel_authorization(rdpRdg* rdg)
{
	wStream* s = NULL;
	BOOL status = 0;
	WINPR_ASSERT(rdg);
	size_t clientNameLen = 0;
	WCHAR* clientName = freerdp_settings_get_string_as_utf16(
	    rdg->context->settings, FreeRDP_ClientHostname, &clientNameLen);

	clientNameLen++; // length including terminating '\0'

	const size_t packetSize = 12ull + clientNameLen * sizeof(WCHAR);
	if (!clientName || (clientNameLen >= UINT16_MAX / sizeof(WCHAR)) || (packetSize > UINT32_MAX))
	{
		free(clientName);
		return FALSE;
	}

	s = Stream_New(NULL, packetSize);

	if (!s)
	{
		free(clientName);
		return FALSE;
	}

	Stream_Write_UINT16(s, PKT_TYPE_TUNNEL_AUTH);                  /* Type (2 bytes) */
	Stream_Write_UINT16(s, 0);                                     /* Reserved (2 bytes) */
	Stream_Write_UINT32(s, (UINT32)packetSize);                    /* PacketLength (4 bytes) */
	Stream_Write_UINT16(s, 0);                                     /* FieldsPresent (2 bytes) */
	Stream_Write_UINT16(s, (UINT16)clientNameLen * sizeof(WCHAR)); /* Client name string length */
	Stream_Write_UTF16_String(s, clientName, clientNameLen);
	Stream_SealLength(s);
	status = rdg_write_packet(rdg, s);
	Stream_Free(s, TRUE);
	free(clientName);

	if (status)
	{
		rdg->state = RDG_CLIENT_STATE_TUNNEL_AUTHORIZE;
	}

	return status;
}

static BOOL rdg_send_channel_create(rdpRdg* rdg)
{
	wStream* s = NULL;
	BOOL status = FALSE;
	WCHAR* serverName = NULL;
	size_t serverNameLen = 0;

	WINPR_ASSERT(rdg);
	serverName = freerdp_settings_get_string_as_utf16(rdg->context->settings,
	                                                  FreeRDP_ServerHostname, &serverNameLen);

	serverNameLen++; // length including terminating '\0'
	const size_t packetSize = 16ull + serverNameLen * sizeof(WCHAR);
	if (!serverName || (serverNameLen >= UINT16_MAX / sizeof(WCHAR)) || (packetSize > UINT32_MAX))
		goto fail;

	s = Stream_New(NULL, packetSize);

	if (!s)
		goto fail;

	Stream_Write_UINT16(s, PKT_TYPE_CHANNEL_CREATE); /* Type (2 bytes) */
	Stream_Write_UINT16(s, 0);                       /* Reserved (2 bytes) */
	Stream_Write_UINT32(s, (UINT32)packetSize);      /* PacketLength (4 bytes) */
	Stream_Write_UINT8(s, 1);                        /* Number of resources. (1 byte) */
	Stream_Write_UINT8(s, 0);                        /* Number of alternative resources (1 byte) */
	Stream_Write_UINT16(s,
	                    (UINT16)rdg->context->settings->ServerPort); /* Resource port (2 bytes) */
	Stream_Write_UINT16(s, 3);                                       /* Protocol number (2 bytes) */
	Stream_Write_UINT16(s, (UINT16)serverNameLen * sizeof(WCHAR));
	Stream_Write_UTF16_String(s, serverName, serverNameLen);
	Stream_SealLength(s);
	status = rdg_write_packet(rdg, s);
fail:
	free(serverName);
	Stream_Free(s, TRUE);

	if (status)
		rdg->state = RDG_CLIENT_STATE_CHANNEL_CREATE;

	return status;
}

static BOOL rdg_set_auth_header(rdpCredsspAuth* auth, HttpRequest* request)
{
	const SecBuffer* authToken = credssp_auth_get_output_buffer(auth);
	char* base64AuthToken = NULL;

	if (authToken)
	{
		if (authToken->cbBuffer > INT_MAX)
			return FALSE;

		base64AuthToken = crypto_base64_encode(authToken->pvBuffer, authToken->cbBuffer);
	}

	if (base64AuthToken)
	{
		BOOL rc = http_request_set_auth_scheme(request, credssp_auth_pkg_name(auth)) &&
		          http_request_set_auth_param(request, base64AuthToken);
		free(base64AuthToken);

		if (!rc)
			return FALSE;
	}

	return TRUE;
}

static wStream* rdg_build_http_request(rdpRdg* rdg, const char* method,
                                       TRANSFER_ENCODING transferEncoding)
{
	wStream* s = NULL;
	HttpRequest* request = NULL;
	const char* uri = NULL;

	if (!rdg || !method)
		return NULL;

	uri = http_context_get_uri(rdg->http);
	request = http_request_new();

	if (!request)
		return NULL;

	if (!http_request_set_method(request, method) || !http_request_set_uri(request, uri))
		goto out;

	if (rdg->auth)
	{
		if (!rdg_set_auth_header(rdg->auth, request))
			goto out;
	}

	else if (rdg->extAuth == HTTP_EXTENDED_AUTH_BEARER)
	{
		http_request_set_auth_scheme(request, "Bearer");
		http_request_set_auth_param(request, rdg->context->settings->GatewayHttpExtAuthBearer);
	}

	http_request_set_transfer_encoding(request, transferEncoding);

	s = http_request_write(rdg->http, request);
out:
	http_request_free(request);

	if (s)
		Stream_SealLength(s);

	return s;
}

static BOOL rdg_recv_auth_token(wLog* log, rdpCredsspAuth* auth, HttpResponse* response)
{
	size_t len = 0;
	const char* token64 = NULL;
	size_t authTokenLength = 0;
	BYTE* authTokenData = NULL;
	SecBuffer authToken = { 0 };
	long StatusCode = 0;
	int rc = 0;

	if (!auth || !response)
		return FALSE;

	StatusCode = http_response_get_status_code(response);
	switch (StatusCode)
	{
		case HTTP_STATUS_DENIED:
		case HTTP_STATUS_OK:
		case HTTP_STATUS_SWITCH_PROTOCOLS:
			break;
		default:
			http_response_log_error_status(log, WLOG_WARN, response);
			return FALSE;
	}

	token64 = http_response_get_auth_token(response, credssp_auth_pkg_name(auth));

	if (!token64)
		return FALSE;

	len = strlen(token64);

	crypto_base64_decode(token64, len, &authTokenData, &authTokenLength);

	if (authTokenLength && authTokenData && (authTokenLength <= UINT32_MAX))
	{
		authToken.pvBuffer = authTokenData;
		authToken.cbBuffer = (UINT32)authTokenLength;
		credssp_auth_take_input_buffer(auth, &authToken);
	}
	else
		free(authTokenData);

	rc = credssp_auth_authenticate(auth);
	if (rc < 0)
		return FALSE;

	return TRUE;
}

static BOOL rdg_skip_seed_payload(rdpContext* context, rdpTls* tls, size_t lastResponseLength,
                                  rdg_http_encoding_context* transferEncoding)
{
	BYTE seed_payload[10] = { 0 };
	const size_t size = sizeof(seed_payload);

	/* Per [MS-TSGU] 3.3.5.1 step 4, after final OK response RDG server sends
	 * random "seed" payload of limited size. In practice it's 10 bytes.
	 */
	if (lastResponseLength < size)
	{
		if (!rdg_read_all(context, tls, seed_payload, size - lastResponseLength, transferEncoding))
		{
			return FALSE;
		}
	}

	return TRUE;
}

static BOOL rdg_process_handshake_response(rdpRdg* rdg, wStream* s)
{
	UINT32 errorCode = 0;
	UINT16 serverVersion = 0;
	UINT16 extendedAuth = 0;
	BYTE verMajor = 0;
	BYTE verMinor = 0;
	const char* error = NULL;
	WLog_Print(rdg->log, WLOG_DEBUG, "Handshake response received");

	if (rdg->state != RDG_CLIENT_STATE_HANDSHAKE)
	{
		return FALSE;
	}

	if (!Stream_CheckAndLogRequiredLengthWLog(rdg->log, s, 10))
		return FALSE;

	Stream_Read_UINT32(s, errorCode);
	Stream_Read_UINT8(s, verMajor);
	Stream_Read_UINT8(s, verMinor);
	Stream_Read_UINT16(s, serverVersion);
	Stream_Read_UINT16(s, extendedAuth);
	error = rpc_error_to_string(errorCode);
	WLog_Print(rdg->log, WLOG_DEBUG,
	           "errorCode=%s, verMajor=%" PRId8 ", verMinor=%" PRId8 ", serverVersion=%" PRId16
	           ", extendedAuth=%s",
	           error, verMajor, verMinor, serverVersion, extended_auth_to_string(extendedAuth));

	if (FAILED((HRESULT)errorCode))
	{
		WLog_Print(rdg->log, WLOG_ERROR, "Handshake error %s", error);
		freerdp_set_last_error_log(rdg->context, errorCode);
		return FALSE;
	}

	if (rdg->extAuth == HTTP_EXTENDED_AUTH_SSPI_NTLM)
		return rdg_send_extauth_sspi(rdg);

	return rdg_send_tunnel_request(rdg);
}

static BOOL rdg_process_tunnel_response_optional(rdpRdg* rdg, wStream* s, UINT16 fieldsPresent)
{
	if (fieldsPresent & HTTP_TUNNEL_RESPONSE_FIELD_TUNNEL_ID)
	{
		/* Seek over tunnelId (4 bytes) */
		if (!Stream_SafeSeek(s, 4))
		{
			WLog_Print(rdg->log, WLOG_ERROR, "Short tunnelId, got %" PRIuz ", expected 4",
			           Stream_GetRemainingLength(s));
			return FALSE;
		}
	}

	if (fieldsPresent & HTTP_TUNNEL_RESPONSE_FIELD_CAPS)
	{
		UINT32 caps = 0;
		if (!Stream_CheckAndLogRequiredLengthWLog(rdg->log, s, 4))
			return FALSE;

		Stream_Read_UINT32(s, caps);
		WLog_Print(rdg->log, WLOG_DEBUG, "capabilities=%s", capabilities_enum_to_string(caps));
	}

	if (fieldsPresent & HTTP_TUNNEL_RESPONSE_FIELD_SOH_REQ)
	{
		/* Seek over nonce (20 bytes) */
		if (!Stream_SafeSeek(s, 20))
		{
			WLog_Print(rdg->log, WLOG_ERROR, "Short nonce, got %" PRIuz ", expected 20",
			           Stream_GetRemainingLength(s));
			return FALSE;
		}

		/* Read serverCert */
		if (!rdg_read_http_unicode_string(rdg->log, s, NULL, NULL))
		{
			WLog_Print(rdg->log, WLOG_ERROR, "Failed to read server certificate");
			return FALSE;
		}
	}

	if (fieldsPresent & HTTP_TUNNEL_RESPONSE_FIELD_CONSENT_MSG)
	{
		const WCHAR* msg = NULL;
		UINT16 msgLenBytes = 0;
		rdpContext* context = rdg->context;

		WINPR_ASSERT(context);
		WINPR_ASSERT(context->instance);

		/* Read message string and invoke callback */
		if (!rdg_read_http_unicode_string(rdg->log, s, &msg, &msgLenBytes))
		{
			WLog_Print(rdg->log, WLOG_ERROR, "Failed to read consent message");
			return FALSE;
		}

		return IFCALLRESULT(TRUE, context->instance->PresentGatewayMessage, context->instance,
		                    GATEWAY_MESSAGE_CONSENT, TRUE, TRUE, msgLenBytes, msg);
	}

	return TRUE;
}

static BOOL rdg_process_tunnel_response(rdpRdg* rdg, wStream* s)
{
	UINT16 serverVersion = 0;
	UINT16 fieldsPresent = 0;
	UINT32 errorCode = 0;
	const char* error = NULL;
	WLog_Print(rdg->log, WLOG_DEBUG, "Tunnel response received");

	if (rdg->state != RDG_CLIENT_STATE_TUNNEL_CREATE)
	{
		return FALSE;
	}

	if (!Stream_CheckAndLogRequiredLengthWLog(rdg->log, s, 10))
		return FALSE;

	Stream_Read_UINT16(s, serverVersion);
	Stream_Read_UINT32(s, errorCode);
	Stream_Read_UINT16(s, fieldsPresent);
	Stream_Seek_UINT16(s); /* reserved */
	error = rpc_error_to_string(errorCode);
	WLog_Print(rdg->log, WLOG_DEBUG, "serverVersion=%" PRId16 ", errorCode=%s, fieldsPresent=%s",
	           serverVersion, error, tunnel_response_fields_present_to_string(fieldsPresent));

	if (FAILED((HRESULT)errorCode))
	{
		WLog_Print(rdg->log, WLOG_ERROR, "Tunnel creation error %s", error);
		freerdp_set_last_error_log(rdg->context, errorCode);
		return FALSE;
	}

	if (!rdg_process_tunnel_response_optional(rdg, s, fieldsPresent))
		return FALSE;

	return rdg_send_tunnel_authorization(rdg);
}

static BOOL rdg_process_tunnel_authorization_response(rdpRdg* rdg, wStream* s)
{
	UINT32 errorCode = 0;
	UINT16 fieldsPresent = 0;
	const char* error = NULL;
	WLog_Print(rdg->log, WLOG_DEBUG, "Tunnel authorization received");

	if (rdg->state != RDG_CLIENT_STATE_TUNNEL_AUTHORIZE)
	{
		return FALSE;
	}

	if (!Stream_CheckAndLogRequiredLengthWLog(rdg->log, s, 8))
		return FALSE;

	Stream_Read_UINT32(s, errorCode);
	Stream_Read_UINT16(s, fieldsPresent);
	Stream_Seek_UINT16(s); /* reserved */
	error = rpc_error_to_string(errorCode);
	WLog_Print(rdg->log, WLOG_DEBUG, "errorCode=%s, fieldsPresent=%s", error,
	           tunnel_authorization_response_fields_present_to_string(fieldsPresent));

	/* [MS-TSGU] 3.7.5.2.7 */
	if (errorCode != S_OK && errorCode != E_PROXY_QUARANTINE_ACCESSDENIED)
	{
		WLog_Print(rdg->log, WLOG_ERROR, "Tunnel authorization error %s", error);
		freerdp_set_last_error_log(rdg->context, errorCode);
		return FALSE;
	}

	if (fieldsPresent & HTTP_TUNNEL_AUTH_RESPONSE_FIELD_REDIR_FLAGS)
	{
		UINT32 redirFlags = 0;
		if (!Stream_CheckAndLogRequiredCapacityWLog(rdg->log, s, 4))
			return FALSE;
		Stream_Read_UINT32(s, redirFlags);

		rdpContext* context = rdg->context;
		if (!utils_apply_gateway_policy(rdg->log, context, redirFlags, "RDG"))
			return FALSE;
	}

	if (fieldsPresent & HTTP_TUNNEL_AUTH_RESPONSE_FIELD_IDLE_TIMEOUT)
	{
		UINT32 idleTimeout = 0;
		if (!Stream_CheckAndLogRequiredCapacityWLog(rdg->log, s, 4))
			return FALSE;
		Stream_Read_UINT32(s, idleTimeout);
		WLog_Print(rdg->log, WLOG_DEBUG, "[IDLE_TIMEOUT] idleTimeout=%" PRIu32 ": TODO: unused",
		           idleTimeout);
	}

	if (fieldsPresent & HTTP_TUNNEL_AUTH_RESPONSE_FIELD_SOH_RESPONSE)
	{
		UINT16 cbLen = 0;
		if (!Stream_CheckAndLogRequiredCapacityWLog(rdg->log, s, 2))
			return FALSE;
		Stream_Read_UINT16(s, cbLen);

		WLog_Print(rdg->log, WLOG_DEBUG, "[SOH_RESPONSE] cbLen=%" PRIu16 ": TODO: unused", cbLen);
		if (!Stream_CheckAndLogRequiredLengthWLog(rdg->log, s, cbLen))
			return FALSE;
		Stream_Seek(s, cbLen);
	}

	return rdg_send_channel_create(rdg);
}

static BOOL rdg_process_extauth_sspi(rdpRdg* rdg, wStream* s)
{
	INT32 errorCode = 0;
	UINT16 authBlobLen = 0;
	SecBuffer authToken = { 0 };
	BYTE* authTokenData = NULL;

	WINPR_ASSERT(rdg);

	Stream_Read_INT32(s, errorCode);
	Stream_Read_UINT16(s, authBlobLen);

	if (errorCode != ERROR_SUCCESS)
	{
		WLog_Print(rdg->log, WLOG_ERROR, "EXTAUTH_SSPI_NTLM failed with error %s [0x%08X]",
		           GetSecurityStatusString(errorCode), errorCode);
		return FALSE;
	}

	if (authBlobLen == 0)
	{
		if (credssp_auth_is_complete(rdg->auth))
		{
			credssp_auth_free(rdg->auth);
			rdg->auth = NULL;
			return rdg_send_tunnel_request(rdg);
		}
		return FALSE;
	}

	authTokenData = malloc(authBlobLen);
	if (authTokenData == NULL)
		return FALSE;
	Stream_Read(s, authTokenData, authBlobLen);

	authToken.pvBuffer = authTokenData;
	authToken.cbBuffer = authBlobLen;

	credssp_auth_take_input_buffer(rdg->auth, &authToken);

	if (credssp_auth_authenticate(rdg->auth) < 0)
		return FALSE;

	if (credssp_auth_have_output_token(rdg->auth))
		return rdg_send_extauth_sspi(rdg);

	return FALSE;
}

static BOOL rdg_process_channel_response(rdpRdg* rdg, wStream* s)
{
	UINT16 fieldsPresent = 0;
	UINT32 errorCode = 0;
	const char* error = NULL;
	WLog_Print(rdg->log, WLOG_DEBUG, "Channel response received");

	if (rdg->state != RDG_CLIENT_STATE_CHANNEL_CREATE)
	{
		return FALSE;
	}

	if (!Stream_CheckAndLogRequiredLengthWLog(rdg->log, s, 8))
		return FALSE;

	Stream_Read_UINT32(s, errorCode);
	Stream_Read_UINT16(s, fieldsPresent);
	Stream_Seek_UINT16(s); /* reserved */
	error = rpc_error_to_string(errorCode);
	WLog_Print(rdg->log, WLOG_DEBUG, "channel response errorCode=%s, fieldsPresent=%s", error,
	           channel_response_fields_present_to_string(fieldsPresent));

	if (FAILED((HRESULT)errorCode))
	{
		WLog_Print(rdg->log, WLOG_ERROR, "channel response errorCode=%s, fieldsPresent=%s", error,
		           channel_response_fields_present_to_string(fieldsPresent));
		freerdp_set_last_error_log(rdg->context, errorCode);
		return FALSE;
	}

	rdg->state = RDG_CLIENT_STATE_OPENED;
	return TRUE;
}

static BOOL rdg_process_packet(rdpRdg* rdg, wStream* s)
{
	BOOL status = TRUE;
	UINT16 type = 0;
	UINT32 packetLength = 0;
	Stream_SetPosition(s, 0);

	if (!Stream_CheckAndLogRequiredLengthWLog(rdg->log, s, 8))
		return FALSE;

	Stream_Read_UINT16(s, type);
	Stream_Seek_UINT16(s); /* reserved */
	Stream_Read_UINT32(s, packetLength);

	if (Stream_Length(s) < packetLength)
	{
		WLog_Print(rdg->log, WLOG_ERROR, "Short packet %" PRIuz ", expected %" PRIuz,
		           Stream_Length(s), packetLength);
		return FALSE;
	}

	switch (type)
	{
		case PKT_TYPE_HANDSHAKE_RESPONSE:
			status = rdg_process_handshake_response(rdg, s);
			break;

		case PKT_TYPE_TUNNEL_RESPONSE:
			status = rdg_process_tunnel_response(rdg, s);
			break;

		case PKT_TYPE_TUNNEL_AUTH_RESPONSE:
			status = rdg_process_tunnel_authorization_response(rdg, s);
			break;

		case PKT_TYPE_CHANNEL_RESPONSE:
			status = rdg_process_channel_response(rdg, s);
			break;

		case PKT_TYPE_DATA:
			WLog_Print(rdg->log, WLOG_ERROR, "Unexpected packet type DATA");
			return FALSE;

		case PKT_TYPE_EXTENDED_AUTH_MSG:
			status = rdg_process_extauth_sspi(rdg, s);
			break;

		default:
			WLog_Print(rdg->log, WLOG_ERROR, "PKG TYPE 0x%x not implemented", type);
			return FALSE;
	}

	return status;
}

DWORD rdg_get_event_handles(rdpRdg* rdg, HANDLE* events, DWORD count)
{
	DWORD nCount = 0;
	WINPR_ASSERT(rdg != NULL);

	if (rdg->tlsOut && rdg->tlsOut->bio)
	{
		if (events && (nCount < count))
		{
			BIO_get_event(rdg->tlsOut->bio, &events[nCount]);
			nCount++;
		}
		else
			return 0;
	}

	/* We just need the read event handle even in non-websocket mode. */

	return nCount;
}

static BOOL rdg_get_gateway_credentials(rdpContext* context, rdp_auth_reason reason)
{
	freerdp* instance = context->instance;

	auth_status rc = utils_authenticate_gateway(instance, reason);
	switch (rc)
	{
		case AUTH_SUCCESS:
		case AUTH_SKIP:
			return TRUE;
		case AUTH_CANCELLED:
			freerdp_set_last_error_log(instance->context, FREERDP_ERROR_CONNECT_CANCELLED);
			return FALSE;
		case AUTH_NO_CREDENTIALS:
			WLog_INFO(TAG, "No credentials provided - using NULL identity");
			return TRUE;
		case AUTH_FAILED:
		default:
			return FALSE;
	}
}

static BOOL rdg_auth_init(rdpRdg* rdg, rdpTls* tls, TCHAR* authPkg)
{
	rdpContext* context = rdg->context;
	rdpSettings* settings = context->settings;
	SEC_WINNT_AUTH_IDENTITY identity = { 0 };
	int rc = 0;

	rdg->auth = credssp_auth_new(context);
	if (!rdg->auth)
		return FALSE;

	if (!credssp_auth_init(rdg->auth, authPkg, tls->Bindings))
		return FALSE;

	BOOL doSCLogon = freerdp_settings_get_bool(settings, FreeRDP_SmartcardLogon);
	if (doSCLogon)
	{
		if (!smartcard_getCert(context, &rdg->smartcard, TRUE))
			return FALSE;

		if (!rdg_get_gateway_credentials(context, AUTH_SMARTCARD_PIN))
			return FALSE;
	}
	else
	{
		if (!rdg_get_gateway_credentials(context, GW_AUTH_RDG))
			return FALSE;

		/* Auth callback might changed logon to smartcard so check again */
		doSCLogon = freerdp_settings_get_bool(settings, FreeRDP_SmartcardLogon);
		if (doSCLogon && !smartcard_getCert(context, &rdg->smartcard, TRUE))
			return FALSE;
	}

	SEC_WINNT_AUTH_IDENTITY* identityArg = &identity;
	if (doSCLogon)
	{
		if (!identity_set_from_smartcard_hash(&identity, settings, FreeRDP_GatewayUsername,
		                                      FreeRDP_GatewayDomain, FreeRDP_GatewayPassword,
		                                      rdg->smartcard->sha1Hash,
		                                      sizeof(rdg->smartcard->sha1Hash)))
			return FALSE;
	}
	else
	{
		if (!identity_set_from_settings(&identity, settings, FreeRDP_GatewayUsername,
		                                FreeRDP_GatewayDomain, FreeRDP_GatewayPassword))
			return FALSE;

		if (!settings->GatewayUsername)
			identityArg = NULL;
	}

	if (!credssp_auth_setup_client(rdg->auth, "HTTP", settings->GatewayHostname, identityArg,
	                               rdg->smartcard ? rdg->smartcard->pkinitArgs : NULL))
	{
		sspi_FreeAuthIdentity(&identity);
		return FALSE;
	}
	sspi_FreeAuthIdentity(&identity);

	credssp_auth_set_flags(rdg->auth, ISC_REQ_CONFIDENTIALITY | ISC_REQ_MUTUAL_AUTH);

	rc = credssp_auth_authenticate(rdg->auth);
	if (rc < 0)
		return FALSE;

	return TRUE;
}

static BOOL rdg_send_http_request(rdpRdg* rdg, rdpTls* tls, const char* method,
                                  TRANSFER_ENCODING transferEncoding)
{
	int status = -1;
	wStream* s = rdg_build_http_request(rdg, method, transferEncoding);

	if (!s)
		return FALSE;

	const size_t sz = Stream_Length(s);
	status = freerdp_tls_write_all(tls, Stream_Buffer(s), sz);

	Stream_Free(s, TRUE);
	return (status >= 0);
}

static BOOL rdg_tls_connect(rdpRdg* rdg, rdpTls* tls, const char* peerAddress, UINT32 timeout)
{
	long status = 0;
	BIO* layerBio = NULL;
	BIO* bufferedBio = NULL;
	rdpTransportLayer* layer = NULL;
	rdpSettings* settings = rdg->context->settings;
	rdpTransport* transport = freerdp_get_transport(rdg->context);
	const char* peerHostname = settings->GatewayHostname;
	UINT16 peerPort = (UINT16)settings->GatewayPort;
	const char* proxyUsername = NULL;
	const char* proxyPassword = NULL;
	BOOL isProxyConnection =
	    proxy_prepare(settings, &peerHostname, &peerPort, &proxyUsername, &proxyPassword);

	if (settings->GatewayPort > UINT16_MAX)
		return FALSE;

	layer = transport_connect_layer(transport, peerAddress ? peerAddress : peerHostname, peerPort,
	                                timeout);

	if (!layer)
	{
		return FALSE;
	}

	layerBio = BIO_new(BIO_s_transport_layer());
	if (!layerBio)
	{
		transport_layer_free(layer);
		return FALSE;
	}
	BIO_set_data(layerBio, layer);

	bufferedBio = BIO_new(BIO_s_buffered_socket());
	if (!bufferedBio)
	{
		BIO_free_all(layerBio);
		return FALSE;
	}

	bufferedBio = BIO_push(bufferedBio, layerBio);
	status = BIO_set_nonblock(bufferedBio, TRUE);

	if (isProxyConnection)
	{
		if (!proxy_connect(rdg->context, bufferedBio, proxyUsername, proxyPassword,
		                   settings->GatewayHostname, (UINT16)settings->GatewayPort))
		{
			BIO_free_all(bufferedBio);
			return FALSE;
		}
	}

	if (!status)
	{
		BIO_free_all(bufferedBio);
		return FALSE;
	}

	tls->hostname = settings->GatewayHostname;
	tls->port = WINPR_ASSERTING_INT_CAST(int32_t, MIN(UINT16_MAX, settings->GatewayPort));
	tls->isGatewayTransport = TRUE;
	status = freerdp_tls_connect(tls, bufferedBio);
	if (status < 1)
	{
		rdpContext* context = rdg->context;
		if (status < 0)
		{
			freerdp_set_last_error_if_not(context, FREERDP_ERROR_TLS_CONNECT_FAILED);
		}
		else
		{
			freerdp_set_last_error_if_not(context, FREERDP_ERROR_CONNECT_CANCELLED);
		}

		return FALSE;
	}
	return (status >= 1);
}

static BOOL rdg_establish_data_connection(rdpRdg* rdg, rdpTls* tls, const char* method,
                                          const char* peerAddress, UINT32 timeout,
                                          BOOL* rpcFallback)
{
	char buffer[64] = { 0 };
	HttpResponse* response = NULL;

	if (!rdg_tls_connect(rdg, tls, peerAddress, timeout))
		return FALSE;

	WINPR_ASSERT(rpcFallback);
	if (rdg->context->settings->GatewayHttpExtAuthBearer && rdg->extAuth == HTTP_EXTENDED_AUTH_NONE)
		rdg->extAuth = HTTP_EXTENDED_AUTH_BEARER;
	if (rdg->extAuth == HTTP_EXTENDED_AUTH_NONE)
	{
		if (!rdg_auth_init(rdg, tls, AUTH_PKG))
			return FALSE;

		if (!rdg_send_http_request(rdg, tls, method, TransferEncodingIdentity))
			return FALSE;

		response = http_response_recv(tls, TRUE);
		/* MS RD Gateway seems to just terminate the tls connection without
		 * sending an answer if it is not happy with the http request */
		if (!response)
		{
			WLog_Print(rdg->log, WLOG_INFO, "RD Gateway HTTP transport broken.");
			*rpcFallback = TRUE;
			return FALSE;
		}

		const long StatusCode = http_response_get_status_code(response);

		switch (StatusCode)
		{
			case HTTP_STATUS_GONE:
			case HTTP_STATUS_FORBIDDEN:
			case HTTP_STATUS_NOT_FOUND:
			{
				WLog_Print(rdg->log, WLOG_INFO, "RD Gateway does not support HTTP transport.");
				http_response_log_error_status(rdg->log, WLOG_DEBUG, response);
				*rpcFallback = TRUE;

				http_response_free(response);
				return FALSE;
			}
			case HTTP_STATUS_OK:
				break;

			case HTTP_STATUS_DENIED:
				http_response_log_error_status(rdg->log, WLOG_DEBUG, response);
				break;

			default:
				http_response_log_error_status(rdg->log, WLOG_WARN, response);
				break;
		}

		while (!credssp_auth_is_complete(rdg->auth))
		{
			if (!rdg_recv_auth_token(rdg->log, rdg->auth, response))
			{
				http_response_free(response);
				return FALSE;
			}

			if (credssp_auth_have_output_token(rdg->auth))
			{
				http_response_free(response);

				if (!rdg_send_http_request(rdg, tls, method, TransferEncodingIdentity))
					return FALSE;

				response = http_response_recv(tls, TRUE);
				if (!response)
				{
					WLog_Print(rdg->log, WLOG_INFO, "RD Gateway HTTP transport broken.");
					*rpcFallback = TRUE;
					return FALSE;
				}
			}
		}
		credssp_auth_free(rdg->auth);
		rdg->auth = NULL;
	}
	else
	{
		credssp_auth_free(rdg->auth);
		rdg->auth = NULL;

		if (!rdg_send_http_request(rdg, tls, method, TransferEncodingIdentity))
			return FALSE;

		response = http_response_recv(tls, TRUE);

		if (!response)
		{
			WLog_Print(rdg->log, WLOG_INFO, "RD Gateway HTTP transport broken.");
			*rpcFallback = TRUE;
			return FALSE;
		}
	}

	const long statusCode = http_response_get_status_code(response);
	const size_t bodyLength = http_response_get_body_length(response);
	const TRANSFER_ENCODING encoding = http_response_get_transfer_encoding(response);
	const BOOL isWebsocket = http_response_is_websocket(rdg->http, response);

	WLog_Print(rdg->log, WLOG_DEBUG, "%s authorization result: %s", method,
	           freerdp_http_status_string_format(statusCode, buffer, ARRAYSIZE(buffer)));

	switch (statusCode)
	{
		case HTTP_STATUS_OK:
			/* old rdg endpoint without websocket support, don't request websocket for RDG_IN_DATA
			 */
			http_context_enable_websocket_upgrade(rdg->http, FALSE);
			http_response_free(response);
			break;
		case HTTP_STATUS_DENIED:
			freerdp_set_last_error_log(rdg->context, FREERDP_ERROR_CONNECT_ACCESS_DENIED);
			http_response_free(response);
			return FALSE;
		case HTTP_STATUS_SWITCH_PROTOCOLS:
			http_response_free(response);
			if (!isWebsocket)
			{
				/*
				 * webserver is broken, a fallback may be possible here
				 * but only if already tested with oppurtonistic upgrade
				 */
				if (http_context_is_websocket_upgrade_enabled(rdg->http))
				{
					long fd = BIO_get_fd(tls->bio, NULL);
					if (fd >= 0)
						closesocket((SOCKET)fd);
					http_context_enable_websocket_upgrade(rdg->http, FALSE);
					return rdg_establish_data_connection(rdg, tls, method, peerAddress, timeout,
					                                     rpcFallback);
				}
				return FALSE;
			}

			rdg->transferEncoding.isWebsocketTransport = TRUE;
			if (!websocket_context_reset(rdg->transferEncoding.context.websocket))
				return FALSE;

			if (rdg->extAuth == HTTP_EXTENDED_AUTH_SSPI_NTLM)
			{
				/* create a new auth context for SSPI_NTLM. This must be done after the last
				 * rdg_send_http_request */
				if (!rdg_auth_init(rdg, tls, NTLM_SSP_NAME))
					return FALSE;
			}
			return TRUE;
		default:
			http_response_log_error_status(rdg->log, WLOG_WARN, response);
			http_response_free(response);
			return FALSE;
	}

	if (strcmp(method, "RDG_OUT_DATA") == 0)
	{
		if (encoding == TransferEncodingChunked)
		{
			rdg->transferEncoding.httpTransferEncoding = TransferEncodingChunked;
			rdg->transferEncoding.context.chunked.nextOffset = 0;
			rdg->transferEncoding.context.chunked.headerFooterPos = 0;
			rdg->transferEncoding.context.chunked.state = ChunkStateLenghHeader;
		}
		if (!rdg_skip_seed_payload(rdg->context, tls, bodyLength, &rdg->transferEncoding))
		{
			return FALSE;
		}
	}
	else
	{
		if (!rdg_send_http_request(rdg, tls, method, TransferEncodingChunked))
			return FALSE;

		if (rdg->extAuth == HTTP_EXTENDED_AUTH_SSPI_NTLM)
		{
			/* create a new auth context for SSPI_NTLM. This must be done after the last
			 * rdg_send_http_request (RDG_IN_DATA is always after RDG_OUT_DATA) */
			if (!rdg_auth_init(rdg, tls, NTLM_SSP_NAME))
				return FALSE;
		}
	}

	return TRUE;
}

static BOOL rdg_tunnel_connect(rdpRdg* rdg)
{
	BOOL status = 0;
	wStream* s = NULL;
	rdg_send_handshake(rdg);

	while (rdg->state < RDG_CLIENT_STATE_OPENED)
	{
		status = FALSE;
		s = rdg_receive_packet(rdg);

		if (s)
		{
			status = rdg_process_packet(rdg, s);
			Stream_Free(s, TRUE);
		}

		if (!status)
		{
			WINPR_ASSERT(rdg);
			WINPR_ASSERT(rdg->context);
			WINPR_ASSERT(rdg->context->rdp);
			transport_set_layer(rdg->context->rdp->transport, TRANSPORT_LAYER_CLOSED);
			return FALSE;
		}
	}

	return TRUE;
}

BOOL rdg_connect(rdpRdg* rdg, DWORD timeout, BOOL* rpcFallback)
{
	BOOL status = 0;
	SOCKET outConnSocket = 0;
	char* peerAddress = NULL;
	BOOL rpcFallbackLocal = FALSE;

	WINPR_ASSERT(rdg != NULL);
	status = rdg_establish_data_connection(rdg, rdg->tlsOut, "RDG_OUT_DATA", NULL, timeout,
	                                       &rpcFallbackLocal);

	if (status)
	{
		if (rdg->transferEncoding.isWebsocketTransport)
		{
			WLog_Print(rdg->log, WLOG_DEBUG, "Upgraded to websocket. RDG_IN_DATA not required");
		}
		else
		{
			/* Establish IN connection with the same peer/server as OUT connection,
			 * even when server hostname resolves to different IP addresses.
			 */
			BIO_get_socket(rdg->tlsOut->underlying, &outConnSocket);
			peerAddress = freerdp_tcp_get_peer_address(outConnSocket);
			status = rdg_establish_data_connection(rdg, rdg->tlsIn, "RDG_IN_DATA", peerAddress,
			                                       timeout, &rpcFallbackLocal);
			free(peerAddress);
		}
	}

	if (rpcFallback)
		*rpcFallback = rpcFallbackLocal;

	if (!status)
	{
		WINPR_ASSERT(rdg);
		WINPR_ASSERT(rdg->context);
		WINPR_ASSERT(rdg->context->rdp);
		if (rpcFallbackLocal)
		{
			http_context_enable_websocket_upgrade(rdg->http, FALSE);
			credssp_auth_free(rdg->auth);
			rdg->auth = NULL;
		}

		transport_set_layer(rdg->context->rdp->transport, TRANSPORT_LAYER_CLOSED);
		return FALSE;
	}

	status = rdg_tunnel_connect(rdg);

	if (!status)
		return FALSE;

	return TRUE;
}

static int rdg_write_websocket_data_packet(rdpRdg* rdg, const BYTE* buf, int isize)
{
	WINPR_ASSERT(rdg);
	if (isize < 0)
		return -1;

	const size_t payloadSize = (size_t)isize + 10;
	union
	{
		UINT32 u32;
		UINT8 u8[4];
	} maskingKey;

	wStream* sWS =
	    websocket_context_packet_new(payloadSize, WebsocketBinaryOpcode, &maskingKey.u32);
	if (!sWS)
		return FALSE;

	Stream_Write_UINT16(
	    sWS, WINPR_ASSERTING_INT_CAST(
	             uint16_t, PKT_TYPE_DATA ^ (maskingKey.u8[0] | maskingKey.u8[1] << 8))); /* Type */
	Stream_Write_UINT16(
	    sWS, WINPR_ASSERTING_INT_CAST(
	             uint16_t, 0 ^ (maskingKey.u8[2] | maskingKey.u8[3] << 8))); /* Reserved */
	Stream_Write_UINT32(
	    sWS, WINPR_ASSERTING_INT_CAST(uint32_t, payloadSize ^ maskingKey.u32)); /* Packet length */
	Stream_Write_UINT16(
	    sWS, WINPR_ASSERTING_INT_CAST(
	             uint16_t, isize ^ (maskingKey.u8[0] | maskingKey.u8[1] << 8))); /* Data size */

	/* masking key is now off by 2 bytes. fix that */
	maskingKey.u32 = (maskingKey.u32 & 0xffff) << 16 | (maskingKey.u32 >> 16);

	WINPR_ASSERT(rdg->tlsOut);
	wStream sPacket = { 0 };
	Stream_StaticConstInit(&sPacket, buf, (size_t)isize);
	if (!websocket_context_mask_and_send(rdg->tlsOut->bio, sWS, &sPacket, maskingKey.u32))
		return -1;

	return isize;
}

static int rdg_write_chunked_data_packet(rdpRdg* rdg, const BYTE* buf, int isize)
{
	int status = 0;
	size_t len = 0;
	wStream* sChunk = NULL;

	if (isize > UINT16_MAX)
		return -1;

	const size_t size = (size_t)isize;
	if (size < 1)
		return 0;

	const size_t packetSize = size + 10;
	char chunkSize[11] = { 0 };
	(void)sprintf_s(chunkSize, sizeof(chunkSize), "%" PRIxz "\r\n", packetSize);
	sChunk = Stream_New(NULL, strnlen(chunkSize, sizeof(chunkSize)) + packetSize + 2);

	if (!sChunk)
		return -1;

	Stream_Write(sChunk, chunkSize, strnlen(chunkSize, sizeof(chunkSize)));
	Stream_Write_UINT16(sChunk, PKT_TYPE_DATA);      /* Type */
	Stream_Write_UINT16(sChunk, 0);                  /* Reserved */
	Stream_Write_UINT32(sChunk, (UINT32)packetSize); /* Packet length */
	Stream_Write_UINT16(sChunk, (UINT16)size);       /* Data size */
	Stream_Write(sChunk, buf, size);                 /* Data */
	Stream_Write(sChunk, "\r\n", 2);
	Stream_SealLength(sChunk);
	len = Stream_Length(sChunk);

	status = freerdp_tls_write_all(rdg->tlsIn, Stream_Buffer(sChunk), len);
	Stream_Free(sChunk, TRUE);

	if (status < 0)
		return -1;

	return (int)size;
}

static int rdg_write_data_packet(rdpRdg* rdg, const BYTE* buf, int isize)
{
	WINPR_ASSERT(rdg);
	if (rdg->transferEncoding.isWebsocketTransport)
		return rdg_write_websocket_data_packet(rdg, buf, isize);
	else
		return rdg_write_chunked_data_packet(rdg, buf, isize);
}

static BOOL rdg_process_close_packet(rdpRdg* rdg, wStream* s)
{
	int status = -1;
	wStream* sClose = NULL;
	UINT32 errorCode = 0;
	UINT32 packetSize = 12;

	/* Read error code */
	if (!Stream_CheckAndLogRequiredLengthWLog(rdg->log, s, 4))
		return FALSE;
	Stream_Read_UINT32(s, errorCode);

	if (errorCode != 0)
		freerdp_set_last_error_log(rdg->context, errorCode);

	sClose = Stream_New(NULL, packetSize);
	if (!sClose)
		return FALSE;

	Stream_Write_UINT16(sClose, PKT_TYPE_CLOSE_CHANNEL_RESPONSE); /* Type */
	Stream_Write_UINT16(sClose, 0);                               /* Reserved */
	Stream_Write_UINT32(sClose, packetSize);                      /* Packet length */
	Stream_Write_UINT32(sClose, 0);                               /* Status code */
	Stream_SealLength(sClose);
	status = rdg_write_packet(rdg, sClose);
	Stream_Free(sClose, TRUE);

	return (status < 0 ? FALSE : TRUE);
}

static BOOL rdg_process_keep_alive_packet(rdpRdg* rdg)
{
	int status = -1;
	wStream* sKeepAlive = NULL;
	size_t packetSize = 8;

	sKeepAlive = Stream_New(NULL, packetSize);

	if (!sKeepAlive)
		return FALSE;

	Stream_Write_UINT16(sKeepAlive, PKT_TYPE_KEEPALIVE); /* Type */
	Stream_Write_UINT16(sKeepAlive, 0);                  /* Reserved */
	Stream_Write_UINT32(sKeepAlive, (UINT32)packetSize); /* Packet length */
	Stream_SealLength(sKeepAlive);
	status = rdg_write_packet(rdg, sKeepAlive);
	Stream_Free(sKeepAlive, TRUE);

	return (status < 0 ? FALSE : TRUE);
}

static BOOL rdg_process_service_message(rdpRdg* rdg, wStream* s)
{
	const WCHAR* msg = NULL;
	UINT16 msgLenBytes = 0;
	rdpContext* context = rdg->context;
	WINPR_ASSERT(context);
	WINPR_ASSERT(context->instance);

	/* Read message string */
	if (!rdg_read_http_unicode_string(rdg->log, s, &msg, &msgLenBytes))
	{
		WLog_Print(rdg->log, WLOG_ERROR, "Failed to read string");
		return FALSE;
	}

	return IFCALLRESULT(TRUE, context->instance->PresentGatewayMessage, context->instance,
	                    GATEWAY_MESSAGE_SERVICE, TRUE, FALSE, msgLenBytes, msg);
}

static BOOL rdg_process_unknown_packet(rdpRdg* rdg, int type)
{
	WINPR_UNUSED(rdg);
	WINPR_UNUSED(type);
	WLog_Print(rdg->log, WLOG_WARN, "Unknown Control Packet received: %X", type);
	return TRUE;
}

static BOOL rdg_process_control_packet(rdpRdg* rdg, int type, size_t packetLength)
{
	wStream* s = NULL;
	size_t readCount = 0;
	int status = 0;
	size_t payloadSize = packetLength - sizeof(RdgPacketHeader);

	if (packetLength < sizeof(RdgPacketHeader))
		return FALSE;

	// NOLINTNEXTLINE(bugprone-sizeof-expression)
	WINPR_ASSERT(sizeof(RdgPacketHeader) < INT_MAX);

	if (payloadSize)
	{
		s = Stream_New(NULL, payloadSize);

		if (!s)
			return FALSE;

		while (readCount < payloadSize)
		{
			if (rdg_shall_abort(rdg))
			{
				Stream_Free(s, TRUE);
				return FALSE;
			}
			status = rdg_socket_read(rdg->tlsOut->bio, Stream_Pointer(s), payloadSize - readCount,
			                         &rdg->transferEncoding);

			if (status <= 0)
			{
				if (!BIO_should_retry(rdg->tlsOut->bio))
				{
					Stream_Free(s, TRUE);
					return FALSE;
				}

				continue;
			}

			Stream_Seek(s, (size_t)status);
			readCount += (size_t)status;

			if (readCount > INT_MAX)
			{
				Stream_Free(s, TRUE);
				return FALSE;
			}
		}

		Stream_SetPosition(s, 0);
	}

	switch (type)
	{
		case PKT_TYPE_CLOSE_CHANNEL:
			EnterCriticalSection(&rdg->writeSection);
			status = rdg_process_close_packet(rdg, s);
			LeaveCriticalSection(&rdg->writeSection);
			break;

		case PKT_TYPE_KEEPALIVE:
			EnterCriticalSection(&rdg->writeSection);
			status = rdg_process_keep_alive_packet(rdg);
			LeaveCriticalSection(&rdg->writeSection);
			break;

		case PKT_TYPE_SERVICE_MESSAGE:
			if (!s)
			{
				WLog_Print(rdg->log, WLOG_ERROR,
				           "PKT_TYPE_SERVICE_MESSAGE requires payload but none was sent");
				return FALSE;
			}
			status = rdg_process_service_message(rdg, s);
			break;

		case PKT_TYPE_REAUTH_MESSAGE:
		default:
			status = rdg_process_unknown_packet(rdg, type);
			break;
	}

	Stream_Free(s, TRUE);
	return status;
}

static int rdg_read_data_packet(rdpRdg* rdg, BYTE* buffer, size_t size)
{
	RdgPacketHeader header = { 0 };
	size_t readCount = 0;
	size_t readSize = 0;
	int status = 0;

	if (!rdg->packetRemainingCount)
	{
		// NOLINTNEXTLINE(bugprone-sizeof-expression)
		WINPR_ASSERT(sizeof(RdgPacketHeader) < INT_MAX);

		while (readCount < sizeof(RdgPacketHeader))
		{
			if (rdg_shall_abort(rdg))
				return -1;

			status = rdg_socket_read(rdg->tlsOut->bio, (BYTE*)(&header) + readCount,
			                         sizeof(RdgPacketHeader) - readCount, &rdg->transferEncoding);

			if (status <= 0)
			{
				if (!BIO_should_retry(rdg->tlsOut->bio))
					return -1;

				if (!readCount)
					return 0;

				BIO_wait_read(rdg->tlsOut->bio, 50);
				continue;
			}

			readCount += (size_t)status;

			if (readCount > INT_MAX)
				return -1;
		}

		if (header.type != PKT_TYPE_DATA)
		{
			status = rdg_process_control_packet(rdg, header.type, header.packetLength);

			if (!status)
				return -1;

			return 0;
		}

		readCount = 0;

		while (readCount < 2)
		{
			if (rdg_shall_abort(rdg))
				return -1;
			status =
			    rdg_socket_read(rdg->tlsOut->bio, (BYTE*)(&rdg->packetRemainingCount) + readCount,
			                    2 - readCount, &rdg->transferEncoding);

			if (status < 0)
			{
				if (!BIO_should_retry(rdg->tlsOut->bio))
					return -1;

				BIO_wait_read(rdg->tlsOut->bio, 50);
				continue;
			}

			readCount += (size_t)status;
		}
	}

	readSize = (rdg->packetRemainingCount < size) ? rdg->packetRemainingCount : size;
	status = rdg_socket_read(rdg->tlsOut->bio, buffer, readSize, &rdg->transferEncoding);

	if (status <= 0)
	{
		if (!BIO_should_retry(rdg->tlsOut->bio))
		{
			return -1;
		}

		return 0;
	}

	rdg->packetRemainingCount -= status;
	return status;
}

static int rdg_bio_write(BIO* bio, const char* buf, int num)
{
	int status = 0;
	rdpRdg* rdg = (rdpRdg*)BIO_get_data(bio);
	if (num < 0)
		return num;

	BIO_clear_flags(bio, BIO_FLAGS_WRITE);
	EnterCriticalSection(&rdg->writeSection);
	status = rdg_write_data_packet(rdg, (const BYTE*)buf, num);
	LeaveCriticalSection(&rdg->writeSection);

	if (status < 0)
	{
		BIO_clear_flags(bio, BIO_FLAGS_SHOULD_RETRY);
		return -1;
	}
	else if (status < num)
	{
		BIO_set_flags(bio, BIO_FLAGS_WRITE);
		WSASetLastError(WSAEWOULDBLOCK);
	}
	else
	{
		BIO_set_flags(bio, BIO_FLAGS_WRITE);
	}

	return status;
}

static int rdg_bio_read(BIO* bio, char* buf, int size)
{
	int status = 0;
	rdpRdg* rdg = (rdpRdg*)BIO_get_data(bio);
	if (size < 0)
		return size;
	status = rdg_read_data_packet(rdg, (BYTE*)buf, (size_t)size);

	if (status < 0)
	{
		BIO_clear_retry_flags(bio);
		return -1;
	}
	else if (status == 0)
	{
		BIO_set_retry_read(bio);
		WSASetLastError(WSAEWOULDBLOCK);
		return -1;
	}
	else
	{
		BIO_set_flags(bio, BIO_FLAGS_READ);
	}

	return status;
}

static int rdg_bio_puts(BIO* bio, const char* str)
{
	WINPR_UNUSED(bio);
	WINPR_UNUSED(str);
	return -2;
}

// NOLINTNEXTLINE(readability-non-const-parameter)
static int rdg_bio_gets(BIO* bio, char* str, int size)
{
	WINPR_UNUSED(bio);
	WINPR_UNUSED(str);
	WINPR_UNUSED(size);
	return -2;
}

static long rdg_bio_ctrl(BIO* in_bio, int cmd, long arg1, void* arg2)
{
	long status = -1;
	rdpRdg* rdg = (rdpRdg*)BIO_get_data(in_bio);
	rdpTls* tlsOut = rdg->tlsOut;
	rdpTls* tlsIn = rdg->tlsIn;

	if (cmd == BIO_CTRL_FLUSH)
	{
		(void)BIO_flush(tlsOut->bio);
		if (!rdg->transferEncoding.isWebsocketTransport)
			(void)BIO_flush(tlsIn->bio);
		status = 1;
	}
	else if (cmd == BIO_C_SET_NONBLOCK)
	{
		status = 1;
	}
	else if (cmd == BIO_C_READ_BLOCKED)
	{
		BIO* cbio = tlsOut->bio;
		status = BIO_read_blocked(cbio);
	}
	else if (cmd == BIO_C_WRITE_BLOCKED)
	{
		BIO* cbio = tlsIn->bio;

		if (rdg->transferEncoding.isWebsocketTransport)
			cbio = tlsOut->bio;

		status = BIO_write_blocked(cbio);
	}
	else if (cmd == BIO_C_WAIT_READ)
	{
		int timeout = (int)arg1;
		BIO* cbio = tlsOut->bio;

		if (BIO_read_blocked(cbio))
			return BIO_wait_read(cbio, timeout);
		else if (BIO_write_blocked(cbio))
			return BIO_wait_write(cbio, timeout);
		else
			status = 1;
	}
	else if (cmd == BIO_C_WAIT_WRITE)
	{
		int timeout = (int)arg1;
		BIO* cbio = tlsIn->bio;

		if (rdg->transferEncoding.isWebsocketTransport)
			cbio = tlsOut->bio;

		if (BIO_write_blocked(cbio))
			status = BIO_wait_write(cbio, timeout);
		else if (BIO_read_blocked(cbio))
			status = BIO_wait_read(cbio, timeout);
		else
			status = 1;
	}
	else if (cmd == BIO_C_GET_EVENT || cmd == BIO_C_GET_FD)
	{
		/*
		 * A note about BIO_C_GET_FD:
		 * Even if two FDs are part of RDG, only one FD can be returned here.
		 *
		 * In FreeRDP, BIO FDs are only used for polling, so it is safe to use the outgoing FD only
		 *
		 * See issue #3602
		 */
		status = BIO_ctrl(tlsOut->bio, cmd, arg1, arg2);
	}
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
	else if (cmd == BIO_CTRL_GET_KTLS_SEND)
	{
		/* Even though BIO_get_ktls_send says that returning negative values is valid
		 * openssl internal sources are full of if(!BIO_get_ktls_send && ) stuff. This has some
		 * nasty sideeffects. return 0 as proper no KTLS offloading flag
		 */
		status = 0;
	}
	else if (cmd == BIO_CTRL_GET_KTLS_RECV)
	{
		/* Even though BIO_get_ktls_recv says that returning negative values is valid
		 * there is no reason to trust  trust negative values are implemented right everywhere
		 */
		status = 0;
	}
#endif
	return status;
}

static int rdg_bio_new(BIO* bio)
{
	BIO_set_init(bio, 1);
	BIO_set_flags(bio, BIO_FLAGS_SHOULD_RETRY);
	return 1;
}

static int rdg_bio_free(BIO* bio)
{
	WINPR_UNUSED(bio);
	return 1;
}

static BIO_METHOD* BIO_s_rdg(void)
{
	static BIO_METHOD* bio_methods = NULL;

	if (bio_methods == NULL)
	{
		if (!(bio_methods = BIO_meth_new(BIO_TYPE_TSG, "RDGateway")))
			return NULL;

		BIO_meth_set_write(bio_methods, rdg_bio_write);
		BIO_meth_set_read(bio_methods, rdg_bio_read);
		BIO_meth_set_puts(bio_methods, rdg_bio_puts);
		BIO_meth_set_gets(bio_methods, rdg_bio_gets);
		BIO_meth_set_ctrl(bio_methods, rdg_bio_ctrl);
		BIO_meth_set_create(bio_methods, rdg_bio_new);
		BIO_meth_set_destroy(bio_methods, rdg_bio_free);
	}

	return bio_methods;
}

rdpRdg* rdg_new(rdpContext* context)
{
	if (!context)
		return NULL;

	rdpRdg* rdg = (rdpRdg*)calloc(1, sizeof(rdpRdg));
	if (!rdg)
		return NULL;

	rdg->log = WLog_Get(TAG);
	rdg->state = RDG_CLIENT_STATE_INITIAL;
	rdg->context = context;
	rdg->extAuth =
	    (rdg->context->settings->GatewayHttpExtAuthSspiNtlm ? HTTP_EXTENDED_AUTH_SSPI_NTLM
	                                                        : HTTP_EXTENDED_AUTH_NONE);

	if (rdg->context->settings->GatewayAccessToken)
		rdg->extAuth = HTTP_EXTENDED_AUTH_PAA;

	UuidCreate(&rdg->guid);

	rdg->tlsOut = freerdp_tls_new(rdg->context);

	if (!rdg->tlsOut)
		goto rdg_alloc_error;

	rdg->tlsIn = freerdp_tls_new(rdg->context);

	if (!rdg->tlsIn)
		goto rdg_alloc_error;

	rdg->http = http_context_new();

	if (!rdg->http)
		goto rdg_alloc_error;

	if (!http_context_set_uri(rdg->http, "/remoteDesktopGateway/") ||
	    !http_context_set_accept(rdg->http, "*/*") ||
	    !http_context_set_cache_control(rdg->http, "no-cache") ||
	    !http_context_set_pragma(rdg->http, "no-cache") ||
	    !http_context_set_connection(rdg->http, "Keep-Alive") ||
	    !http_context_set_user_agent(rdg->http, "MS-RDGateway/1.0") ||
	    !http_context_set_host(rdg->http, rdg->context->settings->GatewayHostname) ||
	    !http_context_set_rdg_connection_id(rdg->http, &rdg->guid) ||
	    !http_context_set_rdg_correlation_id(rdg->http, &rdg->guid) ||
	    !http_context_enable_websocket_upgrade(
	        rdg->http,
	        freerdp_settings_get_bool(rdg->context->settings, FreeRDP_GatewayHttpUseWebsockets)))
	{
		goto rdg_alloc_error;
	}

	if (rdg->extAuth != HTTP_EXTENDED_AUTH_NONE)
	{
		switch (rdg->extAuth)
		{
			case HTTP_EXTENDED_AUTH_PAA:
				if (!http_context_set_rdg_auth_scheme(rdg->http, "PAA"))
					goto rdg_alloc_error;

				break;

			case HTTP_EXTENDED_AUTH_SSPI_NTLM:
				if (!http_context_set_rdg_auth_scheme(rdg->http, "SSPI_NTLM"))
					goto rdg_alloc_error;

				break;

			default:
				WLog_Print(rdg->log, WLOG_DEBUG,
				           "RDG extended authentication method %d not supported", rdg->extAuth);
		}
	}

	rdg->frontBio = BIO_new(BIO_s_rdg());

	if (!rdg->frontBio)
		goto rdg_alloc_error;

	BIO_set_data(rdg->frontBio, rdg);
	InitializeCriticalSection(&rdg->writeSection);

	rdg->transferEncoding.httpTransferEncoding = TransferEncodingIdentity;
	rdg->transferEncoding.isWebsocketTransport = FALSE;

	rdg->transferEncoding.context.websocket = websocket_context_new();
	if (!rdg->transferEncoding.context.websocket)
		goto rdg_alloc_error;

	return rdg;
rdg_alloc_error:
	WINPR_PRAGMA_DIAG_PUSH
	WINPR_PRAGMA_DIAG_IGNORED_MISMATCHED_DEALLOC
	rdg_free(rdg);
	WINPR_PRAGMA_DIAG_POP
	return NULL;
}

void rdg_free(rdpRdg* rdg)
{
	if (!rdg)
		return;

	freerdp_tls_free(rdg->tlsOut);
	freerdp_tls_free(rdg->tlsIn);
	http_context_free(rdg->http);
	credssp_auth_free(rdg->auth);

	if (!rdg->attached)
		BIO_free_all(rdg->frontBio);

	DeleteCriticalSection(&rdg->writeSection);

	smartcardCertInfo_Free(rdg->smartcard);

	websocket_context_free(rdg->transferEncoding.context.websocket);

	free(rdg);
}

BIO* rdg_get_front_bio_and_take_ownership(rdpRdg* rdg)
{
	if (!rdg)
		return NULL;

	rdg->attached = TRUE;
	return rdg->frontBio;
}
