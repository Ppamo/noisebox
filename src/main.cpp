#include <iostream>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

#include "RtAudio.h"
#include "RtMidi.h"

#define SAMPLERATE 22050
#define BUFFERSIZE 128
#define MIDIDEVICE "LPD8"

#include "Oscillator.h"
#include "Filter.h"

#include "Parameter.h"

#define MOD_PITCH 1
#define MOD_CUTOFF 2

using namespace std;

/////////////////////////////////
// HANDY UTILS

float map(float in, float inMin, float inMax, float outMin, float outMax) {
	return outMin + (outMax - outMin)*(in - inMin)/(inMax-inMin);
}

float logMap(float in, float inMin, float inMax, float outMin, float outMax) {
	float norm = (in - inMin)/(inMax-inMin);
	norm *= norm;
	return outMin + (outMax - outMin)*norm;
}

RtAudio audio;
RtMidiIn *midiin = 0;

SinLUT sinLut;

Oscillator lfo;
Oscillator osc;
Filter filter;

/////////////////////////////////
// PARAMETERS CONTROLLED BY MIDI

Parameter modAmount(0.5);
Parameter oscFreq(440);
Parameter lfoFreq(0.1);

Parameter filterFreq(3000);
Parameter filterRes(1);

int modType = MOD_PITCH;

float distGain = 1;
float distVolume = 1;

bool mute = true;

/////////////////////////////
// NICE DISTORTION
float distort(float inp) {
	float d = (1.0+distGain)*inp/(1.0+distGain*(inp>0?inp:-inp));
	if(d>1) return 1;
	else if(d<-1) return -1;
	else return d;
}

//////////////////////////////////////////
// the magic
int audioCallback( void *outputBuffer, void *inputBuffer, unsigned int length, double streamTime, RtAudioStreamStatus status, void *userData )
{
	float *buffer = (float *) outputBuffer;

	if ( status )
		std::cout << "Stream underflow detected!" << std::endl;
	float out = 0;
	for(int i = 0; i < length; i++) {

		float of = oscFreq.get();
		lfo.frequency = lfoFreq.get();

		if(modType==MOD_PITCH) {
			osc.frequency = of - modAmount.get()*map(lfo.getSample(), -1, 1, 0, of);
		} else {
			osc.frequency = of;
		}

		float filtFreq = filterFreq.get();

		if(modType==MOD_CUTOFF) {
			filtFreq = filtFreq - modAmount.get()*map(lfo.getSample(), -1, 1, 40, filtFreq);
		}

		float s = osc.getSample();

		out = filter.lores(s, filtFreq, filterRes.get());
		out = distort(out*distVolume);

		if(out!=out) { // NaN prevention
			out = s;
		}
		out *= 0.7f;

		if(mute) out = 0;
		buffer[i*2] = out;
		buffer[i*2+1] = out;
	}

	return 0;
}

//////////////////////////////////////////
// The main midi callback
void midiCallback( double deltatime, std::vector< unsigned char > *message, void *userData )
{

	if(message->at(0)==144) {
		// note on
		mute = false;
		int oscType = message->at(1) - 36;
		oscType %= 4;
		osc.setOscType(oscType, SAMPLERATE);
		return;
	}
	if(message->at(0)==128) {
		// note off
		if(message->at(1)<=39) {
			mute = true;
		}
		return;
	}
	if(message->at(0)==176) {
		int val = message->at(2);
		switch(message->at(1)) {
			case 1:
				oscFreq.set(logMap(val, 0, 127, 20, 8000));
				break; 
			case 2:
				lfoFreq.set(logMap(val, 0, 127, 0.01, 100));
				break;
			case 3:
				modAmount.set(map(val, 0, 127, 0, 1));
				break;
			case 4:
				if(val>63) {
					modType = MOD_PITCH;
				} else {
					modType = MOD_CUTOFF;
				}
				break;
			case 5:
				filterFreq.set(map(val, 0, 127, 50, 4000));
				break;
			case 6:
				filterRes.set(map(val, 0, 127, 1, 10));
				break;
			case 7:
				distGain = logMap(val, 0, 127, 0, 10);
				break;
			case 8:
				distVolume = logMap(val, 0, 127, 0, 10);
				break;
		}
		return;
	}
}

int startMidi();
int startAudio(int);

int main(int argc, char *argv[]) {
	printf("Noisebox!\n");
	int outputDevice = 0;
	if (argc > 1)
		outputDevice = atoi(argv[1]);

	osc.setOscType(kOSC_TYPE_SAW , SAMPLERATE);
	osc.frequency = 440;

	lfo.setOscType(kOSC_TYPE_SAW, SAMPLERATE);
	lfo.frequency = 0.1;

	if(startMidi()!=0) {
		return 1;
	}

	if(startAudio(outputDevice)!=0) {
		return 1;
	}

	// report status to parent process
	std::string slave = "slave";
	if (argc > 2) {
		if (!slave.compare(argv[2])) {
			kill(getppid(), SIGUSR1);
		}
	}

	cout << "\nPlaying ... press any to quit.\n";
	while ( cin.peek() == EOF ) {
		usleep(100*1000);
	}

	printf("Bye!\n");
}

//////////////////////////////////////////////////////////////////////////////////////////
// ALL BORING AUDIO AND MIDI SETUP STUFF BELOW HERE

int startAudio(int outputDevice) {

	RtAudio::StreamParameters parameters;
	parameters.deviceId = outputDevice;
	parameters.nChannels = 2;
	parameters.firstChannel = 0;
	unsigned int sampleRate = SAMPLERATE;
	unsigned int bufferFrames = BUFFERSIZE;
	double data[2];

	try {
		audio.openStream( &parameters, NULL, RTAUDIO_FLOAT32, sampleRate, &bufferFrames, &audioCallback, (void *)&data );
		audio.startStream();
	} catch ( RtError& e ) {
		e.printMessage();
		return 1;
	}

	return 0;
}

int startMidi() {
	try {
		// RtMidiIn constructor ... exception possible
		midiin = new RtMidiIn();

		// Check inputs.
		unsigned int nPorts = midiin->getPortCount();
		std::cout << "\nThere are " << nPorts << " MIDI input sources available.\n";
		if(nPorts==0) {
			cout << "Try running 'sudo modprobe snd_seq' if you're getting permission errors\n";
			return 1;
		}

		std::string deviceName (MIDIDEVICE);
		for ( unsigned i=0; i<nPorts; i++ ) {
			// open the first MIDIDEVICE
			std::string portName = midiin->getPortName(i);
			if (portName.compare(0, deviceName.length(), deviceName) == 0){
				std::cout << "Openning Port #" << i+1 << ": " << portName << '\n';
				midiin->openPort(i);
				midiin->setCallback(&midiCallback);
				midiin->ignoreTypes( true, true, true );
				break;
			}
		}
	} catch ( RtError &error ) {
		error.printMessage();
		return 1;
	}
	return 0;
}
