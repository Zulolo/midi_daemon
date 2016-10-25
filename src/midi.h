/*
 * midi.h
 *
 *  Created on: Oct 25, 2016
 *      Author: zulolo
 */

#ifndef MIDI_H_
#define MIDI_H_

int nCheckSnd(const char *operation, int err);

int nInitSeq(snd_seq_t** pSeq);

void listVersion(void);

int nParsePorts(const char* arg, snd_seq_addr_t** pPorts, snd_seq_t *pSeq);

void listPorts(snd_seq_t *pSeq);

void listUsage(const char *argv0);

int pCreateSourcePort(snd_seq_t *pSeq);

int nConnectPorts(snd_seq_t *pSeq, int nPortCount, snd_seq_addr_t *pPorts);

void erroExitHandler(snd_seq_t *pSeq, snd_seq_addr_t *pPorts, int nPortid);

int nPlayReadyMidi(snd_seq_t *pSeq, int nMyPortID);


#endif /* MIDI_H_ */
