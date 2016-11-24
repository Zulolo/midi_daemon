/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2002-2010  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <signal.h>
#include <termios.h>
#include <poll.h>
#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <pthread.h>

#include "lib/bluetooth.h"
#include "lib/hci.h"
#include "lib/hci_lib.h"
#include "lib/rfcomm.h"

#include <alsa/asoundlib.h>

#include "midi.h"

#define MAX_CLIENT_SOCKET_CNT			10
#define NOTE_FRAME_LENGTH				sizeof("0601AE2C")	// type+channel+note+velocity
#define NOTE_FRAME_DATA_NUMBER			((NOTE_FRAME_LENGTH - 1)/2)
#define NOTE_FRAME_END_SYMBOL			'\0'
#define EMPTY_PID						((pid_t)0)
#define EMPTY_TID						((pthread_t)0)
#define EMPTY_SOCKET					((int32_t)0)

typedef struct  {
  int32_t nVolume;
}midi_para_t;

pthread_mutex_t tMidiAttrMutex = PTHREAD_MUTEX_INITIALIZER;
static int32_t __io_canceled = 0;
static int32_t nSocketList[MAX_CLIENT_SOCKET_CNT];
static midi_para_t tMidiPara = {.nVolume = 50};
char cSndPort[128];

int32_t nProgramChange(snd_seq_t *pSeq, int32_t nMyPortID, int32_t nChannel,
		int32_t nProgram, int32_t nUseless1, int32_t nUseless2);
int32_t nSetVolume(snd_seq_t *pSeq, int32_t nMyPortID, int32_t nVolume,
		int32_t nUseless1, int32_t nUseless2, int32_t nUseless3);


// Something interesting:
const char* MIDI_EVENT_UNIX_FORMAT[] = {"channel:%d,instrument:%d,EMPTY_PARA_1:%d,EMPTY_PARA_2:%d\n",
		"volume:%d,EMPTY_PARA_1:%d,EMPTY_PARA_2:%d,EMPTY_PARA_3:%d\n"};
int32_t (*MIDI_EVENT_UNIX_FUNCTION[])() = {nProgramChange, nSetVolume};

static void sig_term(int32_t sig)
{
	__io_canceled = 1;
}

int32_t getFreeClient(int32_t* pSocketList, int32_t nSocketListLen)
{
	while (nSocketListLen > 0){
		nSocketListLen--;
		if (EMPTY_SOCKET == pSocketList[nSocketListLen]){
			return nSocketListLen;
		}
	}
	return (-1);
}
void setSocketSlotFree(int32_t* pSocketList, int32_t nSocketListLen, int32_t nSporeSocket)
{
	while (nSocketListLen > 0){
		nSocketListLen--;
		if (nSporeSocket == pSocketList[nSocketListLen]){
			pSocketList[nSocketListLen] = EMPTY_SOCKET;
		}
	}
}

int32_t nSetVolume(snd_seq_t *pSeq, int32_t nMyPortID, int32_t nVolume,
		int32_t nUseless1, int32_t nUseless2, int32_t nUseless3)
{
	printf("  Set volume to %d.\n", nVolume);
	pthread_mutex_lock(&tMidiAttrMutex);
	tMidiPara.nVolume = nVolume;
	pthread_mutex_unlock(&tMidiAttrMutex);
	return 0;
}
int32_t nProgramChange(snd_seq_t *pSeq, int32_t nMyPortID, int32_t nChannel,
		int32_t nProgram, int32_t nUseless1, int32_t nUseless2)
{
	snd_seq_event_t tSndSeqEvent;
	printf("  Set channel %d's program to %d.\n", nChannel, nProgram);
	snd_seq_ev_clear(&tSndSeqEvent);
    snd_seq_ev_set_source(&tSndSeqEvent, nMyPortID);
    snd_seq_ev_set_subs(&tSndSeqEvent);
    snd_seq_ev_set_direct(&tSndSeqEvent);
	snd_seq_ev_set_fixed(&tSndSeqEvent);

	tSndSeqEvent.type = SND_SEQ_EVENT_PGMCHANGE;
	tSndSeqEvent.data.control.channel = nChannel;
	tSndSeqEvent.data.control.value = nProgram;
	snd_seq_event_output(pSeq, &tSndSeqEvent);
	snd_seq_drain_output(pSeq);
	return 0;
}

