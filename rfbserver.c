/*
 * rfbserver.c - deal with server-side of the RFB protocol.
 */

/*
 *  OSXvnc Copyright (C) 2001 Dan McGuirk <mcguirk@incompleteness.net>.
 *  Original Xvnc code Copyright (C) 1999 AT&T Laboratories Cambridge.  
 *  All Rights Reserved.
 *
 *  This is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This software is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this software; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 *  USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#ifdef HAVE_PTHREADS
#include <pthread.h>
#endif
#include "rfb.h"

rfbClientPtr pointerClient = NULL;  /* Mutex for pointer events */

static void rfbProcessClientProtocolVersion(rfbClientPtr cl);
static void rfbProcessClientNormalMessage(rfbClientPtr cl);
static void rfbProcessClientInitMessage(rfbClientPtr cl);


struct rfbClientIterator {
    rfbClientPtr next;
};

#ifdef HAVE_PTHREADS
static pthread_mutex_t rfbClientListMutex;
#endif
static struct rfbClientIterator rfbClientIteratorInstance;

void
rfbClientListInit(rfbScreenInfoPtr rfbScreen)
{
    rfbScreen->rfbClientHead = NULL;
#ifdef HAVE_PTHREADS
    pthread_mutex_init(&rfbClientListMutex, NULL);
#endif
}

rfbClientIteratorPtr
rfbGetClientIterator(rfbScreenInfoPtr rfbScreen)
{
#ifdef HAVE_PTHREADS
    pthread_mutex_lock(&rfbClientListMutex);
#endif
    rfbClientIteratorInstance.next = rfbScreen->rfbClientHead;
    
    return &rfbClientIteratorInstance;
}

rfbClientPtr
rfbClientIteratorNext(rfbClientIteratorPtr iterator)
{
    rfbClientPtr result;
    result = iterator->next;
    if (result)
        iterator->next = result->next;
    return result;
}

void
rfbReleaseClientIterator(rfbClientIteratorPtr iterator)
{
#ifdef HAVE_PTHREADS
    pthread_mutex_unlock(&rfbClientListMutex);
#endif
}


/*
 * rfbNewClientConnection is called from sockets.c when a new connection
 * comes in.
 */

void
rfbNewClientConnection(rfbScreen,sock)
    rfbScreenInfoPtr rfbScreen;
    int sock;
{
    rfbClientPtr cl;

    cl = rfbNewClient(rfbScreen,sock);
}


/*
 * rfbReverseConnection is called by the CORBA stuff to make an outward
 * connection to a "listening" RFB client.
 */

rfbClientPtr
rfbReverseConnection(host, port)
    char *host;
    int port;
{
    return NULL;

#ifdef NOT_YET
    int sock;
    rfbClientPtr cl;

    if ((sock = rfbConnect(host, port)) < 0)
        return (rfbClientPtr)NULL;

    cl = rfbNewClient(sock);

    if (cl) {
        cl->reverseConnection = TRUE;
    }

    return cl;
#endif
}


/*
 * rfbNewClient is called when a new connection has been made by whatever
 * means.
 */

rfbClientPtr
rfbNewClient(rfbScreen,sock)
    rfbScreenInfoPtr rfbScreen;
    int sock;
{
    rfbProtocolVersionMsg pv;
    rfbClientIteratorPtr iterator;
    rfbClientPtr cl;
    BoxRec box;
    struct sockaddr_in addr;
    int addrlen = sizeof(struct sockaddr_in);
    int i;

    rfbLog("  other clients:\n");
    iterator = rfbGetClientIterator(rfbScreen);
    while ((cl = rfbClientIteratorNext(iterator)) != NULL) {
        rfbLog("     %s\n",cl->host);
    }
    rfbReleaseClientIterator(iterator);

    cl = (rfbClientPtr)xalloc(sizeof(rfbClientRec));

    cl->screen = rfbScreen;
    cl->sock = sock;
    getpeername(sock, (struct sockaddr *)&addr, &addrlen);
    cl->host = strdup(inet_ntoa(addr.sin_addr));

#ifdef HAVE_PTHREADS    
    pthread_mutex_init(&cl->outputMutex, NULL);
#endif

    cl->state = RFB_PROTOCOL_VERSION;

    cl->reverseConnection = FALSE;
    cl->readyForSetColourMapEntries = FALSE;
    cl->useCopyRect = FALSE;
    cl->preferredEncoding = rfbEncodingRaw;
    cl->correMaxWidth = 48;
    cl->correMaxHeight = 48;

    REGION_INIT(rfbScreen,&cl->copyRegion,NullBox,0);
    cl->copyDX = 0;
    cl->copyDY = 0;
   
    box.x1 = box.y1 = 0;
    box.x2 = rfbScreen->width;
    box.y2 = rfbScreen->height;
    REGION_INIT(pScreen,&cl->modifiedRegion,&box,0);

#ifdef HAVE_PTHREADS
    pthread_mutex_init(&cl->updateMutex, NULL);
    pthread_cond_init(&cl->updateCond, NULL);
#endif

    REGION_INIT(pScreen,&cl->requestedRegion,NullBox,0);

    cl->format = cl->screen->rfbServerFormat;
    cl->translateFn = rfbTranslateNone;
    cl->translateLookupTable = NULL;

#ifdef HAVE_PTHREADS
    pthread_mutex_lock(&rfbClientListMutex);
#endif
    cl->next = rfbScreen->rfbClientHead;
    cl->prev = NULL;
    if (rfbScreen->rfbClientHead)
        rfbScreen->rfbClientHead->prev = cl;

    rfbScreen->rfbClientHead = cl;
#ifdef HAVE_PTHREADS
    pthread_mutex_unlock(&rfbClientListMutex);
#endif

    cl->tightCompressLevel = TIGHT_DEFAULT_COMPRESSION;
    cl->tightQualityLevel = -1;
    for (i = 0; i < 4; i++)
        cl->zsActive[i] = FALSE;

    cl->enableCursorShapeUpdates = FALSE;
    cl->useRichCursorEncoding = FALSE;
    cl->enableLastRectEncoding = FALSE;

    rfbResetStats(cl);

    cl->compStreamInited = FALSE;
    cl->compStream.total_in = 0;
    cl->compStream.total_out = 0;
    cl->compStream.zalloc = Z_NULL;
    cl->compStream.zfree = Z_NULL;
    cl->compStream.opaque = Z_NULL;

    cl->zlibCompressLevel = 5;

    sprintf(pv,rfbProtocolVersionFormat,rfbProtocolMajorVersion,
            rfbProtocolMinorVersion);

    cl->clientData = NULL;
    cl->clientGoneHook = doNothingWithClient;
    cl->screen->newClientHook(cl);

    if (WriteExact(cl, pv, sz_rfbProtocolVersionMsg) < 0) {
        rfbLogPerror("rfbNewClient: write");
        rfbCloseClient(cl);
        return NULL;
    }

    return cl;
}


