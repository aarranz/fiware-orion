/*
*
* Copyright 2013 Telefonica Investigacion y Desarrollo, S.A.U
*
* This file is part of Orion Context Broker.
*
* Orion Context Broker is free software: you can redistribute it and/or
* modify it under the terms of the GNU Affero General Public License as
* published by the Free Software Foundation, either version 3 of the
* License, or (at your option) any later version.
*
* Orion Context Broker is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero
* General Public License for more details.
*
* You should have received a copy of the GNU Affero General Public License
* along with Orion Context Broker. If not, see http://www.gnu.org/licenses/.
*
* For those usages not covered by this license please contact with
* iot_support at tid dot es
*
* Author: developer
*/
#include <unistd.h>                             // close()
#include <sys/types.h>                          // system types ...
#include <sys/socket.h>                         // socket, bind, listen
#include <sys/un.h>                             // sockaddr_un
#include <netinet/in.h>                         // struct sockaddr_in
#include <netdb.h>                              // gethostbyname
#include <arpa/inet.h>                          // inet_ntoa
#include <netinet/tcp.h>                        // TCP_NODELAY
#include <curl/curl.h>

#include <string>
#include <vector>
#include <iostream>
#include <sstream>

#include "logMsg/logMsg.h"
#include "logMsg/traceLevels.h"

#include "common/string.h"
#include "common/sem.h"
#include "common/limits.h"
#include "alarmMgr/alarmMgr.h"
#include "rest/ConnectionInfo.h"
#include "rest/httpRequestSend.h"
#include "rest/rest.h"
#include "serviceRoutines/versionTreat.h"



/* ****************************************************************************
*
* defaultTimeout - 
*/
static long defaultTimeout = DEFAULT_TIMEOUT;



/* ****************************************************************************
*
* httpRequestInit - 
*/
void httpRequestInit(long defaultTimeoutInMilliseconds)
{
  if (defaultTimeoutInMilliseconds != -1)
  {
    defaultTimeout = defaultTimeoutInMilliseconds;
  }
}



/* **************************************************************************** 
*
* See [1] for a discussion on how curl_multi is to be used. Libcurl does not seem
* to provide a way to do asynchronous HTTP transactions in the way we intended
* with the previous version of httpRequestSend. To enable the old behavior of asynchronous
* HTTP requests uncomment the following #define line.
*
* [1] http://stackoverflow.com/questions/24288513/how-to-do-curl-multi-perform-asynchronously-in-c
*/

struct MemoryStruct
{
  char*   memory;
  size_t  size;
};



/* ****************************************************************************
*
* writeMemoryCallback -
*/
size_t writeMemoryCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
  size_t realsize = size * nmemb;
  MemoryStruct* mem = (MemoryStruct *) userp;

  mem->memory = (char*) realloc(mem->memory, mem->size + realsize + 1);
  if (mem->memory == NULL)
  {
    LM_E(("Runtime Error (out of memory)"));
    return 0;
  }

  memcpy(&(mem->memory[mem->size]), contents, realsize);
  mem->size += realsize;
  mem->memory[mem->size] = 0;

  return realsize;
}



/* ****************************************************************************
*
* curlVersionGet - 
*/
static char* curlVersionGet(char* buf, int bufLen)
{
  curl_version_info_data* idP;

  idP = curl_version_info(CURLVERSION_NOW);

  snprintf(buf, bufLen, "%s", idP->version);

  return buf;
}



/* ****************************************************************************
*
* httpHeaderAdd - add HTTP header to the list of HTTP headers for curl
*
* PARAMETERS
*   o headersP            pointer to the list of HTTP headers, to be used by curl
*   o headerName          The name of the header, e.g. "Content-Type"
*   o headerString        The complete header, e.g. "Content-Type: application/json"
*   o headerTotalSizeP    Pointer to a variable holding the total size of the list of headers,
*                         the string length of the list. To this variable, the string length of the added header must be added.
*   o extraHeaders        list of extra headers that were asked for when creating the subscription.
*                         We need this variable here in case the user overloaded any standard header.
*   o usedExtraHeaders    list of headers already used, so that any overloaded standard header are not present twice in the notification.
*
*/
static void httpHeaderAdd
(
  struct curl_slist**                        headersP,
  const std::string&                         headerName,
  const std::string&                         headerString,
  int*                                       headerTotalSizeP,
  const std::map<std::string, std::string>&  extraHeaders,
  std::map<std::string, bool>&               usedExtraHeaders
)
{
  std::string  h = headerString;

  std::map<std::string, std::string>::const_iterator it;
  it = extraHeaders.find(headerName);
  if (it == extraHeaders.end())  // headerName NOT found in extraHeaders, use headerString
  {
    h = headerString;
  }
  else
  {
    h = headerName + ": " + it->second;

    std::string headerNameLowerCase = headerName;
    std::transform(headerNameLowerCase.begin(), headerNameLowerCase.end(), headerNameLowerCase.begin(), ::tolower);
    usedExtraHeaders[headerNameLowerCase.c_str()] = true;
  }

  *headersP           = curl_slist_append(*headersP, h.c_str());
  *headerTotalSizeP  += h.size();

  LM_W(("KZ: Added curl header '%s'", h.c_str()));
}