int32_t executeCmdFromUnixSocket(const char* pBuff, int32_t nRc, snd_seq_t *pSeq, int32_t nMyPortID)
{
	int32_t nIndex;
	int32_t nPara1, nPara2, nPara3, nPara4;
	printf("  Received: %s", pBuff);
	for (nIndex = 0; nIndex < sizeof(MIDI_EVENT_UNIX_FORMAT); nIndex++){
		if (4 == sscanf(pBuff, MIDI_EVENT_UNIX_FORMAT[nIndex], &nPara1, &nPara2, &nPara3, &nPara4)){
			return MIDI_EVENT_UNIX_FUNCTION[nIndex](pSeq, nMyPortID, nPara1, nPara2, nPara3, nPara4);
		}
	}
	return (-1);
}

static int32_t handleReceivedDataCrossTwoFrame(char* pBuff, int32_t nAlreadyReadLength, int32_t nBuffLen)
{
	int32_t nEndSymbolPos;
	static char cTemp[32];
	// if this situation happened "/0/0/0/0/0", it is also OK
	// Every time one new char will be red into buff and the first /0 will be removed
	// and then one new char will be red into buff...
	for (nEndSymbolPos = 0; nEndSymbolPos < nAlreadyReadLength; nEndSymbolPos++){
		if (NOTE_FRAME_END_SYMBOL == pBuff[nEndSymbolPos]){
			if (nEndSymbolPos == (nBuffLen - 1)){
				return nBuffLen;
			}else{
				memset(cTemp, 0, sizeof(cTemp));
				memcpy(cTemp, pBuff + nEndSymbolPos + 1, nAlreadyReadLength - nEndSymbolPos - 1);
				memset(pBuff, 0, nBuffLen);
				memcpy(pBuff, cTemp, nAlreadyReadLength - nEndSymbolPos - 1);
				return nAlreadyReadLength - nEndSymbolPos - 1;
			}
		}
	}
	if (nAlreadyReadLength == nBuffLen){
		perror("Buffer is full but no end symbol.");
		memset(pBuff, 0, nBuffLen);
		return 0;
	}else{
		return nAlreadyReadLength;
	}

}

