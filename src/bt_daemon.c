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
#include <sys/wait.h>
#include <pthread.h>

#include "lib/bluetooth.h"
#include "lib/hci.h"
#include "lib/hci_lib.h"
#include "lib/rfcomm.h"

#include <alsa/asoundlib.h>

#include "midi.h"

#define MAX_CLIENT_SOCKET_CNT			10
#define NOTE_FRAME_LENGTH				5	// AE2C\n
#define NOTE_FRAME_END_SYMBOL			'\0'
#define EMPTY_PID						((pid_t)0)
#define EMPTY_TID						((pthread_t)0)
#define EMPTY_SOCKET					((int)0)

static int __io_canceled = 0;
static int nSocketList[MAX_CLIENT_SOCKET_CNT];
char cSndPort[128];

static void sig_term(int sig)
{
	__io_canceled = 1;
}

int getFreeClient(int* pSocketList, int nSocketListLen)
{
	while (nSocketListLen > 0){
		nSocketListLen--;
		if (EMPTY_SOCKET == pSocketList[nSocketListLen]){
			return nSocketListLen;
		}
	}
	return (-1);
}
void setSocketSlotFree(int* pSocketList, int nSocketListLen, int nSporeSocket)
{
	while (nSocketListLen > 0){
		nSocketListLen--;
		if (nSporeSocket == pSocketList[nSocketListLen]){
			pSocketList[nSocketListLen] = EMPTY_SOCKET;
		}
	}
}

//void waitAllThreadExit(pthread_t* pChildThreadList, int nThreadListLen)
//{
//	while (nThreadListLen > 0){
//		nThreadListLen--;
//		if (EMPTY_TID != pChildThreadList[nThreadListLen]){
//			pthread_join(pChildThreadList[nThreadListLen], NULL);
//		}
//	}
//}

//
//int setClientOffDuty(pid_t* pChildPID_List, int nPID_ListLen, pid_t tPID)
//{
//	while (nPID_ListLen > 0){
//		nPID_ListLen--;
//		if (tPID == pChildPID_List[nPID_ListLen]){
//			pChildPID_List[nPID_ListLen] = EMPTY_PID;
//			return 0;
//		}
//	}
//	return (-1);
//}
//
//int setClientOnDuty(pid_t* pChildPID_List, int nPID_ListLen, pid_t tPID)
//{
//	while (nPID_ListLen > 0){
//		nPID_ListLen--;
//		if (EMPTY_PID == pChildPID_List[nPID_ListLen]){
//			pChildPID_List[nPID_ListLen] = tPID;
//			return 0;
//		}
//	}
//	return (-1);
//}
//
//int isAllClientProcessExit(pid_t* pChildPID_List, int nPID_ListLen)
//{
//	while (nPID_ListLen > 0){
//		nPID_ListLen--;
//		if (EMPTY_PID != pChildPID_List[nPID_ListLen]){
//			return 0;
//		}
//	}
//	return 1;
//}

//void killAllClientProcess(pid_t* pChildPID_List, int nPID_ListLen)
//{
//	int nLen = nPID_ListLen;
//	while (nPID_ListLen > 0){
//		nPID_ListLen--;
//		if (EMPTY_PID != pChildPID_List[nPID_ListLen]){
//			kill(pChildPID_List[nPID_ListLen], SIGINT);
//		}
//	}
//
//	while (isAllClientProcessExit(pChildPID_List, nLen)){
//		usleep(100000);
//	}
//}
//
//static void sig_chld(int signo)
//{
//	pid_t   tPID;
//	int     nStat;
//
//	while((tPID = waitpid(-1, &nStat, WNOHANG)) > 0){
//		setClientOffDuty(tChildPID_List, MAX_CLIENT_SOCKET_CNT, tPID);
//	}
//	return;
//}

static int handleReceivedDataCrossTwoFrame(char* pBuff, int32_t nAlreadyReadLength, int32_t nBuffLen)
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
		puts("Buffer is full but no end symbol.");
		memset(pBuff, 0, nBuffLen);
		return 0;
	}else{
		return nAlreadyReadLength;
	}

}

