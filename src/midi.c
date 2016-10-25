      /* seqdemo.c by Matthias Nagorni */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <alsa/asoundlib.h>

#define ALSA_MIDI_STUDY_VERSION_STR		"01.01"

/* error handling for ALSA functions */
static int nCheckSnd(const char *operation, int err)
{
	if (err < 0){
		printf("Cannot %s - %s", operation, snd_strerror(err));
		return (-1);
	}
	return (0);
}

static int nInitSeq(snd_seq_t** pSeq)
{
	int err;
	int nClientID;

	/* open sequencer */
	err = snd_seq_open(pSeq, "default", SND_SEQ_OPEN_DUPLEX, 0);
	if (nCheckSnd("open sequencer", err) < 0){
		return (-1);
	}

	/* set our name (otherwise it's "Client-xxx") */
	err = snd_seq_set_client_name(*pSeq, "alsa study midi generator");
	if (nCheckSnd("set client name", err) < 0){
		return (-1);
	}

	/* find out who we actually are */
	nClientID = snd_seq_client_id(*pSeq);
	if (nCheckSnd("get client id", nClientID) < 0){
		return (-1);
	}

	return nClientID;
}

static void listVersion(void)
{
	puts("alsa study midi generator listVersion " ALSA_MIDI_STUDY_VERSION_STR);
}

/* parses one or more port addresses from the string */
static int nParsePorts(const char* arg, snd_seq_addr_t** pPorts, snd_seq_t *pSeq)
{
	char *pBuffer, *pStr, *pPortName;
	int err;
	int nPortCount = 0;

	/* make a copy of the string because we're going to modify it */
	pBuffer = strdup(arg);
	if (NULL == pBuffer){
		printf("nParsePorts failed because of strdup error.");
		return (-1);
	}

	for (pPortName = pStr = pBuffer; pStr; pPortName = pStr + 1) {
		/* Assume that ports are separated by commas.  We don't use
		 * spaces because those are valid in client names. */
		pStr = strchr(pPortName, ',');
		if (pStr)
			*pStr = '\0';

		++nPortCount;
		*pPorts = realloc(*pPorts, nPortCount * sizeof(snd_seq_addr_t));
		if (NULL == *pPorts){
			printf("nParsePorts failed because of realloc error.");
			free(pBuffer);
			return (-1);
		}

		err = snd_seq_parse_address(pSeq, &((*pPorts)[nPortCount - 1]), pPortName);
		if (err < 0){
			printf("Invalid port %s - %s", pPortName, snd_strerror(err));
			free(*pPorts);
			free(pBuffer);
			return (-1);
		}
	}

	free(pBuffer);
	return nPortCount;
}

static void listPorts(snd_seq_t *pSeq)
{
	snd_seq_client_info_t *pClientInfo;
	snd_seq_port_info_t *pPortInfo;
	int nClientID;

	snd_seq_client_info_alloca(&pClientInfo);
	snd_seq_port_info_alloca(&pPortInfo);

	puts(" Port    Client name                      Port name");

	snd_seq_client_info_set_client(pClientInfo, -1);
	while (snd_seq_query_next_client(pSeq, pClientInfo) >= 0) {
		nClientID = snd_seq_client_info_get_client(pClientInfo);

		snd_seq_port_info_set_client(pPortInfo, nClientID);
		snd_seq_port_info_set_port(pPortInfo, -1);
		while (snd_seq_query_next_port(pSeq, pPortInfo) >= 0) {
			/* port must understand MIDI messages */
			if (!(snd_seq_port_info_get_type(pPortInfo)
			      & SND_SEQ_PORT_TYPE_MIDI_GENERIC))
				continue;
			/* we need both WRITE and SUBS_WRITE */
			if ((snd_seq_port_info_get_capability(pPortInfo)
			     & (SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE))
			    != (SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE))
				continue;
			printf("%3d:%-3d  %-32.32s %s\n",
			       snd_seq_port_info_get_client(pPortInfo),
			       snd_seq_port_info_get_port(pPortInfo),
			       snd_seq_client_info_get_name(pClientInfo),
			       snd_seq_port_info_get_name(pPortInfo));
		}
	}
}

static void listUsage(const char *argv0)
{
	printf(
		"Usage: %s -p client:port[,...] [-d delay] midifile ...\n"
		"-h, --help                  this help\n"
		"-V, --version               print current version\n"
		"-l, --list                  list all possible output ports\n"
		"-p, --port=client:port,...  set port(s) to play to\n"
		"-d, --delay=seconds         delay after song ends\n",
		argv0);
}

static int pCreateSourcePort(snd_seq_t *pSeq)
{
	int nPortID;

	if ((nPortID = snd_seq_create_simple_port(pSeq, "alsa midi generator",
			SND_SEQ_PORT_CAP_READ|SND_SEQ_PORT_CAP_SUBS_READ,
			SND_SEQ_PORT_TYPE_APPLICATION)) < 0) {
			fprintf(stderr, "Error creating sequencer port.\n");
	}

	return nPortID;
}