static void* clientService(void* pSporeSocket)
{
//	struct sigaction tSignalAction;
	int32_t nBytesRead;
	snd_seq_event_t tSndSeqEvent;
	snd_seq_t *pSeq = NULL;
	int32_t nClientID;
	int32_t nPortCount;
	int32_t nMyPortID = 0;
	snd_seq_addr_t *pPorts = NULL;
//	snd_seq_ev_note_t tNoteEvent;
	char cBuff[NOTE_FRAME_LENGTH];
	int32_t nNeedToReadByte;
	int32_t nIndex;
	uint8_t unNoteData[NOTE_FRAME_DATA_NUMBER];
	int32_t nSporeSocket = *((int32_t*)pSporeSocket);
//
//	int32_t i;

	nClientID = nInitSeq(&pSeq);
	if ((nClientID < 0) || (NULL == pSeq)){
		perror("Initialize sequencer failed.");
		if (pSeq != NULL){
			snd_seq_close(pSeq);
		}
		return NULL;	//exit(1);
	}
	printf("  Sequencer initialized by new BT connection.\n");

	nPortCount = nParsePorts(cSndPort, &pPorts, pSeq);
	if (nPortCount < 1) {
		printf("  Please specify at least one port.\n");
		goto RELEASE_SOCKET;
	}
	printf("  Sequencer ports parsed by new BT connection.\n");

	nMyPortID = pCreateSourcePort(pSeq);
	if (nMyPortID < 0){
		goto RELEASE_SOCKET;
	}
	printf("  Sequencer source port created by new BT connection.\n");
	if (nConnectPorts(pSeq, nPortCount, pPorts) < 0){
		goto RELEASE_SOCKET;
	}
	printf("  Sequencer target port connected by new BT connection.\n");

	snd_seq_ev_clear(&tSndSeqEvent);
    snd_seq_ev_set_source(&tSndSeqEvent, nMyPortID);
    snd_seq_ev_set_subs(&tSndSeqEvent);
    snd_seq_ev_set_direct(&tSndSeqEvent);
	snd_seq_ev_set_fixed(&tSndSeqEvent);

	nPlayConnectedMidi(pSeq, nMyPortID);

	tSndSeqEvent.type = SND_SEQ_EVENT_NOTEON;
	nNeedToReadByte = NOTE_FRAME_LENGTH;
	memset(cBuff, 0, sizeof(cBuff));
	while(0 == __io_canceled){
		nBytesRead = recv(nSporeSocket, cBuff + (NOTE_FRAME_LENGTH - nNeedToReadByte), nNeedToReadByte, 0);	//&tNoteEvent, sizeof(tNoteEvent), 0);
		if(nBytesRead < 0){	//sizeof(snd_seq_event_t)) {
			printf("  Received error with code %d.\n", errno);
			goto RELEASE_SOCKET;
		}else if (0 == nBytesRead) {
			perror("Received empty.");
		}else{
			// tricky, I tried my best to explain
//			for (i = 0 ; i < nBytesRead; i++){
//				printf("  %02X, ", cBuff[NOTE_FRAME_LENGTH - nNeedToReadByte + i]);
//				printf ("\n");
//			}
			nNeedToReadByte -= nBytesRead;
			nNeedToReadByte = NOTE_FRAME_LENGTH - handleReceivedDataCrossTwoFrame(cBuff,
					NOTE_FRAME_LENGTH - nNeedToReadByte, NOTE_FRAME_LENGTH);

			if (0 == nNeedToReadByte){
				printf("  BT received: %s\n", cBuff);
				for (nIndex = 0; nIndex < NOTE_FRAME_DATA_NUMBER; nIndex++)
				    sscanf(cBuff + nIndex * 2, "%2hhx", unNoteData + nIndex);
				printf("  Channel is: %u, Note is: %u, velocity is: %u.\n",
						unNoteData[1], unNoteData[2], unNoteData[3]);
				tSndSeqEvent.data.note.channel = unNoteData[1];
				if (0 == unNoteData[1]){
					tSndSeqEvent.data.note.velocity = 0;
				}else{
					tSndSeqEvent.data.note.note = unNoteData[2];
	//				pthread_mutex_lock(&tMidiAttrMutex);
					tSndSeqEvent.data.note.velocity = unNoteData[3];	//tMidiPara.nVolume;
	//				pthread_mutex_unlock(&tMidiAttrMutex);
				}

				snd_seq_event_output(pSeq, &tSndSeqEvent);
				snd_seq_drain_output(pSeq);
				nNeedToReadByte = NOTE_FRAME_LENGTH;
				memset(cBuff, 0, sizeof(cBuff));
			}
		}
	}

RELEASE_SOCKET:
	close(nSporeSocket);
	setSocketSlotFree(nSocketList, MAX_CLIENT_SOCKET_CNT, nSporeSocket);
	if (pPorts != NULL){
		free(pPorts);
	}
	if (nMyPortID >= 0){
		snd_seq_delete_port(pSeq, nMyPortID);
	}
	if (pSeq != NULL){
		snd_seq_close(pSeq);
	}
	return NULL;
//	exit(0);
}