/*
 * rfbClientConnectionGone is called from sockets.c just after a connection
 * has gone away.
 */

void
rfbClientConnectionGone(cl)
     rfbClientPtr cl;
{
    int i;
#ifdef HAVE_PTHREADS
    pthread_mutex_lock(&rfbClientListMutex);
#endif

    cl->clientGoneHook(cl);

    rfbLog("Client %s gone\n",cl->host);
    free(cl->host);

    /* Release the compression state structures if any. */
    if ( cl->compStreamInited == TRUE ) {
	deflateEnd( &(cl->compStream) );
    }

    for (i = 0; i < 4; i++) {
	if (cl->zsActive[i])
	    deflateEnd(&cl->zsStruct[i]);
    }

    if (pointerClient == cl)
        pointerClient = NULL;

    if (cl->prev)
        cl->prev->next = cl->next;
    else
        cl->screen->rfbClientHead = cl->next;
    if (cl->next)
        cl->next->prev = cl->prev;

#ifdef HAVE_PTHREADS
    pthread_mutex_unlock(&rfbClientListMutex);
#endif

    REGION_UNINIT(pScreen,&cl->modifiedRegion);

    rfbPrintStats(cl);

    if (cl->translateLookupTable) free(cl->translateLookupTable);

#ifdef HAVE_PTHREADS
    pthread_cond_destroy(&cl->updateCond);
    pthread_mutex_destroy(&cl->updateMutex);
    pthread_mutex_destroy(&cl->outputMutex);
#endif

    xfree(cl);
}


/*
 * rfbProcessClientMessage is called when there is data to read from a client.
 */

void
rfbProcessClientMessage(cl)
     rfbClientPtr cl;
{
    switch (cl->state) {
    case RFB_PROTOCOL_VERSION:
        rfbProcessClientProtocolVersion(cl);
        return;
    case RFB_AUTHENTICATION:
        rfbAuthProcessClientMessage(cl);
        return;
    case RFB_INITIALISATION:
        rfbProcessClientInitMessage(cl);
        return;
    default:
        rfbProcessClientNormalMessage(cl);
        return;
    }
}


/*
 * rfbProcessClientProtocolVersion is called when the client sends its
 * protocol version.
 */

static void
rfbProcessClientProtocolVersion(cl)
    rfbClientPtr cl;
{
    rfbProtocolVersionMsg pv;
    int n, major, minor;
    char failureReason[256];

    if ((n = ReadExact(cl, pv, sz_rfbProtocolVersionMsg)) <= 0) {
        if (n == 0)
            rfbLog("rfbProcessClientProtocolVersion: client gone\n");
        else
            rfbLogPerror("rfbProcessClientProtocolVersion: read");
        rfbCloseClient(cl);
        return;
    }

    pv[sz_rfbProtocolVersionMsg] = 0;
    if (sscanf(pv,rfbProtocolVersionFormat,&major,&minor) != 2) {
        rfbLog("rfbProcessClientProtocolVersion: not a valid RFB client\n");
        rfbCloseClient(cl);
        return;
    }
    rfbLog("Protocol version %d.%d\n", major, minor);

    if (major != rfbProtocolMajorVersion) {
        /* Major version mismatch - send a ConnFailed message */

        rfbLog("Major version mismatch\n");
        sprintf(failureReason,
                "RFB protocol version mismatch - server %d.%d, client %d.%d",
                rfbProtocolMajorVersion,rfbProtocolMinorVersion,major,minor);
        rfbClientConnFailed(cl, failureReason);
        return;
    }

    if (minor != rfbProtocolMinorVersion) {
        /* Minor version mismatch - warn but try to continue */
        rfbLog("Ignoring minor version mismatch\n");
    }

    rfbAuthNewClient(cl);
}


/*
 * rfbClientConnFailed is called when a client connection has failed either
 * because it talks the wrong protocol or it has failed authentication.
 */

void
rfbClientConnFailed(cl, reason)
    rfbClientPtr cl;
    char *reason;
{
    char *buf;
    int len = strlen(reason);

    buf = (char *)xalloc(8 + len);
    ((CARD32 *)buf)[0] = Swap32IfLE(rfbConnFailed);
    ((CARD32 *)buf)[1] = Swap32IfLE(len);
    memcpy(buf + 8, reason, len);

    if (WriteExact(cl, buf, 8 + len) < 0)
        rfbLogPerror("rfbClientConnFailed: write");
    xfree(buf);
    rfbCloseClient(cl);
}


/*
 * rfbProcessClientInitMessage is called when the client sends its
 * initialisation message.
 */