/* ****************************************************************************
*
* httpRequestSendWithCurl -
*
* The waitForResponse arguments specifies if the method has to wait for response
* before return. If this argument is false, the return string is ""
*
* NOTE
* We are using a hybrid approach, consisting in a static thread-local buffer of a
* small size that copes with most notifications to avoid expensive
* calloc/free syscalls if the notification payload is not very large.
*
* RETURN VALUES
*   httpRequestSendWithCurl returns 0 on success and a negative number on failure:
*     -1: Invalid port
*     -2: Invalid IP
*     -3: Invalid verb
*     -4: Invalid resource
*     -5: No Content-Type BUT content present
*     -6: Content-Type present but there is no content
*     -7: Total outgoing message size is too big
*     -9: Error making HTTP request
*/
int httpRequestSendWithCurl
(
   CURL*                                      curl,
   const std::string&                         _ip,
   unsigned short                             port,
   const std::string&                         protocol,
   const std::string&                         verb,
   const std::string&                         tenant,
   const std::string&                         servicePath,
   const std::string&                         xauthToken,
   const std::string&                         resource,
   const std::string&                         orig_content_type,
   const std::string&                         content,
   const std::string&                         fiwareCorrelation,
   const std::string&                         ngisv2AttrFormat,
   bool                                       useRush,
   bool                                       waitForResponse,
   std::string*                               outP,
   const std::map<std::string, std::string>&  extraHeaders,
   const std::string&                         acceptFormat,
   long                                       timeoutInMilliseconds
)
{
  //
  // TEMP FIX:
  //   Early stop if port == 443 or protocol == "https" - see issue 2844
  //
  if (protocol == "https:")
  {
    LM_W(("KZ: protocol is https - no notification is sent"));
    return 0;
  }
  if (port == 443)
  {
    LM_W(("KZ: port is 443 - no notification is sent"));
    return 0;
  }

  LM_W(("KZ: protocol is '%s'", protocol.c_str()));
  LM_W(("KZ: port is %d", port));

  char                            portAsString[STRING_SIZE_FOR_INT];
  static unsigned long long       callNo             = 0;
  std::string                     result;
  std::string                     ip                 = _ip;
  struct curl_slist*              headers            = NULL;
  MemoryStruct*                   httpResponse       = NULL;
  CURLcode                        res;
  int                             outgoingMsgSize       = 0;
  std::string                     content_type(orig_content_type);
  std::map<std::string, bool>     usedExtraHeaders;

  ++callNo;

  // For content-type application/json we add charset=utf-8
  if ((orig_content_type == "application/json") || (orig_content_type == "text/plain"))
  {
    content_type += "; charset=utf-8";
  }

  if (timeoutInMilliseconds == -1)
  {
    timeoutInMilliseconds = defaultTimeout;
  }

  lmTransactionStart("to", ip.c_str(), port, resource.c_str());

  // Preconditions check
  if (port == 0)
  {
    LM_E(("Runtime Error (port is ZERO)"));
    lmTransactionEnd();

    *outP = "error";
    return -1;
  }

  if (ip.empty())
  {
    LM_E(("Runtime Error (ip is empty)"));
    lmTransactionEnd();

    *outP = "error";
    return -2;
  }

  if (verb.empty())
  {
    LM_E(("Runtime Error (verb is empty)"));
    lmTransactionEnd();

    *outP = "error";
    return -3;
  }

  if (resource.empty())
  {
    LM_E(("Runtime Error (resource is empty)"));
    lmTransactionEnd();

    *outP = "error";
    return -4;
  }

  if ((content_type.empty()) && (!content.empty()))
  {
    LM_E(("Runtime Error (Content-Type is empty but there is actual content)"));
    lmTransactionEnd();

    *outP = "error";
    return -5;
  }

  if ((!content_type.empty()) && (content.empty()))
  {
    LM_E(("Runtime Error (Content-Type non-empty but there is no content)"));
    lmTransactionEnd();

    *outP = "error";
    return -6;
  }



  // Allocate to hold HTTP response
  httpResponse = new MemoryStruct;
  httpResponse->memory = (char*) malloc(1); // will grow as needed
  httpResponse->size = 0; // no data at this point

  //
  // Rush
  // Every call to httpRequestSend specifies whether RUSH should be used or not.
  // But, this depends also on how the broker was started, so here the 'useRush'
  // parameter is cancelled in case the broker was started without rush.
  //
  // If rush is to be used, the IP/port is stored in rushHeaderIP/rushHeaderPort and
  // after that, the host and port of rush is set as ip/port for the message, that is
  // now sent to rush instead of to its final destination.
  // Also, a few HTTP headers for rush must be setup.
  //
  if ((rushPort == 0) || (rushHost == ""))
  {
    useRush = false;
  }

  if (useRush)
  {
    char         rushHeaderPortAsString[STRING_SIZE_FOR_INT];
    uint16_t     rushHeaderPort     = port;
    std::string  rushHeaderIP       = ip;
    std::string  headerRushHttp;

    ip    = rushHost;
    port  = rushPort;

    snprintf(rushHeaderPortAsString, sizeof(rushHeaderPortAsString), "%d", rushHeaderPort);
    headerRushHttp = "X-relayer-host: " + rushHeaderIP + ":" + rushHeaderPortAsString;
    LM_T(LmtHttpHeaders, ("HTTP-HEADERS: '%s'", headerRushHttp.c_str()));
    httpHeaderAdd(&headers,
                  "X-relayer-host",
                  headerRushHttp,
                  &outgoingMsgSize,
                  extraHeaders,
                  usedExtraHeaders);

    if (protocol == "https:")
    {
      headerRushHttp = "X-relayer-protocol: https";
      LM_T(LmtHttpHeaders, ("HTTP-HEADERS: '%s'", headerRushHttp.c_str()));
      httpHeaderAdd(&headers, "X-relayer-protocol", headerRushHttp, &outgoingMsgSize, extraHeaders, usedExtraHeaders);
    }
  }

  snprintf(portAsString, sizeof(portAsString), "%u", port);

  // ----- User Agent
  char cvBuf[CURL_VERSION_MAX_LENGTH];
  char headerUserAgent[HTTP_HEADER_USER_AGENT_MAX_LENGTH];

  snprintf(headerUserAgent, sizeof(headerUserAgent), "User-Agent: orion/%s libcurl/%s", versionGet(), curlVersionGet(cvBuf, sizeof(cvBuf)));
  LM_W(("KZ: libcurl version: %s", cvBuf));
  LM_T(LmtHttpHeaders, ("HTTP-HEADERS: '%s'", headerUserAgent));
  httpHeaderAdd(&headers, "User-Agent", headerUserAgent, &outgoingMsgSize, extraHeaders, usedExtraHeaders);

  // ----- Host
  char headerHost[HTTP_HEADER_HOST_MAX_LENGTH];

  snprintf(headerHost, sizeof(headerHost), "Host: %s:%d", ip.c_str(), (int) port);
  LM_T(LmtHttpHeaders, ("HTTP-HEADERS: '%s'", headerHost));
  httpHeaderAdd(&headers, "Host", headerHost, &outgoingMsgSize, extraHeaders, usedExtraHeaders);

  // ----- Tenant
  if (tenant != "")
  {
    std::string fiwareService = std::string("fiware-service: ") + tenant;
    httpHeaderAdd(&headers, "fiware-service", fiwareService, &outgoingMsgSize, extraHeaders, usedExtraHeaders);
  }

  // ----- Service-Path
  if (servicePath != "")
  {
    std::string fiwareServicePath = std::string("Fiware-ServicePath: ") + servicePath;
    httpHeaderAdd(&headers, "Fiware-ServicePath", fiwareServicePath, &outgoingMsgSize, extraHeaders, usedExtraHeaders);
  }

  // ----- X-Auth-Token
  if (xauthToken != "")
  {
    std::string xauthTokenString = std::string("X-Auth-Token: ") + xauthToken;
    httpHeaderAdd(&headers, "X-Auth-Token", xauthTokenString, &outgoingMsgSize, extraHeaders, usedExtraHeaders);
  }

  // ----- Accept
  std::string acceptedFormats = "application/json";
  if (acceptFormat != "")
  {
    acceptedFormats = acceptFormat;
  }

  std::string acceptString = "Accept: " + acceptedFormats;
  httpHeaderAdd(&headers, "Accept", acceptString, &outgoingMsgSize, extraHeaders, usedExtraHeaders);

  // ----- Expect
  httpHeaderAdd(&headers, "Expect", "Expect: ", &outgoingMsgSize, extraHeaders, usedExtraHeaders);

  // ----- Content-length
  std::stringstream contentLengthStringStream;
  contentLengthStringStream << content.size();
  std::string headerContentLength = "Content-length: " + contentLengthStringStream.str();
  LM_T(LmtHttpHeaders, ("HTTP-HEADERS: '%s'", headerContentLength.c_str()));
  httpHeaderAdd(&headers, "Content-length", headerContentLength, &outgoingMsgSize, extraHeaders, usedExtraHeaders);

  // Add the size of the actual payload
  outgoingMsgSize += content.size();

  // ----- Content-type
  std::string contentTypeString = std::string("Content-type: ") + content_type;
  httpHeaderAdd(&headers, "Content-type", contentTypeString, &outgoingMsgSize, extraHeaders, usedExtraHeaders);

  // Fiware-Correlator
  std::string correlation = "Fiware-Correlator: " + fiwareCorrelation;
  httpHeaderAdd(&headers, "Fiware-Correlator", correlation, &outgoingMsgSize, extraHeaders, usedExtraHeaders);

  // Notify Format
  if ((ngisv2AttrFormat != "") && (ngisv2AttrFormat != "JSON") && (ngisv2AttrFormat != "legacy"))
  {
    std::string nFormat = "Ngsiv2-AttrsFormat: " + ngisv2AttrFormat;

    httpHeaderAdd(&headers, "Ngsiv2-AttrsFormat", nFormat, &outgoingMsgSize, extraHeaders, usedExtraHeaders);
  }

  // Extra headers
  for (std::map<std::string, std::string>::const_iterator it = extraHeaders.begin(); it != extraHeaders.end(); ++it)
  {
    std::string headerNameLowerCase = it->first;
    transform(headerNameLowerCase.begin(), headerNameLowerCase.end(), headerNameLowerCase.begin(), ::tolower);

    if (!usedExtraHeaders[headerNameLowerCase])
    {
      std::string header = it->first + ": " + it->second;
      
      headers = curl_slist_append(headers, header.c_str());
      outgoingMsgSize += header.size();
      LM_W(("KZ: Added curl extra header '%s'", header.c_str()));
    }  
  }


  // Check if total outgoing message size is too big
  if (outgoingMsgSize > MAX_DYN_MSG_SIZE)
  {
    LM_E(("Runtime Error (HTTP request to send is too large: %d bytes)", outgoingMsgSize));

    curl_slist_free_all(headers);

    free(httpResponse->memory);
    delete httpResponse;

    lmTransactionEnd();
    *outP = "error";
    return -7;
  }

  // Contents
  const char* payload = content.c_str();
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, (u_int8_t*) payload);
  LM_W(("KZ: Added curl payload"));

  // Set up URL
  std::string url;
  if (isIPv6(ip))
    url = "[" + ip + "]";
  else
    url = ip;
  url = url + ":" + portAsString + (resource.at(0) == '/'? "" : "/") + resource;

  // Prepare CURL handle with obtained options
  LM_W(("KZ: Setting CURLOPT_URL: '%s' (not used protocol parameter == '%s')", url.c_str(), protocol.c_str()));
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  LM_W(("KZ: Setting CURLOPT_CUSTOMREQUEST: '%s'", verb.c_str()));
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, verb.c_str()); // Set HTTP verb
  LM_W(("KZ: Setting CURLOPT_FOLLOWLOCATION: 1"));
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L); // Allow redirection (?)
  LM_W(("KZ: Setting CURLOPT_HEADER: 1"));
  curl_easy_setopt(curl, CURLOPT_HEADER, 1); // Activate include the header in the body output
  LM_W(("KZ: Setting CURLOPT_HTTPHEADER: (list of headers)"));
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers); // Put headers in place
  LM_W(("KZ: Setting CURLOPT_WRITEFUNCTION: writeMemoryCallback (%p)", writeMemoryCallback));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeMemoryCallback); // Send data here
  LM_W(("KZ: Setting CURLOPT_WRITEDATA: httpResponse (%p)", httpResponse));
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*) httpResponse); // Custom data for response handling

  //
  // There is a known problem in libcurl (see http://stackoverflow.com/questions/9191668/error-longjmp-causes-uninitialized-stack-frame)
  // which is solved using CURLOPT_NOSIGNAL. If we update some day from libcurl 7.19 (the one that comes with CentOS 6.x) to a newer version
  // (there are some reports about the bug is no longer in libcurl 7.32), using CURLOPT_NOSIGNAL could be not necessary and this be removed).
  // See issue #1016 for more details.
  //
  LM_W(("KZ: Setting CURLOPT_NOSIGNAL: 1 (to avoid some known problem in libcurl)"));
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);

  //
  // Timeout 
  //
  // The parameter timeoutInMilliseconds holds the timeout time in milliseconds.
  // If the timeout time requested is 0, then no timeuot is used.
  //
  if (timeoutInMilliseconds != 0) 
  {
    LM_W(("KZ: Setting CURLOPT_TIMEOUT_MS: %d", timeoutInMilliseconds));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeoutInMilliseconds);
  }


  // Synchronous HTTP request
  // This was previously a LM_T trace, but we have "promoted" it to INFO due to it is needed to check logs in a .test case (case 000 notification_different_sizes.test)
  LM_I(("Sending message %lu to HTTP server: sending message of %d bytes to HTTP server", callNo, outgoingMsgSize));
  LM_W(("KZ: calling curl_easy_perform"));
  res = curl_easy_perform(curl);
  LM_W(("KZ: curl_easy_perform returned %d", res));
  LM_W(("KZ: ---------------------------------------------------------------------"));
  if (res != CURLE_OK)
  {
    //
    // NOTE: This log line is used by the functional tests in cases/880_timeout_for_forward_and_notifications/
    //       So, this line should not be removed/altered, at least not without also modifying the functests.
    //    
    alarmMgr.notificationError(url, "(curl_easy_perform failed: " + std::string(curl_easy_strerror(res)) + ")");
    *outP = "notification failure";
  }
  else
  {
    // The Response is here
    LM_I(("Notification Successfully Sent to %s", url.c_str()));
    outP->assign(httpResponse->memory, httpResponse->size);
  }

  // Cleanup curl environment

  curl_slist_free_all(headers);

  free(httpResponse->memory);
  delete httpResponse;

  lmTransactionEnd();

  return res == CURLE_OK ? 0 : -9;
}