#define UPDATE_MIDI_ATTR_SOCK_PATH 		"/tmp/.midi-unix"

void* updateMidiAttr(void* pWhatEver)
{
	snd_seq_t *pSeq = NULL;
	int32_t nClientID;
	int32_t nPortCount;
	int32_t nMyPortID = 0;
	snd_seq_addr_t *pPorts = NULL;
	int32_t nFdIndex, nRc, nOptVal = 1;
	int32_t nServerSocket, nMaxSocketFd, nClientSocket;
	int32_t nReadyFd;
	uint32_t nLen;
	uint8_t unCloseConn;
	char cBuff[128];
	struct sockaddr_un tServerSocketAddr, tClientSocketAddr;
	fd_set tMasterFdSet, tWorkingFdSet;

	nClientID = nInitSeq(&pSeq);
	if ((nClientID < 0) || (NULL == pSeq)){
		perror("Initialize sequencer failed in IPC server.");
		if (pSeq != NULL){
			snd_seq_close(pSeq);
		}
		return NULL;	//exit(1);
	}
	printf("  Sequencer initialized by IPC server.\n");

	nPortCount = nParsePorts(cSndPort, &pPorts, pSeq);
	if (nPortCount < 1) {
		printf("  Please specify at least one port.\n");
		goto RELEASE_IPC_SEQ;
	}
	printf("  Sequencer ports parsed by IPC server.\n");

	nMyPortID = pCreateSourcePort(pSeq);
	if (nMyPortID < 0){
		goto RELEASE_IPC_SEQ;
	}
	printf("  Sequencer source port created by IPC server.\n");
	if (nConnectPorts(pSeq, nPortCount, pPorts) < 0){
		goto RELEASE_IPC_SEQ;
	}
	printf("  Sequencer target port connected by IPC server.\n");


	/* ----==== All Sequencer initialize stuff ends here ====----*/

	nServerSocket = socket(AF_UNIX, SOCK_STREAM, 0);
	if (nServerSocket < 0){
		perror("Create Unix server socket failed.");
		goto RELEASE_IPC_SEQ;
	}

	/*************************************************************/
	/* Allow socket descriptor to be reuseable                   */
	/*************************************************************/
	nRc = setsockopt(nServerSocket, SOL_SOCKET, SO_REUSEADDR, (char *)&nOptVal, sizeof(nOptVal));
	if (nRc < 0){
		perror("Set server socket reusable failed");
		close(nServerSocket);
		goto RELEASE_IPC_SEQ;
	}

	/*************************************************************/
	/* Set server socket to be nonblocking. All of the sockets for */
	/* the incoming connections will also be nonblocking since  */
	/* they will inherit that state from the listening socket.   */
	/*************************************************************/
	nRc = ioctl(nServerSocket, FIONBIO, (char *)&nOptVal);
	if (nRc < 0){
		perror("Set server socket non-blocking failed");
		close(nServerSocket);
		goto RELEASE_IPC_SEQ;
	}

	memset(&tServerSocketAddr, 0, sizeof(tServerSocketAddr));
	tServerSocketAddr.sun_family = AF_UNIX;
	strcpy(tServerSocketAddr.sun_path, UPDATE_MIDI_ATTR_SOCK_PATH);
	unlink(tServerSocketAddr.sun_path);
	nLen = strlen(tServerSocketAddr.sun_path) + sizeof(tServerSocketAddr.sun_family);
	nRc = bind(nServerSocket, (struct sockaddr *)&tServerSocketAddr, nLen);
	if (nRc < 0){
		perror("Server socket bind failed");
		close(nServerSocket);
		goto RELEASE_IPC_SEQ;
	}
	chmod(UPDATE_MIDI_ATTR_SOCK_PATH, S_IRWXU|S_IRWXG);
	nRc = listen(nServerSocket, 8);
	if (nRc < 0){
		perror("Listen failed");
		close(nServerSocket);
		goto RELEASE_IPC_SEQ;
	}

	/*************************************************************/
	/* Initialize the master fd_set                              */
	/*************************************************************/
	FD_ZERO(&tMasterFdSet);
	nMaxSocketFd = nServerSocket;
	FD_SET(nServerSocket, &tMasterFdSet);


	/*************************************************************/
	/* Loop waiting for incoming connects or for incoming data   */
	/* on any of the connected sockets.                          */
	/*************************************************************/
	do{
		memcpy(&tWorkingFdSet, &tMasterFdSet, sizeof(tMasterFdSet));

		printf("  Waiting on select()...\n");
		nRc = select(nMaxSocketFd + 1, &tWorkingFdSet, NULL, NULL, NULL);
		if (nRc <= 0){
			perror("Select failed");
			break;
		}

		/**********************************************************/
		/* One or more descriptors are readable.  Need to         */
		/* determine which ones they are.                         */
		/**********************************************************/
		nReadyFd = nRc;	// nRc here is the total count of ready Fd
		for (nFdIndex = 0; nFdIndex <= nMaxSocketFd  &&  nReadyFd > 0; ++nFdIndex){
			if (FD_ISSET(nFdIndex, &tWorkingFdSet)){
				nReadyFd -= 1;

				/****************************************************/
				/* Check to see if this is the listening socket     */
				/****************************************************/
				if (nFdIndex == nServerSocket){
					printf("  Someone want to connect.\n");
					/*************************************************/
					/* Accept all incoming connections that are      */
					/* queued up on the listening socket before we   */
					/* loop back and call select again.              */
					/*************************************************/
					do{
						/**********************************************/
						/* Accept each incoming connection.  If       */
						/* accept fails with EWOULDBLOCK, then we     */
						/* have accepted all of them.  Any other      */
						/* failure on accept will cause us to end the */
						/* server.                                    */
						/**********************************************/
						nClientSocket = accept(nServerSocket, (struct sockaddr *)&tClientSocketAddr, &nLen);
						if (nClientSocket < 0){
							if (errno != EWOULDBLOCK){
								perror("Accept failed and it is not because all connection has been handled.");
								kill(getpid(), SIGINT);
							}
							break;
						}
						/**********************************************/
						/* Add the new incoming connection to the     */
						/* master read set                            */
						/**********************************************/
						printf("  New incoming connection - %d\n", nClientSocket);
						FD_SET(nClientSocket, &tMasterFdSet);
						if (nClientSocket > nMaxSocketFd)
							nMaxSocketFd = nClientSocket;

					/**********************************************/
					/* Loop back up and accept another incoming   */
					/* connection                                 */
					/**********************************************/
					} while (nClientSocket != -1);
				}else{
					/****************************************************/
					/* This is not the listening socket, therefore an   */
					/* existing connection must be readable             */
					/****************************************************/
					printf("  Client %d did something.\n", nFdIndex);
					unCloseConn = 0;
					/*************************************************/
					/* Receive all incoming data on this socket      */
					/* before we loop back and call select again.    */
					/*************************************************/
					do{
						/**********************************************/
						/* Receive data on this connection until the  */
						/* recv fails with EWOULDBLOCK.  If any other */
						/* failure occurs, we will close the          */
						/* connection.                                */
						/**********************************************/
						nRc = recv(nFdIndex, cBuff, sizeof(cBuff), 0);
						if (nRc < 0){
							if (errno != EWOULDBLOCK){
								perror("recv() failed");
								unCloseConn = 1;
							}
							break;
						}

						/**********************************************/
						/* Check to see if the connection has been    */
						/* closed by the client                       */
						/**********************************************/
						if (nRc == 0){
							printf("  Connection closed\n");
							unCloseConn = 1;
							break;
						}

						/**********************************************/
						/* Data was received                          */
						/**********************************************/
						cBuff[nRc] = '\0';
						executeCmdFromUnixSocket(cBuff, nRc, pSeq, nMyPortID);
					} while (1);

					/*************************************************/
					/* If the close_conn flag was turned on, we need */
					/* to clean up this active connection.  This     */
					/* clean up process includes removing the        */
					/* descriptor from the master set and            */
					/* determining the new maximum descriptor value  */
					/* based on the bits that are still turned on in */
					/* the master set.                               */
					/*************************************************/
					if (1 == unCloseConn){
						close(nFdIndex);
						FD_CLR(nFdIndex, &tMasterFdSet);
						if (nFdIndex == nMaxSocketFd){
							while (FD_ISSET(nMaxSocketFd, &tMasterFdSet) == 0)
								nMaxSocketFd -= 1;
						}
					}
				} /* End of existing connection is readable */
			} /* End of if (FD_ISSET(i, &working_set)) */
		} /* End of loop through selectable descriptors */
	} while (0 == __io_canceled);

	/*************************************************************/
	/* Clean up all of the sockets that are open                 */
	/* Including the server socket				                 */
	/*************************************************************/
	for (nFdIndex = 0; nFdIndex <= nMaxSocketFd; ++nFdIndex){
		if (FD_ISSET(nFdIndex, &tMasterFdSet))
		close(nFdIndex);
	}

	RELEASE_IPC_SEQ:
	if (pPorts != NULL){
		free(pPorts);
	}
	if (nMyPortID >= 0){
		snd_seq_delete_port(pSeq, nMyPortID);
	}
	if (pSeq != NULL){
		snd_seq_close(pSeq);
	}
	return NULL;
}