static void
rfbProcessClientInitMessage(cl)
    rfbClientPtr cl;
{
    rfbClientInitMsg ci;
    char buf[256];
    rfbServerInitMsg *si = (rfbServerInitMsg *)buf;
    int len, n;
    rfbClientIteratorPtr iterator;
    rfbClientPtr otherCl;

    if ((n = ReadExact(cl, (char *)&ci,sz_rfbClientInitMsg)) <= 0) {
        if (n == 0)
            rfbLog("rfbProcessClientInitMessage: client gone\n");
        else
            rfbLogPerror("rfbProcessClientInitMessage: read");
        rfbCloseClient(cl);
        return;
    }

    si->framebufferWidth = Swap16IfLE(cl->screen->width);
    si->framebufferHeight = Swap16IfLE(cl->screen->height);
    si->format = cl->screen->rfbServerFormat;
    si->format.redMax = Swap16IfLE(si->format.redMax);
    si->format.greenMax = Swap16IfLE(si->format.greenMax);
    si->format.blueMax = Swap16IfLE(si->format.blueMax);

    if (strlen(cl->screen->desktopName) > 128)      /* sanity check on desktop name len */
        cl->screen->desktopName[128] = 0;

    strcpy(buf + sz_rfbServerInitMsg, cl->screen->desktopName);
    len = strlen(buf + sz_rfbServerInitMsg);
    si->nameLength = Swap32IfLE(len);

    if (WriteExact(cl, buf, sz_rfbServerInitMsg + len) < 0) {
        rfbLogPerror("rfbProcessClientInitMessage: write");
        rfbCloseClient(cl);
        return;
    }

    cl->state = RFB_NORMAL;

    if (!cl->reverseConnection &&
                        (cl->screen->rfbNeverShared || (!cl->screen->rfbAlwaysShared && !ci.shared))) {

        if (cl->screen->rfbDontDisconnect) {
            iterator = rfbGetClientIterator(cl->screen);
            while ((otherCl = rfbClientIteratorNext(iterator)) != NULL) {
                if ((otherCl != cl) && (otherCl->state == RFB_NORMAL)) {
                    rfbLog("-dontdisconnect: Not shared & existing client\n");
                    rfbLog("  refusing new client %s\n", cl->host);
                    rfbCloseClient(cl);
                    rfbReleaseClientIterator(iterator);
                    return;
                }
            }
            rfbReleaseClientIterator(iterator);
        } else {
            iterator = rfbGetClientIterator(cl->screen);
            while ((otherCl = rfbClientIteratorNext(iterator)) != NULL) {
                if ((otherCl != cl) && (otherCl->state == RFB_NORMAL)) {
                    rfbLog("Not shared - closing connection to client %s\n",
                           otherCl->host);
                    rfbCloseClient(otherCl);
                }
            }
            rfbReleaseClientIterator(iterator);
        }
    }
}


/*
 * rfbProcessClientNormalMessage is called when the client has sent a normal
 * protocol message.
 */