/* ****************************************************************************
*
* httpRequestSend - 
*
* RETURN VALUES
*   httpRequestSend returns 0 on success and a negative number on failure:
*     -1: Invalid port
*     -2: Invalid IP
*     -3: Invalid verb
*     -4: Invalid resource
*     -5: No Content-Type BUT content present
*     -6: Content-Type present but there is no content
*     -7: Total outgoing message size is too big
*     -8: Unable to initialize libcurl
*     -9: Error making HTTP request
*
*   [ error codes -1 to -7 comes from httpRequestSendWithCurl ]
*/
int httpRequestSend
(
   const std::string&                         _ip,
   unsigned short                             port,
   const std::string&                         protocol,
   const std::string&                         verb,
   const std::string&                         tenant,
   const std::string&                         servicePath,
   const std::string&                         xauthToken,
   const std::string&                         resource,
   const std::string&                         orig_content_type,
   const std::string&                         content,
   const std::string&                         fiwareCorrelation,
   const std::string&                         ngisv2AttrFormat,
   bool                                       useRush,
   bool                                       waitForResponse,
   std::string*                               outP,
   const std::map<std::string, std::string>&  extraHeaders,
   const std::string&                         acceptFormat,
   long                                       timeoutInMilliseconds
)
{
  struct curl_context  cc;
  int                  response;

  get_curl_context(_ip, &cc);
  if (cc.curl == NULL)
  {
    release_curl_context(&cc);
    LM_E(("Runtime Error (could not init libcurl)"));
    lmTransactionEnd();

    *outP = "error";
    return -8;
  }

  response = httpRequestSendWithCurl(cc.curl,
                                     _ip,
                                     port,
                                     protocol,
                                     verb,
                                     tenant,
                                     servicePath,
                                     xauthToken,
                                     resource,
                                     orig_content_type,
                                     content,
                                     fiwareCorrelation,
                                     ngisv2AttrFormat,
                                     useRush,
                                     waitForResponse,
                                     outP,
                                     extraHeaders,
                                     acceptFormat,
                                     timeoutInMilliseconds);

  release_curl_context(&cc);
  return response;
}