static void* clientService(void* pSporeSocket)
{
//	struct sigaction tSignalAction;
	int nBytesRead;
	snd_seq_event_t tSndSeqEvent;
	snd_seq_t *pSeq = NULL;
	int nClientID;
	int nPortCount;
	int nMyPortID = 0;
	snd_seq_addr_t *pPorts = NULL;
//	snd_seq_ev_note_t tNoteEvent;
	char cBuff[NOTE_FRAME_LENGTH];
//	pid_t tMyPID;
	int nNeedToReadByte;
	int32_t nIndex;
	uint8_t unNoteData[2];
	int nSporeSocket = *((int*)pSporeSocket);

	int i;

//	memset(&tSignalAction, 0, sizeof(tSignalAction));
//	tSignalAction.sa_handler = sig_term;
//	sigaction(SIGINT,  &tSignalAction, NULL);
//	tMyPID = getpid();

	nClientID = nInitSeq(&pSeq);
	if ((nClientID < 0) || (NULL == pSeq)){
		puts("Initialize sequencer failed.");
		if (pSeq != NULL){
			snd_seq_close(pSeq);
		}
		return NULL;	//exit(1);
	}
	printf("Sequencer initialized.\n");

	nPortCount = nParsePorts(cSndPort, &pPorts, pSeq);
	if (nPortCount < 1) {
		printf("Please specify at least one port.\n");
		goto RELEASE_SOCKET;
	}
	printf("Sequencer ports parsed.\n");

	nMyPortID = pCreateSourcePort(pSeq);
	if (nMyPortID < 0){
		goto RELEASE_SOCKET;
	}
	printf("Sequencer source port created.\n");
	if (nConnectPorts(pSeq, nPortCount, pPorts) < 0){
		goto RELEASE_SOCKET;
	}
	printf("Sequencer target port connected.\n");

	snd_seq_ev_clear(&tSndSeqEvent);
    snd_seq_ev_set_source(&tSndSeqEvent, nMyPortID);
    snd_seq_ev_set_subs(&tSndSeqEvent);
    snd_seq_ev_set_direct(&tSndSeqEvent);
	snd_seq_ev_set_fixed(&tSndSeqEvent);

	nPlayConnectedMidi(pSeq, nMyPortID);

	tSndSeqEvent.type = SND_SEQ_EVENT_PGMCHANGE;
	tSndSeqEvent.data.control.channel = 0;
	tSndSeqEvent.data.control.value = 1;
	snd_seq_event_output(pSeq, &tSndSeqEvent);
	snd_seq_drain_output(pSeq);

	tSndSeqEvent.type = SND_SEQ_EVENT_NOTEON;
	tSndSeqEvent.data.note.channel = 0;
	nNeedToReadByte = NOTE_FRAME_LENGTH;
	memset(cBuff, 0, sizeof(cBuff));
	while(0 == __io_canceled){
		nBytesRead = recv(nSporeSocket, cBuff + (NOTE_FRAME_LENGTH - nNeedToReadByte), nNeedToReadByte, 0);	//&tNoteEvent, sizeof(tNoteEvent), 0);
		if(nBytesRead < 0){	//sizeof(snd_seq_event_t)) {
			printf("Received error with code %d.\n", errno);
			break;
		}else if (0 == nBytesRead) {
			puts("Received empty.");
		}else{
			// tricky, I tried my best to explain
			for (i = 0 ; i < nBytesRead; i++){
				printf("%02X, ", cBuff[NOTE_FRAME_LENGTH - nNeedToReadByte + i]);
				printf ("\n");
			}
			nNeedToReadByte -= nBytesRead;
			nNeedToReadByte = NOTE_FRAME_LENGTH - handleReceivedDataCrossTwoFrame(cBuff,
					NOTE_FRAME_LENGTH - nNeedToReadByte, NOTE_FRAME_LENGTH);

			if (0 == nNeedToReadByte){
				printf("BT received: %s\n", cBuff);
				for (nIndex = 0; nIndex < 2; nIndex++)
				    sscanf(cBuff + nIndex * 2, "%2hhx", unNoteData + nIndex);
	//			tSndSeqEvent.data.note.channel = 0;
				tSndSeqEvent.data.note.note = unNoteData[0];
			    tSndSeqEvent.data.note.velocity = unNoteData[1];

	//			tSndSeqEvent.data.note = tNoteEvent;
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

int main(int argc, char *argv[])
{
	struct sigaction tSignalAction;
	static pthread_t tThreadList[MAX_CLIENT_SOCKET_CNT];
	int nFreeSocketSlot;
	int nRSTL;
	pthread_attr_t tAttr;

	// bt related
	int nServerSocket, nSporeSocket;
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
	int nOpt;
	snd_seq_t *pSeq = NULL;
	int nClientID;
	int nPortCount;
	int nMyPortID = 0;
	int nDoList = 0;
	snd_seq_addr_t *pPorts = NULL;

	puts("MIDI daemon start.");

//	memset(&tSignalAction, 0, sizeof(tSignalAction));
//	tSignalAction.sa_handler = sig_chld;
//	sigaction(SIGCHLD, &tSignalAction, NULL);

	tSignalAction.sa_handler = sig_term;
	sigaction(SIGINT,  &tSignalAction, NULL);
	puts("Signal registration done.");

	nRSTL = pthread_attr_init(&tAttr);
	if (nRSTL < 0){
		puts("Initialize thread attribute failed.");
		exit(EXIT_FAILURE);
	}

	nRSTL = pthread_attr_setdetachstate(&tAttr, PTHREAD_CREATE_DETACHED);
	if (nRSTL < 0){
		puts("Set thread attribute failed.");
		exit(EXIT_FAILURE);
	}

	memset(nSocketList, EMPTY_SOCKET, sizeof(nSocketList));
	memset(tThreadList, EMPTY_TID, sizeof(tThreadList));

	nClientID = nInitSeq(&pSeq);
	if ((nClientID < 0) || (NULL == pSeq)){
		puts("Initialize sequencer failed.");
		if (pSeq != NULL){
			snd_seq_close(pSeq);
		}
		exit(1);
	}
	puts("Sequencer initialized.");

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
			puts("Sequencer ports parsed.");
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
			printf("Please specify at least one port.\n");
			erroExitHandler(pSeq, pPorts, nMyPortID);
		}

		nMyPortID = pCreateSourcePort(pSeq);
		if (nMyPortID < 0){
			erroExitHandler(pSeq, pPorts, nMyPortID);
		}
		puts("Source sequencer port created.");
		if (nConnectPorts(pSeq, nPortCount, pPorts) < 0){
			erroExitHandler(pSeq, pPorts, nMyPortID);
		}
		puts("Target sequencer port connected.");
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
	puts("Server BT port created.");

	if (bind(nServerSocket, (struct sockaddr *)&tLocalAddr, sizeof(tLocalAddr)) < 0) {
		perror("Can't bind RFCOMM socket");
		close(nServerSocket);
		erroExitHandler(pSeq, pPorts, nMyPortID);
	}
	puts("Server BT port binded to RFCOMM.");

	listen(nServerSocket, MAX_CLIENT_SOCKET_CNT);

	tAddrLen = sizeof(tRemoteAddr);
	while(0 == __io_canceled){
		puts("Waiting for connection from client...");
		nSporeSocket = accept(nServerSocket, (struct sockaddr *) &tRemoteAddr, &tAddrLen);
		ba2str(&(tRemoteAddr.rc_bdaddr), cDst);
		printf("Client %s connected.\n", cDst);
		nFreeSocketSlot = getFreeClient(nSocketList, MAX_CLIENT_SOCKET_CNT);
		if ((-1) == nFreeSocketSlot){
			close(nSporeSocket);
			perror("Threads are fully powered, don't push me too hard.");
			printf("Socket %d nSporeSocket closed by server.\n", nSporeSocket);
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

//		nRSTL = fork();
//		if (nRSTL < 0){
//			close(nServerSocket);
//			killAllClientProcess(tChildPID_List, MAX_CLIENT_SOCKET_CNT);
//			erroExitHandler(pSeq, pPorts, nMyPortID);
//		}else if (0 == nRSTL){
//			// Child process
//			clientService(nSporeSocket);
//		}else{
//			// Parent process
//			close(nSporeSocket);
//			setClientOnDuty(tChildPID_List, MAX_CLIENT_SOCKET_CNT, nRSTL);
//		}
		usleep(100000);
	}
//	killAllClientProcess(tChildPID_List, MAX_CLIENT_SOCKET_CNT);
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