static void
rfbProcessClientNormalMessage(cl)
    rfbClientPtr cl;
{
    int n=0;
    rfbClientToServerMsg msg;
    char *str;

    if ((n = ReadExact(cl, (char *)&msg, 1)) <= 0) {
        if (n != 0)
            rfbLogPerror("rfbProcessClientNormalMessage: read");
        rfbCloseClient(cl);
        return;
    }

    switch (msg.type) {

    case rfbSetPixelFormat:

        if ((n = ReadExact(cl, ((char *)&msg) + 1,
                           sz_rfbSetPixelFormatMsg - 1)) <= 0) {
            if (n != 0)
                rfbLogPerror("rfbProcessClientNormalMessage: read");
            rfbCloseClient(cl);
            return;
        }

        cl->format.bitsPerPixel = msg.spf.format.bitsPerPixel;
        cl->format.depth = msg.spf.format.depth;
        cl->format.bigEndian = (msg.spf.format.bigEndian ? 1 : 0);
        cl->format.trueColour = (msg.spf.format.trueColour ? 1 : 0);
        cl->format.redMax = Swap16IfLE(msg.spf.format.redMax);
        cl->format.greenMax = Swap16IfLE(msg.spf.format.greenMax);
        cl->format.blueMax = Swap16IfLE(msg.spf.format.blueMax);
        cl->format.redShift = msg.spf.format.redShift;
        cl->format.greenShift = msg.spf.format.greenShift;
        cl->format.blueShift = msg.spf.format.blueShift;

	cl->readyForSetColourMapEntries = TRUE;

        rfbSetTranslateFunction(cl);
        return;


    case rfbFixColourMapEntries:
        if ((n = ReadExact(cl, ((char *)&msg) + 1,
                           sz_rfbFixColourMapEntriesMsg - 1)) <= 0) {
            if (n != 0)
                rfbLogPerror("rfbProcessClientNormalMessage: read");
            rfbCloseClient(cl);
            return;
        }
        rfbLog("rfbProcessClientNormalMessage: %s",
                "FixColourMapEntries unsupported\n");
        rfbCloseClient(cl);
        return;


    case rfbSetEncodings:
    {
        int i;
        CARD32 enc;

        if ((n = ReadExact(cl, ((char *)&msg) + 1,
                           sz_rfbSetEncodingsMsg - 1)) <= 0) {
            if (n != 0)
                rfbLogPerror("rfbProcessClientNormalMessage: read");
            rfbCloseClient(cl);
            return;
        }

        msg.se.nEncodings = Swap16IfLE(msg.se.nEncodings);

        cl->preferredEncoding = -1;
	cl->useCopyRect = FALSE;
	cl->enableCursorShapeUpdates = FALSE;
	cl->enableLastRectEncoding = FALSE;

        for (i = 0; i < msg.se.nEncodings; i++) {
            if ((n = ReadExact(cl, (char *)&enc, 4)) <= 0) {
                if (n != 0)
                    rfbLogPerror("rfbProcessClientNormalMessage: read");
                rfbCloseClient(cl);
                return;
            }
            enc = Swap32IfLE(enc);

            switch (enc) {

            case rfbEncodingCopyRect:
		cl->useCopyRect = TRUE;
                break;
            case rfbEncodingRaw:
                if (cl->preferredEncoding == -1) {
                    cl->preferredEncoding = enc;
                    rfbLog("Using raw encoding for client %s\n",
                           cl->host);
                }
                break;
            case rfbEncodingRRE:
                if (cl->preferredEncoding == -1) {
                    cl->preferredEncoding = enc;
                    rfbLog("Using rre encoding for client %s\n",
                           cl->host);
                }
                break;
            case rfbEncodingCoRRE:
                if (cl->preferredEncoding == -1) {
                    cl->preferredEncoding = enc;
                    rfbLog("Using CoRRE encoding for client %s\n",
                           cl->host);
                }
                break;
            case rfbEncodingHextile:
                if (cl->preferredEncoding == -1) {
                    cl->preferredEncoding = enc;
                    rfbLog("Using hextile encoding for client %s\n",
                           cl->host);
                }
                break;
	    case rfbEncodingZlib:
		if (cl->preferredEncoding == -1) {
		    cl->preferredEncoding = enc;
		    rfbLog("Using zlib encoding for client %s\n",
			   cl->host);
		}
              break;
	    case rfbEncodingTight:
		if (cl->preferredEncoding == -1) {
		    cl->preferredEncoding = enc;
		    rfbLog("Using tight encoding for client %s\n",
			   cl->host);
		}
		break;
	    case rfbEncodingXCursor:
		rfbLog("Enabling X-style cursor updates for client %s\n",
		       cl->host);
		cl->enableCursorShapeUpdates = TRUE;
		cl->cursorWasChanged = TRUE;
		break;
	    case rfbEncodingRichCursor:
	        rfbLog("Enabling full-color cursor updates for client "
		      "%s\n", cl->host);
	        cl->enableCursorShapeUpdates = TRUE;
	        cl->useRichCursorEncoding = TRUE;
	        cl->cursorWasChanged = TRUE;
	        break;
	    case rfbEncodingLastRect:
		if (!cl->enableLastRectEncoding) {
		    rfbLog("Enabling LastRect protocol extension for client "
			   "%s\n", cl->host);
		    cl->enableLastRectEncoding = TRUE;
		}
		break;
            default:
		if ( enc >= (CARD32)rfbEncodingCompressLevel0 &&
		     enc <= (CARD32)rfbEncodingCompressLevel9 ) {
		    cl->zlibCompressLevel = enc & 0x0F;
		    cl->tightCompressLevel = enc & 0x0F;
		    rfbLog("Using compression level %d for client %s\n",
			   cl->tightCompressLevel, cl->host);
		} else if ( enc >= (CARD32)rfbEncodingQualityLevel0 &&
			    enc <= (CARD32)rfbEncodingQualityLevel9 ) {
		    cl->tightQualityLevel = enc & 0x0F;
		    rfbLog("Using image quality level %d for client %s\n",
			   cl->tightQualityLevel, cl->host);
		} else
		 rfbLog("rfbProcessClientNormalMessage: ignoring unknown "
                       "encoding type %d\n", (int)enc);
            }
        }

        if (cl->preferredEncoding == -1) {
            cl->preferredEncoding = rfbEncodingRaw;
        }

        return;
    }


    case rfbFramebufferUpdateRequest:
    {
        RegionRec tmpRegion;
        BoxRec box;

        if ((n = ReadExact(cl, ((char *)&msg) + 1,
                           sz_rfbFramebufferUpdateRequestMsg-1)) <= 0) {
            if (n != 0)
                rfbLogPerror("rfbProcessClientNormalMessage: read");
            rfbCloseClient(cl);
            return;
        }

        box.x1 = Swap16IfLE(msg.fur.x);
        box.y1 = Swap16IfLE(msg.fur.y);
        box.x2 = box.x1 + Swap16IfLE(msg.fur.w);
        box.y2 = box.y1 + Swap16IfLE(msg.fur.h);
        SAFE_REGION_INIT(pScreen,&tmpRegion,&box,0);

#ifdef HAVE_PTHREADS
        pthread_mutex_lock(&cl->updateMutex);
#endif
        REGION_UNION(pScreen, &cl->requestedRegion, &cl->requestedRegion,
                     &tmpRegion);

#ifdef NOT_YET
	if (!cl->readyForSetColourMapEntries) {
	    /* client hasn't sent a SetPixelFormat so is using server's */
	    cl->readyForSetColourMapEntries = TRUE;
	    if (!cl->format.trueColour) {
		if (!rfbSetClientColourMap(cl, 0, 0)) {
		    REGION_UNINIT(pScreen,&tmpRegion);
		    return;
		}
	    }
	}
#endif

       if (!msg.fur.incremental) {
            REGION_UNION(pScreen,&cl->modifiedRegion,&cl->modifiedRegion,
                         &tmpRegion);
	    REGION_SUBTRACT(pScreen,&cl->copyRegion,&cl->copyRegion,&tmpRegion);
        }
#ifdef HAVE_PTHREADS
        pthread_cond_signal(&cl->updateCond);
        pthread_mutex_unlock(&cl->updateMutex);
#endif
	
	if(cl->sock>=0 && FB_UPDATE_PENDING(cl)) {
	  rfbSendFramebufferUpdate(cl,cl->modifiedRegion);
	}
	
        REGION_UNINIT(pScreen,&tmpRegion);

        return;
    }

    case rfbKeyEvent:

        cl->rfbKeyEventsRcvd++;

        if ((n = ReadExact(cl, ((char *)&msg) + 1,
                           sz_rfbKeyEventMsg - 1)) <= 0) {
            if (n != 0)
                rfbLogPerror("rfbProcessClientNormalMessage: read");
            rfbCloseClient(cl);
            return;
        }

        cl->screen->kbdAddEvent(msg.ke.down, (KeySym)Swap32IfLE(msg.ke.key), cl);
        return;


    case rfbPointerEvent:

        cl->rfbPointerEventsRcvd++;

        if ((n = ReadExact(cl, ((char *)&msg) + 1,
                           sz_rfbPointerEventMsg - 1)) <= 0) {
            if (n != 0)
                rfbLogPerror("rfbProcessClientNormalMessage: read");
            rfbCloseClient(cl);
            return;
        }

        if (pointerClient && (pointerClient != cl))
            return;

        if (msg.pe.buttonMask == 0)
            pointerClient = NULL;
        else
            pointerClient = cl;

        cl->screen->ptrAddEvent(msg.pe.buttonMask,
                    Swap16IfLE(msg.pe.x), Swap16IfLE(msg.pe.y), cl);
        return;


    case rfbClientCutText:

        if ((n = ReadExact(cl, ((char *)&msg) + 1,
                           sz_rfbClientCutTextMsg - 1)) <= 0) {
            if (n != 0)
                rfbLogPerror("rfbProcessClientNormalMessage: read");
            rfbCloseClient(cl);
            return;
        }

        msg.cct.length = Swap32IfLE(msg.cct.length);

        str = (char *)xalloc(msg.cct.length);

        if ((n = ReadExact(cl, str, msg.cct.length)) <= 0) {
            if (n != 0)
                rfbLogPerror("rfbProcessClientNormalMessage: read");
            xfree(str);
            rfbCloseClient(cl);
            return;
        }

        cl->screen->setXCutText(str, msg.cct.length, cl);

        xfree(str);
        return;


    default:

        rfbLog("rfbProcessClientNormalMessage: unknown message type %d\n",
                msg.type);
        rfbLog(" ... closing connection\n");
        rfbCloseClient(cl);
        return;
    }
}