static int nConnectPorts(snd_seq_t *pSeq, int nPortCount, snd_seq_addr_t *pPorts)
{
	int i, err;

	/*
	 * We send MIDI events with explicit destination addresses, so we don't
	 * need any connections to the playback ports.  But we connect to those
	 * anyway to force any underlying RawMIDI ports to remain open while
	 * we're playing - otherwise, ALSA would reset the port after every
	 * event.
	 */
	for (i = 0; i < nPortCount; ++i) {
		err = snd_seq_connect_to(pSeq, 0, pPorts[i].client, pPorts[i].port);
		if (err < 0){
			printf("Cannot connect to port %d:%d - %s",
					pPorts[i].client, pPorts[i].port, snd_strerror(err));
			return (-1);
		}
	}
	return 0;
}

static void erroExitHandler(snd_seq_t *pSeq, snd_seq_addr_t *pPorts, int nPortid)
{
	if (pPorts != NULL){
		free(pPorts);
	}
	if (nPortid >= 0){
		snd_seq_delete_port(pSeq, nPortid);
	}
	if (pSeq != NULL){
		snd_seq_close(pSeq);
	}
	exit(1);
}

static int nPlayReadyMidi(snd_seq_t *pSeq, int nMyPortID)
{
	snd_seq_event_t tEvent;
	int i;

	snd_seq_ev_clear(&tEvent);
    snd_seq_ev_set_source(&tEvent, nMyPortID);
    snd_seq_ev_set_subs(&tEvent);
    snd_seq_ev_set_direct(&tEvent);
	snd_seq_ev_set_fixed(&tEvent);

    for (i = 75; i < 78; i++){
		 // set event type, data, so on..
		tEvent.type = SND_SEQ_EVENT_PGMCHANGE;
		tEvent.data.control.channel = 0;
		tEvent.data.control.value = i+1;
		snd_seq_event_output(pSeq, &tEvent);
		snd_seq_drain_output(pSeq);

		tEvent.type = SND_SEQ_EVENT_NOTEON;
		tEvent.data.note.channel = 0;
		tEvent.data.note.note = 100;
		tEvent.data.note.velocity = 100;

		snd_seq_event_output(pSeq, &tEvent);
		snd_seq_drain_output(pSeq);
		usleep(300000);

		tEvent.type = SND_SEQ_EVENT_NOTEOFF;
		tEvent.data.note.channel = 0;
		tEvent.data.note.note = 100;
		tEvent.data.note.velocity = 0;
		snd_seq_event_output(pSeq, &tEvent);
		snd_seq_drain_output(pSeq);
		usleep(300000);
    }


	return 0;
}

//int main(int argc, char *argv[]) {
//	static const char sShortOptions[] = "hVlp:";
//	static const struct option tLongOptions[] = {
//		{"help", 0, NULL, 'h'},
//		{"listVersion", 0, NULL, 'V'},
//		{"list", 0, NULL, 'l'},
//		{"port", 1, NULL, 'p'},
//		{}
//	};
//	int nOpt;
//	int nDoList = 0;
//	snd_seq_t *pSeq = NULL;
//	int nClientID;
//	int nPortCount;
//	snd_seq_addr_t *pPorts = NULL;
//	int nMyPortID = 0;
//
////	sleep(10);
////
////	system("/etc/init.d/bluetooth stop");
////	system("hciconfig hci0 up");
////	system("/usr/sbin/bluetoothd --compat &");
////	system("mknod -m 666 /dev/rfcomm0 c 216 0");
////	system("rfcomm watch /dev/rfcomm0 3 /sbin/agetty rfcomm0 115200 linux &");
//
//
//	nClientID = nInitSeq(&pSeq);
//	if ((nClientID < 0) || (NULL == pSeq)){
//		puts("Initialize sequencer failed.");
//		if (pSeq != NULL){
//			snd_seq_close(pSeq);
//		}
//		return 1;
//	}
//
//	while ((nOpt = getopt_long(argc, argv, sShortOptions, tLongOptions, NULL)) != -1) {
//		switch (nOpt) {
//		case 'h':
//			listUsage(argv[0]);
//			return 0;
//		case 'V':
//			listVersion();
//			return 0;
//		case 'l':
//			nDoList = 1;
//			break;
//		case 'p':
//			nPortCount = nParsePorts(optarg, &pPorts, pSeq);
//			break;
//		default:
//			listUsage(argv[0]);
//			return 1;
//		}
//	}
//
//	if (1 == nDoList) {
//		listPorts(pSeq);
//	} else {
//		if (nPortCount < 1) {
//			printf("Please specify at least one port.");
//			erroExitHandler(pSeq, pPorts, nMyPortID);
//		}
//
//		nMyPortID = pCreateSourcePort(pSeq);
//		if (nMyPortID < 0){
//			erroExitHandler(pSeq, pPorts, nMyPortID);
//		}
//		if (nConnectPorts(pSeq, nPortCount, pPorts) < 0){
//			erroExitHandler(pSeq, pPorts, nMyPortID);
//		}
//		nPlayMidi(pSeq, nMyPortID);
//	}
//	if (pPorts != NULL){
//		free(pPorts);
//	}
//	if (nMyPortID >= 0){
//		snd_seq_delete_port(pSeq, nMyPortID);
//	}
//	if (pSeq != NULL){
//		snd_seq_close(pSeq);
//	}
//
//	return 0;
//}