int32_t main(int32_t argc, char *argv[])
{
	struct sigaction tSignalAction;
	static pthread_t tThreadList[MAX_CLIENT_SOCKET_CNT];
	static pthread_t updateMidiAttrThread;
	int32_t nFreeSocketSlot;
	int32_t nRSTL;
	pthread_attr_t tAttr;

	// bt related
	int32_t nServerSocket, nSporeSocket;
	struct sockaddr_rc tLocalAddr, tRemoteAddr;
	socklen_t tAddrLen;
	char cDst[18];

	// midi related
	static const char sShortOptions[] = "hVlp:";
	static const struct option tLongOptions[] = {
		{"help", 0, NULL, 'h'},
		{"listVersion", 0, NULL, 'V'},
		{"list", 0, NULL, 'l'},
		{"port", 1, NULL, 'p'},
		{}
	};
	int32_t nOpt;
	snd_seq_t *pSeq = NULL;
	int32_t nClientID;
	int32_t nPortCount;
	int32_t nMyPortID = 0;
	int32_t nDoList = 0;
	snd_seq_addr_t *pPorts = NULL;

	printf("  MIDI daemon start.\n");

	memset(nSocketList, EMPTY_SOCKET, sizeof(nSocketList));
	memset(tThreadList, EMPTY_TID, sizeof(tThreadList));

	tSignalAction.sa_handler = sig_term;
	sigaction(SIGINT,  &tSignalAction, NULL);
	printf("  Signal registration done.\n");

	nRSTL = pthread_attr_init(&tAttr);
	if (nRSTL < 0){
		perror("Initialize thread attribute failed.");
		exit(EXIT_FAILURE);
	}

	nRSTL = pthread_attr_setdetachstate(&tAttr, PTHREAD_CREATE_DETACHED);
	if (nRSTL < 0){
		perror("Set thread attribute failed.");
		exit(EXIT_FAILURE);
	}

	nRSTL = pthread_create(&updateMidiAttrThread, &tAttr, updateMidiAttr, NULL);
	if(nRSTL)
	{
		perror("Start update midi attribute thread failed.");
		exit(EXIT_FAILURE);
	}

	nClientID = nInitSeq(&pSeq);
	if ((nClientID < 0) || (NULL == pSeq)){
		perror("Initialize sequencer failed.");
		if (pSeq != NULL){
			snd_seq_close(pSeq);
		}
		exit(1);
	}
	printf("  Sequencer initialized.\n");

	while ((nOpt = getopt_long(argc, argv, sShortOptions, tLongOptions, NULL)) != -1) {
		switch (nOpt) {
		case 'h':
			listUsage(argv[0]);
			exit(0);
		case 'V':
			listVersion();
			exit(0);
		case 'l':
			nDoList = 1;
			break;
		case 'p':
			strcpy(cSndPort, optarg);
			nPortCount = nParsePorts(cSndPort, &pPorts, pSeq);
			printf("Sequencer ports parsed. \n");
			break;
		default:
			listUsage(argv[0]);
			exit(0);
		}
	}

	if (1 == nDoList) {
		listPorts(pSeq);
		exit(0);
	} else {
		if (nPortCount < 1) {
			printf("  Please specify at least one port.\n");
			erroExitHandler(pSeq, pPorts, nMyPortID);
		}

		nMyPortID = pCreateSourcePort(pSeq);
		if (nMyPortID < 0){
			erroExitHandler(pSeq, pPorts, nMyPortID);
		}
		printf("Source sequencer port created.\n");
		if (nConnectPorts(pSeq, nPortCount, pPorts) < 0){
			erroExitHandler(pSeq, pPorts, nMyPortID);
		}
		printf("Target sequencer port connected.\n");
		nPlayReadyMidi(pSeq, nMyPortID);
	}
	// midi ready

	tLocalAddr.rc_family = AF_BLUETOOTH;
	bacpy(&tLocalAddr.rc_bdaddr, BDADDR_ANY);
	tLocalAddr.rc_channel = 1;	//(argc < 2) ? 1 : atoi(argv[1]);

	nServerSocket = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
	if (nServerSocket < 0) {
		perror("Can't open RFCOMM control socket");
		erroExitHandler(pSeq, pPorts, nMyPortID);
	}
	printf("  Server BT port created.\n");

	if (bind(nServerSocket, (struct sockaddr *)&tLocalAddr, sizeof(tLocalAddr)) < 0) {
		perror("Can't bind RFCOMM socket");
		close(nServerSocket);
		erroExitHandler(pSeq, pPorts, nMyPortID);
	}
	printf("  Server BT port binded to RFCOMM.\n");

	listen(nServerSocket, MAX_CLIENT_SOCKET_CNT);

	tAddrLen = sizeof(tRemoteAddr);
	while(0 == __io_canceled){
		printf("  Waiting for connection from client...\n");
		nSporeSocket = accept(nServerSocket, (struct sockaddr *) &tRemoteAddr, &tAddrLen);
		ba2str(&(tRemoteAddr.rc_bdaddr), cDst);
		printf("  Client %s connected.\n", cDst);
		nFreeSocketSlot = getFreeClient(nSocketList, MAX_CLIENT_SOCKET_CNT);
		if ((-1) == nFreeSocketSlot){
			close(nSporeSocket);
			perror("Threads are fully powered, don't push me too hard.");
			printf("  Socket %d nSporeSocket closed by server.\n", nSporeSocket);
			continue;
		}
		nSocketList[nFreeSocketSlot] = nSporeSocket;
		nRSTL = pthread_create(tThreadList + nFreeSocketSlot, &tAttr, clientService, &nSporeSocket);
		if(nRSTL)
		{
			close(nServerSocket);
			kill(getpid(), SIGINT);
			perror("Create thread failed.");
			erroExitHandler(pSeq, pPorts, nMyPortID);
		}

		usleep(100000);
	}

	close(nServerSocket);

	if (pPorts != NULL){
		free(pPorts);
	}
	if (nMyPortID >= 0){
		snd_seq_delete_port(pSeq, nMyPortID);
	}
	if (pSeq != NULL){
		snd_seq_close(pSeq);
	}
	exit(EXIT_SUCCESS);
}