/*
 * rfbSendFramebufferUpdate - send the currently pending framebuffer update to
 * the RFB client.
 */

Bool
rfbSendFramebufferUpdate(cl, updateRegion)
     rfbClientPtr cl;
     RegionRec updateRegion;
{
    int i;
    int nUpdateRegionRects;
    rfbFramebufferUpdateMsg *fu = (rfbFramebufferUpdateMsg *)cl->updateBuf;
    RegionRec updateCopyRegion;
    int dx, dy;
    Bool sendCursorShape = FALSE;
    Bool cursorWasDrawn = FALSE;

    /*
     * If this client understands cursor shape updates, cursor should be
     * removed from the framebuffer. Otherwise, make sure it's put up.
     */

   cursorWasDrawn = cl->screen->cursorIsDrawn;

   if (cl->enableCursorShapeUpdates) {
      if (cl->screen->cursorIsDrawn) {
	 rfbUndrawCursor(cl);
	 //fprintf(stderr,"rfbSpriteRemoveCursor(pScreen); not yet!\n");
      }
      if (!cl->screen->cursorIsDrawn && cl->cursorWasChanged)
	sendCursorShape = TRUE;
   } else {
      if (!cl->screen->cursorIsDrawn)
	//rfbDrawCursor(cl);
	fprintf(stderr,"rfbSpriteRestoreCursor(pScreen); not yet!\n");
   }
   
    /*
     * The modifiedRegion may overlap the destination copyRegion.  We remove
     * any overlapping bits from the copyRegion (since they'd only be
     * overwritten anyway).
     */

    REGION_SUBTRACT(pScreen, &cl->copyRegion, &cl->copyRegion,
		    &cl->modifiedRegion);

    /*
     * The client is interested in the region requestedRegion.  The region
     * which should be updated now is the intersection of requestedRegion
     * and the union of modifiedRegion and copyRegion.  If it's empty then
     * no update is needed.
     */

    //REGION_INIT(pScreen,&updateRegion,NullBox,0);
    //REGION_UNION(pScreen, &updateRegion, &cl->copyRegion,
	//	 &cl->modifiedRegion);
    REGION_INTERSECT(pScreen, &updateRegion, &cl->requestedRegion,
		     &updateRegion);

    if (!REGION_NOTEMPTY(pScreen,&updateRegion) && !sendCursorShape) {
	REGION_UNINIT(pScreen,&updateRegion);
	return TRUE;
    }

    /*
     * We assume that the client doesn't have any pixel data outside the
     * requestedRegion.  In other words, both the source and destination of a
     * copy must lie within requestedRegion.  So the region we can send as a
     * copy is the intersection of the copyRegion with both the requestedRegion
     * and the requestedRegion translated by the amount of the copy.  We set
     * updateCopyRegion to this.
     */

    REGION_INIT(pScreen,&updateCopyRegion,NullBox,0);
    REGION_INTERSECT(pScreen, &updateCopyRegion, &cl->copyRegion,
		     &cl->requestedRegion);
    REGION_TRANSLATE(pScreen, &cl->requestedRegion, cl->copyDX, cl->copyDY);
    REGION_INTERSECT(pScreen, &updateCopyRegion, &updateCopyRegion,
		     &cl->requestedRegion);
    dx = cl->copyDX;
    dy = cl->copyDY;

    /*
     * Next we remove updateCopyRegion from updateRegion so that updateRegion
     * is the part of this update which is sent as ordinary pixel data (i.e not
     * a copy).
     */

    REGION_SUBTRACT(pScreen, &updateRegion, &updateRegion, &updateCopyRegion);

    /*
     * Finally we leave modifiedRegion to be the remainder (if any) of parts of
     * the screen which are modified but outside the requestedRegion.  We also
     * empty both the requestedRegion and the copyRegion - note that we never
     * carry over a copyRegion for a future update.
     */


    REGION_UNION(pScreen, &cl->modifiedRegion, &cl->modifiedRegion,
		                  &cl->copyRegion);
       REGION_SUBTRACT(pScreen, &cl->modifiedRegion, &cl->modifiedRegion,
		                           &updateRegion);
       REGION_SUBTRACT(pScreen, &cl->modifiedRegion, &cl->modifiedRegion,
		                           &updateCopyRegion);
   
       REGION_EMPTY(pScreen, &cl->requestedRegion);
       REGION_EMPTY(pScreen, &cl->copyRegion);
       cl->copyDX = 0;
       cl->copyDY = 0;
   

   /*
     * Now send the update.
     */

    cl->rfbFramebufferUpdateMessagesSent++;

    if (cl->preferredEncoding == rfbEncodingCoRRE) {
        nUpdateRegionRects = 0;

        for (i = 0; i < REGION_NUM_RECTS(&updateRegion); i++) {
            int x = REGION_RECTS(&updateRegion)[i].x1;
            int y = REGION_RECTS(&updateRegion)[i].y1;
            int w = REGION_RECTS(&updateRegion)[i].x2 - x;
            int h = REGION_RECTS(&updateRegion)[i].y2 - y;
            nUpdateRegionRects += (((w-1) / cl->correMaxWidth + 1)
                                     * ((h-1) / cl->correMaxHeight + 1));
        }
    } else if (cl->preferredEncoding == rfbEncodingZlib) {
	nUpdateRegionRects = 0;

	for (i = 0; i < REGION_NUM_RECTS(&updateRegion); i++) {
	    int x = REGION_RECTS(&updateRegion)[i].x1;
	    int y = REGION_RECTS(&updateRegion)[i].y1;
	    int w = REGION_RECTS(&updateRegion)[i].x2 - x;
	    int h = REGION_RECTS(&updateRegion)[i].y2 - y;
	    nUpdateRegionRects += (((h-1) / (ZLIB_MAX_SIZE( w ) / w)) + 1);
	}
    } else if (cl->preferredEncoding == rfbEncodingTight) {
	nUpdateRegionRects = 0;

	for (i = 0; i < REGION_NUM_RECTS(&updateRegion); i++) {
	    int x = REGION_RECTS(&updateRegion)[i].x1;
	    int y = REGION_RECTS(&updateRegion)[i].y1;
	    int w = REGION_RECTS(&updateRegion)[i].x2 - x;
	    int h = REGION_RECTS(&updateRegion)[i].y2 - y;
	    int n = rfbNumCodedRectsTight(cl, x, y, w, h);
	    if (n == 0) {
		nUpdateRegionRects = 0xFFFF;
		break;
	    }
	    nUpdateRegionRects += n;
	}
    } else {
        nUpdateRegionRects = REGION_NUM_RECTS(&updateRegion);
    }

    fu->type = rfbFramebufferUpdate;
    if (nUpdateRegionRects != 0xFFFF) {
	fu->nRects = Swap16IfLE(REGION_NUM_RECTS(&updateCopyRegion)
				+ nUpdateRegionRects + !!sendCursorShape);
    } else {
	fu->nRects = 0xFFFF;
    }
    cl->ublen = sz_rfbFramebufferUpdateMsg;

   if (sendCursorShape) {
	cl->cursorWasChanged = FALSE;
	if (!rfbSendCursorShape(cl))
	    return FALSE;
    }
   
    if (REGION_NOTEMPTY(pScreen,&updateCopyRegion)) {
	if (!rfbSendCopyRegion(cl,&updateCopyRegion,dx,dy)) {
	    REGION_UNINIT(pScreen,&updateRegion);
	    REGION_UNINIT(pScreen,&updateCopyRegion);
	    return FALSE;
	}
    }

    REGION_UNINIT(pScreen,&updateCopyRegion);

    for (i = 0; i < REGION_NUM_RECTS(&updateRegion); i++) {
        int x = REGION_RECTS(&updateRegion)[i].x1;
        int y = REGION_RECTS(&updateRegion)[i].y1;
        int w = REGION_RECTS(&updateRegion)[i].x2 - x;
        int h = REGION_RECTS(&updateRegion)[i].y2 - y;

        cl->rfbRawBytesEquivalent += (sz_rfbFramebufferUpdateRectHeader
                                      + w * (cl->format.bitsPerPixel / 8) * h);

        switch (cl->preferredEncoding) {
        case rfbEncodingRaw:
            if (!rfbSendRectEncodingRaw(cl, x, y, w, h)) {
                return FALSE;
            }
            break;
        case rfbEncodingRRE:
            if (!rfbSendRectEncodingRRE(cl, x, y, w, h)) {
                return FALSE;
            }
            break;
        case rfbEncodingCoRRE:
            if (!rfbSendRectEncodingCoRRE(cl, x, y, w, h)) {
                return FALSE;
            }
            break;
        case rfbEncodingHextile:
            if (!rfbSendRectEncodingHextile(cl, x, y, w, h)) {
                return FALSE;
            }
            break;
	case rfbEncodingZlib:
	    if (!rfbSendRectEncodingZlib(cl, x, y, w, h)) {
		REGION_UNINIT(pScreen,&updateRegion);
		return FALSE;
	    }
	    break;
	case rfbEncodingTight:
	    if (!rfbSendRectEncodingTight(cl, x, y, w, h)) {
		REGION_UNINIT(pScreen,&updateRegion);
		return FALSE;
	    }
	    break;
        }
    }

    if ( nUpdateRegionRects == 0xFFFF &&
	 !rfbSendLastRectMarker(cl) ) {
	return FALSE;
    }

    if (!rfbSendUpdateBuf(cl))
        return FALSE;

    if(cursorWasDrawn != cl->screen->cursorIsDrawn) {
       if(cursorWasDrawn)
	  rfbDrawCursor(cl);
	  //fprintf(stderr,"rfbSpriteRestoreCursor(pScreen); not yet!!\n");
       else
	  rfbUndrawCursor(cl);
	  //fprintf(stderr,"rfbSpriteRemoveCursor(pScreen); not yet!!\n");
    }

    return TRUE;
}


