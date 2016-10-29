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

#include "lib/bluetooth.h"
#include "lib/hci.h"
#include "lib/hci_lib.h"
#include "lib/rfcomm.h"

#include "midi.h"

#define MAX_CLIENT_SOCKET_CNT			10
#define EMPTY_PID						((pid_t)0)

static pid_t tChildPID_List[MAX_CLIENT_SOCKET_CNT];
static volatile sig_atomic_t __io_canceled = 0;

static void sig_hup(int sig)
{
	return;
}

static void sig_term(int sig)
{
	__io_canceled = 1;
}

int setClientOffDuty(pid_t pChildPID_List, int nPID_ListLen, pid_t tPID)
{
	while (nPID_ListLen > 0){
		nPID_ListLen--;
		if (tPID == pChildPID_List[nPID_ListLen]){
			pChildPID_List[nPID_ListLen] = EMPTY_PID;
			return 0;
		}
	}
	return (-1);
}

int setClientOnDuty(pid_t pChildPID_List, int nPID_ListLen, pid_t tPID)
{
	while (nPID_ListLen > 0){
		nPID_ListLen--;
		if (EMPTY_PID == pChildPID_List[nPID_ListLen]){
			pChildPID_List[nPID_ListLen] = tPID;
			return 0;
		}
	}
	return (-1);
}

int isAllClientProcessExit(pid_t pChildPID_List, int nPID_ListLen)
{
	while (nPID_ListLen > 0){
		nPID_ListLen--;
		if (EMPTY_PID != pChildPID_List[nPID_ListLen]){
			return 0;
		}
	}
	return 1;
}

void killAllClientProcess(pid_t pChildPID_List, int nPID_ListLen)
{
	int nLen = nPID_ListLen;
	while (nPID_ListLen > 0){
		nPID_ListLen--;
		if (EMPTY_PID != pChildPID_List[nPID_ListLen]){
			kill(pChildPID_List[nPID_ListLen], SIGINT);
		}
	}

	while (isAllClientProcessExit(pChildPID_List, nLen)){
		usleep(100000);
	}
}

static void sig_chld(int signo)
{
	pid_t   tPID;
	int     nStat;

	while((tPID = waitpid(-1, &nStat, WNOHANG)) > 0){
		setClientOffDuty(tChildPID_List, MAX_CLIENT_SOCKET_CNT, tPID);
	}
	return;
}

static void clientService(int nSporeSocket)
{
	struct sigaction tSignalAction;
	int nBytesRead;

	memset(&tSignalAction, 0, sizeof(tSignalAction));
	tSignalAction.sa_handler = sig_term;
	sigaction(SIGINT,  &tSignalAction, NULL);

	while(!__io_canceled){
		nBytesRead = recv(nSporeSocket , buf , sizeof(buf) , 0);
		if( bytes_read > 0 ) {
			printf( "received [%s]\n" , buf ) ;
		}
	}
	exit(0);
}

int main(int argc, char *argv[])
{
	struct sigaction tSignalAction;

	// bt related
	bdaddr_t tBD_Addr;
	int nServerSocket, nSporeSocket, tPID;
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

	memset(&tSignalAction, 0, sizeof(tSignalAction));
	tSignalAction.sa_handler = sig_chld;
	sigaction(SIGCHLD, &tSignalAction, NULL);

	tSignalAction.sa_handler = sig_term;
	sigaction(SIGINT,  &tSignalAction, NULL);

	nClientID = nInitSeq(&pSeq);
	if ((nClientID < 0) || (NULL == pSeq)){
		puts("Initialize sequencer failed.");
		if (pSeq != NULL){
			snd_seq_close(pSeq);
		}
		return 1;
	}

	while ((nOpt = getopt_long(argc, argv, sShortOptions, tLongOptions, NULL)) != -1) {
		switch (nOpt) {
		case 'h':
			listUsage(argv[0]);
			return 0;
		case 'V':
			listVersion();
			return 0;
		case 'l':
			nDoList = 1;
			break;
		case 'p':
			nPortCount = nParsePorts(optarg, &pPorts, pSeq);
			break;
		default:
			listUsage(argv[0]);
			return 1;
		}
	}

	if (1 == nDoList) {
		listPorts(pSeq);
	} else {
		if (nPortCount < 1) {
			printf("Please specify at least one port.");
			erroExitHandler(pSeq, pPorts, nMyPortID);
		}

		nMyPortID = pCreateSourcePort(pSeq);
		if (nMyPortID < 0){
			erroExitHandler(pSeq, pPorts, nMyPortID);
		}
		if (nConnectPorts(pSeq, nPortCount, pPorts) < 0){
			erroExitHandler(pSeq, pPorts, nMyPortID);
		}
		nPlayReadyMidi(pSeq, nMyPortID);
	}
	// midi ready

	bacpy(&tBD_Addr, BDADDR_ANY);

	tLocalAddr.rc_family = AF_BLUETOOTH;
	bacpy(&tLocalAddr.rc_bdaddr, &tBD_Addr);
	tLocalAddr.rc_channel = 1;	//(argc < 2) ? 1 : atoi(argv[1]);

	nServerSocket = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
	if (nServerSocket < 0) {
		perror("Can't open RFCOMM control socket");
		erroExitHandler(pSeq, pPorts, nMyPortID);
	}

	if (bind(nServerSocket, (struct sockaddr *)&tLocalAddr, sizeof(tLocalAddr)) < 0) {
		perror("Can't bind RFCOMM socket");
		close(nServerSocket);
		erroExitHandler(pSeq, pPorts, nMyPortID);
	}

	listen(nServerSocket, MAX_CLIENT_SOCKET_CNT);
	puts("Waiting for connection from client...");
	tAddrLen = sizeof(tRemoteAddr);
	while(!__io_canceled){
		nSporeSocket = accept(nServerSocket, (struct sockaddr *) &tRemoteAddr, &tAddrLen);
		ba2str(&(tRemoteAddr.rc_bdaddr), cDst);
		printf("Client %s connected.\n", cDst);
		tPID = fork();
		if (tPID < 0 ){
			close(nServerSocket);
			killAllClientProcess(tChildPID_List, MAX_CLIENT_SOCKET_CNT);
			erroExitHandler(pSeq, pPorts, nMyPortID);
		}else if (0 == tPID){
			// Child process
			clientService(nSporeSocket);
		}else{
			// Parent process
			setClientOnDuty(tChildPID_List, MAX_CLIENT_SOCKET_CNT, tPID);
		}
		usleep(100000);
	}
	killAllClientProcess(tChildPID_List, MAX_CLIENT_SOCKET_CNT);
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
	exit(0);
}