/*
 * Send the copy region as a string of CopyRect encoded rectangles.
 * The only slightly tricky thing is that we should send the messages in
 * the correct order so that an earlier CopyRect will not corrupt the source
 * of a later one.
 */

Bool
rfbSendCopyRegion(cl, reg, dx, dy)
    rfbClientPtr cl;
    RegionPtr reg;
    int dx, dy;
{
    int nrects, nrectsInBand, x_inc, y_inc, thisRect, firstInNextBand;
    int x, y, w, h;
    rfbFramebufferUpdateRectHeader rect;
    rfbCopyRect cr;

    nrects = REGION_NUM_RECTS(reg);

    if (dx <= 0) {
	x_inc = 1;
    } else {
	x_inc = -1;
    }

    if (dy <= 0) {
	thisRect = 0;
	y_inc = 1;
    } else {
	thisRect = nrects - 1;
	y_inc = -1;
    }

    while (nrects > 0) {

	firstInNextBand = thisRect;
	nrectsInBand = 0;

	while ((nrects > 0) &&
	       (REGION_RECTS(reg)[firstInNextBand].y1
		== REGION_RECTS(reg)[thisRect].y1))
	{
	    firstInNextBand += y_inc;
	    nrects--;
	    nrectsInBand++;
	}

	if (x_inc != y_inc) {
	    thisRect = firstInNextBand - y_inc;
	}

	while (nrectsInBand > 0) {
	    if ((cl->ublen + sz_rfbFramebufferUpdateRectHeader
		 + sz_rfbCopyRect) > UPDATE_BUF_SIZE)
	    {
		if (!rfbSendUpdateBuf(cl))
		    return FALSE;
	    }

	    x = REGION_RECTS(reg)[thisRect].x1;
	    y = REGION_RECTS(reg)[thisRect].y1;
	    w = REGION_RECTS(reg)[thisRect].x2 - x;
	    h = REGION_RECTS(reg)[thisRect].y2 - y;

	    rect.r.x = Swap16IfLE(x);
	    rect.r.y = Swap16IfLE(y);
	    rect.r.w = Swap16IfLE(w);
	    rect.r.h = Swap16IfLE(h);
	    rect.encoding = Swap32IfLE(rfbEncodingCopyRect);

	    memcpy(&cl->updateBuf[cl->ublen], (char *)&rect,
		   sz_rfbFramebufferUpdateRectHeader);
	    cl->ublen += sz_rfbFramebufferUpdateRectHeader;

	    cr.srcX = Swap16IfLE(x - dx);
	    cr.srcY = Swap16IfLE(y - dy);

	    memcpy(&cl->updateBuf[cl->ublen], (char *)&cr, sz_rfbCopyRect);
	    cl->ublen += sz_rfbCopyRect;

	    cl->rfbRectanglesSent[rfbEncodingCopyRect]++;
	    cl->rfbBytesSent[rfbEncodingCopyRect]
		+= sz_rfbFramebufferUpdateRectHeader + sz_rfbCopyRect;

	    thisRect += x_inc;
	    nrectsInBand--;
	}

	thisRect = firstInNextBand;
    }

    return TRUE;
}

/*
 * Send a given rectangle in raw encoding (rfbEncodingRaw).
 */

Bool
rfbSendRectEncodingRaw(cl, x, y, w, h)
    rfbClientPtr cl;
    int x, y, w, h;
{
    rfbFramebufferUpdateRectHeader rect;
    int nlines;
    int bytesPerLine = w * (cl->format.bitsPerPixel / 8);
    char *fbptr = (cl->screen->frameBuffer + (cl->screen->paddedWidthInBytes * y)
                   + (x * (cl->screen->bitsPerPixel / 8)));

    if (cl->ublen + sz_rfbFramebufferUpdateRectHeader > UPDATE_BUF_SIZE) {
        if (!rfbSendUpdateBuf(cl))
            return FALSE;
    }

    rect.r.x = Swap16IfLE(x);
    rect.r.y = Swap16IfLE(y);
    rect.r.w = Swap16IfLE(w);
    rect.r.h = Swap16IfLE(h);
    rect.encoding = Swap32IfLE(rfbEncodingRaw);

    memcpy(&cl->updateBuf[cl->ublen], (char *)&rect,sz_rfbFramebufferUpdateRectHeader);
    cl->ublen += sz_rfbFramebufferUpdateRectHeader;

    cl->rfbRectanglesSent[rfbEncodingRaw]++;
    cl->rfbBytesSent[rfbEncodingRaw]
        += sz_rfbFramebufferUpdateRectHeader + bytesPerLine * h;

    nlines = (UPDATE_BUF_SIZE - cl->ublen) / bytesPerLine;

    while (TRUE) {
        if (nlines > h)
            nlines = h;

        (*cl->translateFn)(cl->translateLookupTable,
			   &(cl->screen->rfbServerFormat),
                           &cl->format, fbptr, &cl->updateBuf[cl->ublen],
                           cl->screen->paddedWidthInBytes, w, nlines);

        cl->ublen += nlines * bytesPerLine;
        h -= nlines;

        if (h == 0)     /* rect fitted in buffer, do next one */
            return TRUE;

        /* buffer full - flush partial rect and do another nlines */

        if (!rfbSendUpdateBuf(cl))
            return FALSE;

        fbptr += (cl->screen->paddedWidthInBytes * nlines);

        nlines = (UPDATE_BUF_SIZE - cl->ublen) / bytesPerLine;
        if (nlines == 0) {
            rfbLog("rfbSendRectEncodingRaw: send buffer too small for %d "
                   "bytes per line\n", bytesPerLine);
            rfbCloseClient(cl);
            return FALSE;
        }
    }
}



/*
 * Send an empty rectangle with encoding field set to value of
 * rfbEncodingLastRect to notify client that this is the last
 * rectangle in framebuffer update ("LastRect" extension of RFB
 * protocol).
 */

Bool
rfbSendLastRectMarker(cl)
    rfbClientPtr cl;
{
    rfbFramebufferUpdateRectHeader rect;

    if (cl->ublen + sz_rfbFramebufferUpdateRectHeader > UPDATE_BUF_SIZE) {
	if (!rfbSendUpdateBuf(cl))
	    return FALSE;
    }

    rect.encoding = Swap32IfLE(rfbEncodingLastRect);
    rect.r.x = 0;
    rect.r.y = 0;
    rect.r.w = 0;
    rect.r.h = 0;

    memcpy(&cl->updateBuf[cl->ublen], (char *)&rect,sz_rfbFramebufferUpdateRectHeader);
    cl->ublen += sz_rfbFramebufferUpdateRectHeader;

    cl->rfbLastRectMarkersSent++;
    cl->rfbLastRectBytesSent += sz_rfbFramebufferUpdateRectHeader;

    return TRUE;
}


/*
 * Send the contents of cl->updateBuf.  Returns 1 if successful, -1 if
 * not (errno should be set).
 */

Bool
rfbSendUpdateBuf(cl)
    rfbClientPtr cl;
{
    /*
    int i;
    for (i = 0; i < cl->ublen; i++) {
        fprintf(stderr,"%02x ",((unsigned char *)cl->updateBuf)[i]);
    }
    fprintf(stderr,"\n");
    */

    if (WriteExact(cl, cl->updateBuf, cl->ublen) < 0) {
        rfbLogPerror("rfbSendUpdateBuf: write");
        rfbCloseClient(cl);
        return FALSE;
    }

    cl->ublen = 0;
    return TRUE;
}

#ifdef NOT_YET
/*
 * rfbSendSetColourMapEntries sends a SetColourMapEntries message to the
 * client, using values from the currently installed colormap.
 */

Bool
rfbSendSetColourMapEntries(cl, firstColour, nColours)
    rfbClientPtr cl;
    int firstColour;
    int nColours;
{
    char buf[sz_rfbSetColourMapEntriesMsg + 256 * 3 * 2];
    rfbSetColourMapEntriesMsg *scme = (rfbSetColourMapEntriesMsg *)buf;
    CARD16 *rgb = (CARD16 *)(&buf[sz_rfbSetColourMapEntriesMsg]);
    EntryPtr pent;
    int i, len;

    scme->type = rfbSetColourMapEntries;

    scme->firstColour = Swap16IfLE(firstColour);
    scme->nColours = Swap16IfLE(nColours);

    len = sz_rfbSetColourMapEntriesMsg;

    pent = (EntryPtr)&rfbInstalledColormap->red[firstColour];
    for (i = 0; i < nColours; i++) {
	if (pent->fShared) {
	    rgb[i*3] = Swap16IfLE(pent->co.shco.red->color);
	    rgb[i*3+1] = Swap16IfLE(pent->co.shco.green->color);
	    rgb[i*3+2] = Swap16IfLE(pent->co.shco.blue->color);
	} else {
	    rgb[i*3] = Swap16IfLE(pent->co.local.red);
	    rgb[i*3+1] = Swap16IfLE(pent->co.local.green);
	    rgb[i*3+2] = Swap16IfLE(pent->co.local.blue);
	}
	pent++;
    }

    len += nColours * 3 * 2;

    if (WriteExact(cl->sock, buf, len) < 0) {
	rfbLogPerror("rfbSendSetColourMapEntries: write");
	rfbCloseClient(cl);
	return FALSE;
    }
    return TRUE;
}
#endif

/*
 * rfbSendBell sends a Bell message to all the clients.
 */

void
rfbSendBell(rfbScreenInfoPtr rfbScreen)
{
    rfbClientPtr cl, nextCl;
    rfbBellMsg b;

    for (cl = rfbScreen->rfbClientHead; cl; cl = nextCl) {
	nextCl = cl->next;
	b.type = rfbBell;
	if (WriteExact(cl, (char *)&b, sz_rfbBellMsg) < 0) {
	    rfbLogPerror("rfbSendBell: write");
	    rfbCloseClient(cl);
	}
    }
}


/*
 * rfbSendServerCutText sends a ServerCutText message to all the clients.
 */

void
rfbSendServerCutText(rfbScreenInfoPtr rfbScreen,char *str, int len)
{
    rfbClientPtr cl;
    rfbServerCutTextMsg sct;
    rfbClientIteratorPtr iterator;

    /* XXX bad-- writing with client list lock held */
    iterator = rfbGetClientIterator(rfbScreen);
    while ((cl = rfbClientIteratorNext(iterator)) != NULL) {
        sct.type = rfbServerCutText;
        sct.length = Swap32IfLE(len);
        if (WriteExact(cl, (char *)&sct,
                       sz_rfbServerCutTextMsg) < 0) {
            rfbLogPerror("rfbSendServerCutText: write");
            rfbCloseClient(cl);
            continue;
        }
        if (WriteExact(cl, str, len) < 0) {
            rfbLogPerror("rfbSendServerCutText: write");
            rfbCloseClient(cl);
        }
    }
    rfbReleaseClientIterator(iterator);
}

unsigned char ptrAcceleration = 50;

/*****************************************************************************
 *
 * UDP can be used for keyboard and pointer events when the underlying
 * network is highly reliable.  This is really here to support ORL's
 * videotile, whose TCP implementation doesn't like sending lots of small
 * packets (such as 100s of pen readings per second!).
 */

void
rfbNewUDPConnection(rfbScreen,sock)
    rfbScreenInfoPtr rfbScreen;
    int sock;
{
    if (write(sock, &ptrAcceleration, 1) < 0) {
	rfbLogPerror("rfbNewUDPConnection: write");
    }
}

/*
 * Because UDP is a message based service, we can't read the first byte and
 * then the rest of the packet separately like we do with TCP.  We will always
 * get a whole packet delivered in one go, so we ask read() for the maximum
 * number of bytes we can possibly get.
 */

#if 0
void
rfbProcessUDPInput(rfbClientPtr cl)
{
    int n;
    rfbClientToServerMsg msg;

    if ((n = read(cl->udpSock, (char *)&msg, sizeof(msg))) <= 0) {
	if (n < 0) {
	    rfbLogPerror("rfbProcessUDPInput: read");
	}
	rfbDisconnectUDPSock(cl);
	return;
    }

    switch (msg.type) {

    case rfbKeyEvent:
	if (n != sz_rfbKeyEventMsg) {
	    rfbLog("rfbProcessUDPInput: key event incorrect length\n");
	    rfbDisconnectUDPSock(cl);
	    return;
	}
	cl->screen->kbdAddEvent(msg.ke.down, (KeySym)Swap32IfLE(msg.ke.key), cl);
	break;

    case rfbPointerEvent:
	if (n != sz_rfbPointerEventMsg) {
	    rfbLog("rfbProcessUDPInput: ptr event incorrect length\n");
	    rfbDisconnectUDPSock(cl);
	    return;
	}
	cl->screen->ptrAddEvent(msg.pe.buttonMask,
		    Swap16IfLE(msg.pe.x), Swap16IfLE(msg.pe.y), cl);
	break;

    default:
	rfbLog("rfbProcessUDPInput: unknown message type %d\n",
	       msg.type);
	rfbDisconnectUDPSock(cl);
    }
}
#endif