#include <list>
#include <iostream>
#include <algorithm>
using namespace std;

#define FTYPE double
#include "olcNoiseMaker.h"





struct sKnob
{
	FTYPE dMin;
	FTYPE dMax;
	atomic<FTYPE> dValue;
	FTYPE dStep;
};

struct sFilterLowPass
{
	FTYPE dAlpha;
	FTYPE dPreviousSample; 

	sFilterLowPass()
	{
		dAlpha = 0.0;
		dPreviousSample = 0.0;
		SetCutOffFrequency(120.0);
	}

	void SetCutOffFrequency(FTYPE dFrequency)
	{
		dAlpha = exp(-2.0 * PI * dFrequency / 44100.0);
	}

	FTYPE GetFiltered(FTYPE dTime, FTYPE dSample)
	{
		FTYPE dOutput =  ((1.0 - dAlpha) * dSample) - (-dAlpha * dPreviousSample);
		dPreviousSample = dOutput;
		return dOutput;
	}
};


namespace synth
{
	//////////////////////////////////////////////////////////////////////////////
	// Utilities

	// Converts frequency (Hz) to angular velocity
	FTYPE w(const FTYPE dHertz)
	{
		return dHertz * 2.0 * PI;
	}

	// A basic note
	struct note
	{
		int id;		// Position in scale
		FTYPE on;	// Time note was activated
		FTYPE off;	// Time note was deactivated
		bool active;
		int channel;

		note()
		{
			id = 0;
			on = 0.0;
			off = 0.0;
			active = false;
			channel = 0;
		}

		//bool operator==(const note& n1, const note& n2) { return n1.id == n2.id; }
	};

	//////////////////////////////////////////////////////////////////////////////
	// Multi-Function Oscillator
	const int OSC_SINE = 0;
	const int OSC_SQUARE = 1;
	const int OSC_TRIANGLE = 2;
	const int OSC_SAW_ANA = 3;
	const int OSC_SAW_DIG = 4;
	const int OSC_NOISE = 5;

	FTYPE osc(const FTYPE dTime, const FTYPE dHertz, const int nType = OSC_SINE,
		const FTYPE dLFOHertz = 0.0, const FTYPE dLFOAmplitude = 0.0, FTYPE dCustom = 50.0)
	{

		FTYPE dFreq = w(dHertz) * dTime + dLFOAmplitude * dHertz * (sin(w(dLFOHertz) * dTime));// osc(dTime, dLFOHertz, OSC_SINE);

		switch (nType)
		{
		case OSC_SINE: // Sine wave bewteen -1 and +1
			return sin(dFreq);

		case OSC_SQUARE: // Square wave between -1 and +1
			return sin(dFreq) > 0 ? 1.0 : -1.0;

		case OSC_TRIANGLE: // Triangle wave between -1 and +1
			return asin(sin(dFreq)) * (2.0 / PI);

		case OSC_SAW_ANA: // Saw wave (analogue / warm / slow)
		{
			FTYPE dOutput = 0.0;
			for (FTYPE n = 1.0; n < dCustom; n++)
				dOutput += (sin(n*dFreq)) / n;
			return dOutput * (2.0 / PI);
		}

		case OSC_SAW_DIG:
			return (2.0 / PI) * (dHertz * PI * fmod(dTime, 1.0 / dHertz) - (PI / 2.0));

		case OSC_NOISE:
			return 2.0 * ((FTYPE)rand() / (FTYPE)RAND_MAX) - 1.0;

		default:
			return 0.0;
		}
	}

	//////////////////////////////////////////////////////////////////////////////
	// Scale to Frequency conversion

	const int SCALE_DEFAULT = 0;

	FTYPE scale(const int nNoteID, const int nScaleID = SCALE_DEFAULT)
	{
		switch (nScaleID)
		{
		case SCALE_DEFAULT: default:
			return 8 * pow(1.0594630943592952645618252949463, nNoteID);
		}		
	}


	//////////////////////////////////////////////////////////////////////////////
	// Envelopes

	struct envelope
	{
		virtual FTYPE amplitude(const FTYPE dTime, const FTYPE dTimeOn, const FTYPE dTimeOff) = 0;
	};

	struct envelope_adsr : public envelope
	{
		FTYPE dAttackTime;
		FTYPE dDecayTime;
		FTYPE dSustainAmplitude;
		FTYPE dReleaseTime;
		FTYPE dStartAmplitude;

		envelope_adsr()
		{
			dAttackTime = 0.1;
			dDecayTime = 0.1;
			dSustainAmplitude = 1.0;
			dReleaseTime = 0.2;
			dStartAmplitude = 1.0;
		}

		virtual FTYPE amplitude(const FTYPE dTime, const FTYPE dTimeOn, const FTYPE dTimeOff)
		{
			FTYPE dAmplitude = 0.0;
			FTYPE dReleaseAmplitude = 0.0;

			if (dTimeOn > dTimeOff) // Note is on
			{
				FTYPE dLifeTime = dTime - dTimeOn;

				if (dLifeTime <= dAttackTime)
					dAmplitude = (dLifeTime / dAttackTime) * dStartAmplitude;

				if (dLifeTime > dAttackTime && dLifeTime <= (dAttackTime + dDecayTime))
					dAmplitude = ((dLifeTime - dAttackTime) / dDecayTime) * (dSustainAmplitude - dStartAmplitude) + dStartAmplitude;

				if (dLifeTime > (dAttackTime + dDecayTime))
					dAmplitude = dSustainAmplitude;
			}
			else // Note is off
			{
				FTYPE dLifeTime = dTimeOff - dTimeOn;

				if (dLifeTime <= dAttackTime)
					dReleaseAmplitude = (dLifeTime / dAttackTime) * dStartAmplitude;

				if (dLifeTime > dAttackTime && dLifeTime <= (dAttackTime + dDecayTime))
					dReleaseAmplitude = ((dLifeTime - dAttackTime) / dDecayTime) * (dSustainAmplitude - dStartAmplitude) + dStartAmplitude;

				if (dLifeTime > (dAttackTime + dDecayTime))
					dReleaseAmplitude = dSustainAmplitude;

				dAmplitude = ((dTime - dTimeOff) / dReleaseTime) * (0.0 - dReleaseAmplitude) + dReleaseAmplitude;
			}

			// Amplitude should not be negative
			if (dAmplitude <= 0.000)
				dAmplitude = 0.0;

			return dAmplitude;
		}
	};

	FTYPE env(const FTYPE dTime, envelope &env, const FTYPE dTimeOn, const FTYPE dTimeOff)
	{
		return env.amplitude(dTime, dTimeOn, dTimeOff);
	}


	struct instrument_base
	{
		FTYPE dVolume;
		synth::envelope_adsr env;
		virtual FTYPE sound(const FTYPE dTime, synth::note n, bool &bNoteFinished) = 0;
	};

	struct instrument_bell : public instrument_base
	{
		instrument_bell()
		{
			env.dAttackTime = 0.01;
			env.dDecayTime = 1.0;
			env.dSustainAmplitude = 0.0;
			env.dReleaseTime = 1.0;

			dVolume = 1.0;
		}

		virtual FTYPE sound(const FTYPE dTime, synth::note n, bool &bNoteFinished)
		{
			FTYPE dAmplitude = synth::env(dTime, env, n.on, n.off);
			if (dAmplitude <= 0.0) bNoteFinished = true;

			FTYPE dSound =
				+ 1.00 * synth::osc(dTime - n.on, synth::scale(n.id + 12), synth::OSC_SINE, 5.0, 0.001)
				+ 0.50 * synth::osc(dTime - n.on, synth::scale(n.id + 24))
				+ 0.25 * synth::osc(dTime - n.on, synth::scale(n.id + 36));

			return dAmplitude * dSound * dVolume;
		}

	};

	struct instrument_bell8 : public instrument_base
	{
		instrument_bell8()
		{
			env.dAttackTime = 0.01;
			env.dDecayTime = 0.5;
			env.dSustainAmplitude = 0.8;
			env.dReleaseTime = 1.0;

			dVolume = 1.0;
		}

		virtual FTYPE sound(const FTYPE dTime, synth::note n, bool &bNoteFinished)
		{
			FTYPE dAmplitude = synth::env(dTime, env, n.on, n.off);
			if (dAmplitude <= 0.0) bNoteFinished = true;

			FTYPE dSound =
				+1.00 * synth::osc(dTime - n.on, synth::scale(n.id), synth::OSC_SQUARE, 5.0, 0.001)
				+ 0.50 * synth::osc(dTime - n.on, synth::scale(n.id + 12))
				+ 0.25 * synth::osc(dTime - n.on, synth::scale(n.id + 24));

			return dAmplitude * dSound * dVolume;
		}

	};

	struct instrument_harmonica : public instrument_base
	{
		instrument_harmonica()
		{
			env.dAttackTime = 0.05;
			env.dDecayTime = 1.0;
			env.dSustainAmplitude = 0.95;
			env.dReleaseTime = 0.1;

			dVolume = 1.0;
		}

		virtual FTYPE sound(const FTYPE dTime, synth::note n, bool &bNoteFinished)
		{
			FTYPE dAmplitude = synth::env(dTime, env, n.on, n.off);
			if (dAmplitude <= 0.0) bNoteFinished = true;

			FTYPE dSound =
				//+ 1.0  * synth::osc(n.on - dTime, synth::scale(n.id-12), synth::OSC_SAW_ANA, 5.0, 0.001, 100)
				+ 1.00 * synth::osc(dTime - n.on, synth::scale(n.id), synth::OSC_SQUARE, 5.0, 0.001)
				+ 0.50 * synth::osc(dTime - n.on, synth::scale(n.id + 12), synth::OSC_SQUARE)
				+ 0.05  * synth::osc(dTime - n.on, synth::scale(n.id + 24), synth::OSC_NOISE);

			return dAmplitude * dSound * dVolume;
		}

	};
	
}

vector<synth::note> vecNotes;
mutex muxNotes;
synth::instrument_bell instBell;
synth::instrument_harmonica instHarm;



typedef bool(*lambda)(synth::note const& item);
template<class T>
void safe_remove(T &v, lambda f)
{
	auto n = v.begin();
	while (n != v.end())
		if (!f(*n))
			n = v.erase(n);
		else
			++n;
}

#define SWAP32(n) (((n>>24)&0xff) | ((n << 8) & 0xff0000) | ((n >> 8) & 0xff00) | ((n << 24) & 0xff000000))
#define SWAP16(n) ((n >> 8) | (n << 8))
struct midifile
{

	struct midievent
	{
		int nTickDelta;
		int nChannel;
		int nNote;
		int nVelocity;
		bool bSound;
		double dRealTime;
	};

	struct midichannel
	{
		list<midievent> listEvents;
		double dLastEventTime;
	};


	list<midichannel> listChannels;

	midifile(wstring sFilename)
	{
		ifstream fs;
		fs.open(sFilename, fstream::in | ios::binary);
		if (!fs.is_open())
			return;

		unsigned int nFileID;
		unsigned int nHeaderLength;
		unsigned short nFormat;
		unsigned short nTrackChunks;
		unsigned short nDivision;
		

		fs.read((char*)&nFileID, sizeof(unsigned int)); 
		nFileID = SWAP32(nFileID);
		fs.read((char*)&nHeaderLength, sizeof(unsigned int));
		nHeaderLength = SWAP32(nHeaderLength);
		fs.read((char*)&nFormat, sizeof(unsigned short));
		nFormat = SWAP16(nFormat);
		fs.read((char*)&nTrackChunks, sizeof(unsigned short));
		nTrackChunks = SWAP16(nTrackChunks);
		fs.read((char*)&nDivision, sizeof(unsigned short));
		nDivision = SWAP16(nDivision);
		
		

		for (int chunk = 0; chunk < nTrackChunks; chunk++)
		{
			unsigned int nTrackID, nTrackLength;
			fs.read((char*)&nTrackID, sizeof(unsigned int));
			nTrackID = SWAP32(nTrackID);
			fs.read((char*)&nTrackLength, sizeof(unsigned int));
			nTrackLength = SWAP32(nTrackLength);

			int length;
			bool bTrack = true;

			double dRealTime = 0.0;
			midichannel channel;
			channel.dLastEventTime = 0.0;

			auto readtext = [&]()
			{
				string s;				
				for (int i = 0; i < length; i++)
					s += (char)fs.get();
				return s;
			};

			while (!fs.eof() && bTrack)
			{
				unsigned int nEventDelta = read_var(fs);
				unsigned char nEventStatus = fs.get();

				dRealTime += (double)nEventDelta;

				if (nEventStatus == 0xF0)
				{
					length = read_var(fs);
					wcout << "SysEx1: " << readtext().c_str() << endl;
				}

				if (nEventStatus == 0xF7)
				{
					length = read_var(fs);
					wcout << "SysEx2: " << readtext().c_str() << endl;
				}

				if (nEventStatus == 0xFF)
				{
					int id = fs.get();
					switch (id)
					{
					case 0x01:
						length = read_var(fs);
						wcout << "Text: " << readtext().c_str() << endl;
						break;
					case 0x02:
						length = read_var(fs);
						wcout << "Copyright: " << readtext().c_str() << endl;
						break;
					case 0x03:
						length = read_var(fs);
						wcout << "Track Name: " << readtext().c_str() << endl;
						break;
					case 0x04: 
						length = read_var(fs);
						wcout << "Instrument: " << readtext().c_str() << endl;
						break;
					case 0x05: 
						length = read_var(fs);
						wcout << "Lyric: " << readtext().c_str() << endl;
						break;
					case 0x06:
						length = read_var(fs);
						wcout << "Marker: " << readtext().c_str() << endl;
						break;
					case 0x07:
						length = read_var(fs);
						wcout << "Cue: " << readtext().c_str() << endl;
						break;
					case 0x20:
						wcout << "Prefix: " << fs.get() << " Channel: " << fs.get() << endl;
						break;
					case 0x2F:
						fs.get();
						wcout << "[END OF TRACK]" << endl;
						bTrack = false;
						break;
					case 0x51:
						fs.get(); fs.get(); fs.get(); fs.get();
						wcout << "Set Tempo" << endl;
						break;
					case 0x54:
						fs.get();
						wcout << "SMPTE Offset: H:" << fs.get() << " M:" << fs.get() << " S:" << fs.get() << " FR:" << fs.get() << " FF:" << fs.get() << endl;
						break;
					case 0x58:
						fs.get();
						wcout << "Time Signature: " << fs.get() << "/" << ( fs.get()) << " ClocksPerTick: " << fs.get() << " 32per24Clocks: " << fs.get() << endl;
						break;
					case 0x59:
						fs.get();
						wcout << "Key Signature: " << fs.get() << " Minor Key: " << fs.get() << endl;
						break;
					case 0x40:
						fs.get();
						wcout << "Odd Code" << endl;
						break;

					default:
						wcout << "Unknown Event: " << id << endl;
					}
				}
				
				if (nEventStatus >= 0x80 && nEventStatus < 0x90)
				{
					// MIDI Note Off
					unsigned char nNoteID = fs.get();
					unsigned char nNoteVel = fs.get();
					wcout << "[NOTE OFF] Channel: " << nEventStatus-0x80 << " Delta: " << nEventDelta << " Note: " << nNoteID << " Vel: " << nNoteVel << endl;

					if (nNoteID == 72 && nNoteVel == 64 && nEventDelta == 14)
					{
						int j = 0;
					}

					midievent e;
					e.nTickDelta = nEventDelta;
					e.nNote = nNoteID;
					e.nVelocity = nNoteVel;
					e.bSound = false;
					e.dRealTime = dRealTime;
					channel.listEvents.push_back(e);
				}
				
				if (nEventStatus >= 0x90 && nEventStatus < 0xA0)
				{
					// MIDI Note ON
					unsigned char nNoteID = fs.get();
					unsigned char nNoteVel = fs.get();
					wcout << "[NOTE ON ] Channel: " << nEventStatus - 0x90 << " Delta: " << nEventDelta << " Note: " << nNoteID << " Vel: " << nNoteVel << endl;
					
					midievent e;
					e.nTickDelta = nEventDelta;
					e.nNote = nNoteID;
					e.nVelocity = nNoteVel;
					e.bSound = true;
					e.dRealTime = dRealTime;
					channel.listEvents.push_back(e);
				}

				if (nEventStatus >= 0xA0 && nEventStatus < 0xB0)
				{
					// MIDI Polyphonic Aftertouch
					unsigned char nNoteID = fs.get();
					unsigned char nNoteVel = fs.get();
					wcout << "[POLYPHON] Channel: " << nEventStatus - 0xA0 << " Delta: " << nEventDelta << " Note: " << nNoteID << " Pressure: " << nNoteVel << endl;
				}

				if (nEventStatus >= 0xB0 && nEventStatus < 0xC0)
				{
					// MIDI Control
					unsigned char nCommand = fs.get();
					unsigned char nParam = fs.get();
					wcout << "[CONTROL ] Channel: " << nCommand - 0xB0 << " Delta: " << nEventDelta << " Command: " << nCommand << " Param: " << nParam << endl;
				}

				if (nEventStatus >= 0xC0 && nEventStatus < 0xD0)
				{
					// MIDI Control
					unsigned char nCommand = fs.get();
					//unsigned char nParam = fs.get();
					//wcout << "[PROGRAM ] Channel: " << nCommand - 0xC0 << " Delta: " << nEventDelta << " Command: " << nCommand << " Param: " << nParam << endl;
				}

				if (nEventStatus >= 0xD0 && nEventStatus < 0xE0)
				{
					// MIDI Control
					unsigned char nCommand = fs.get();
					//unsigned char nParam = fs.get();
					//wcout << "[AFTER P ] Channel: " << nCommand - 0xD0 << " Delta: " << nEventDelta << " Command: " << nCommand << " Param: " << nParam << endl;
				}

				if (nEventStatus >= 0xE0 && nEventStatus < 0xF0)
				{
					// MIDI Control
					unsigned char nCommand = fs.get();
					unsigned char nParam = fs.get();
					wcout << "[PITCH WH] Channel: " << nCommand - 0xE0 << " Delta: " << nEventDelta << " Command: " << nCommand << " Param: " << nParam << endl;
				}



			}

			listChannels.push_back(channel);
		}

		fs.close();

	}

	unsigned int read_var(ifstream &fs)
	{
		unsigned int value = 0;
		unsigned char c = 0;

		
		value = fs.get();

		if (value == 0x92)
		{
			int i = 0;
		};

		if (value & 0x80)
		{
			value &= 0x7F;
			do
			{
				c = fs.get();
				value = (value << 7) + (c & 0x7F);
			}
			while (c & 0x80);
		}
		return value;
	}
};


// Function used by olcNoiseMaker to generate sound waves
// Returns amplitude (-1.0 to +1.0) as a function of time
FTYPE MakeNoise(int nChannel, FTYPE dTime)
{	
	unique_lock<mutex> lm(muxNotes);
	FTYPE dMixedOutput = 0.0;

	for (auto &n : vecNotes)
	{
		bool bNoteFinished = false;
		FTYPE dSound = 0;
		if(n.channel == 2)
			dSound = instBell.sound(dTime, n, bNoteFinished);
		if (n.channel == 1)
			dSound = instHarm.sound(dTime, n, bNoteFinished) * 0.5;
		dMixedOutput += dSound;

		if (bNoteFinished && n.off > n.on)
			n.active = false;
	}

	// Woah! Modern C++ Overload!!!
	safe_remove<vector<synth::note>>(vecNotes, [](synth::note const& item) { return item.active; });


	return dMixedOutput * 0.2;
}

int main()
{

	midifile mfile(L"test1.mid");

	// Shameless self-promotion
	wcout << "www.OneLoneCoder.com - Synthesizer Part 2" << endl << "Multiple Oscillators with Single Amplitude Envelope, No Polyphony" << endl << endl;

	// Get all sound hardware
	vector<wstring> devices = olcNoiseMaker<short>::Enumerate();

	// Display findings
	for (auto d : devices) wcout << "Found Output Device: " << d << endl;
	wcout << "Using Device: " << devices[0] << endl;

	// Display a keyboard
	wcout << endl <<
		"|   |   |   |   |   | |   |   |   |   | |   | |   |   |   |" << endl <<
		"|   | S |   |   | F | | G |   |   | J | | K | | L |   |   |" << endl <<
		"|   |___|   |   |___| |___|   |   |___| |___| |___|   |   |__" << endl <<
		"|     |     |     |     |     |     |     |     |     |     |" << endl <<
		"|  Z  |  X  |  C  |  V  |  B  |  N  |  M  |  ,  |  .  |  /  |" << endl <<
		"|_____|_____|_____|_____|_____|_____|_____|_____|_____|_____|" << endl << endl;

	wcout << "Knobs: (Hold key and use '+' and '-' to change)" << endl;
	wcout << "F1 - Low Pass Filter, F2 = High Pass Filter" << endl << endl;

	// Create sound machine!!
	olcNoiseMaker<short> sound(devices[0], 44100, 2, 24, 1024);

	// Link noise function with sound machine
	sound.SetUserFunction(MakeNoise);

	//// Sit in loop, capturing keyboard state changes and modify
	//// synthesizer output accordingly
	//int nCurrentKey = -1;	
	//bool bKeyPressed = false;

	//knobFilterLP.dMax = 10000.0;
	//knobFilterLP.dMin = 0.0;
	//knobFilterLP.dStep = 1;
	//knobFilterLP.dValue = 10000.0;

	//knobFilterHP.dMax = 10000.0;
	//knobFilterHP.dMin = 0.0;
	//knobFilterHP.dStep = 1;
	//knobFilterHP.dValue = 0.0;

	//knobWaveform.dMax = 1.0;
	//knobWaveform.dMin = 0.0;
	//knobWaveform.dStep = 0.001;
	//knobWaveform.dValue = 0.8;

	//sKnob *pSelectedKnob = nullptr;

	char keyboard[129];
	memset(keyboard, ' ', 127);
	keyboard[128] = '\0';

	auto clock_old_time = chrono::high_resolution_clock::now();
	auto clock_real_time = chrono::high_resolution_clock::now();
	double dElapsedTime = 0.0;

	while (1)
	{
		clock_real_time = chrono::high_resolution_clock::now();
		auto time_last_loop = clock_real_time - clock_old_time;
		clock_old_time = clock_real_time;
		dElapsedTime += 280.0 * chrono::duration<double>(time_last_loop).count();

		bool bDisplay = false;
		int nChannel = 0;
		//auto channel = mfile.listChannels.back();
		for (auto &channel : mfile.listChannels)
		{
			if (channel.listEvents.size() > 0)
			{
				muxNotes.lock();
				auto evt = channel.listEvents.front();
				while ((evt = channel.listEvents.front()).dRealTime <= dElapsedTime)
				{
					auto noteFound = find_if(vecNotes.begin(), vecNotes.end(), [&evt](synth::note const& item) { return item.id == evt.nNote; });
					if (noteFound == vecNotes.end())
					{
						synth::note n;
						n.channel = nChannel;
						n.id = evt.nNote;
						n.on = sound.GetTime();
						n.active = true;
						vecNotes.emplace_back(n);
						keyboard[n.id] = '#';
						bDisplay = true;
					}
					else
					{
						// Note is playing
						if (!evt.bSound)
						{
							noteFound->off = sound.GetTime();
							keyboard[noteFound->id] = ' ';
							bDisplay = true;
						}
						else
						{
							//noteFound->off = sound.GetTime();
							noteFound->on = sound.GetTime();
							keyboard[noteFound->id] = '#';
							bDisplay = true;
						}
					}
					channel.listEvents.pop_front();
					if (channel.listEvents.size() == 0)
						break;
				}
				muxNotes.unlock();
			}

			nChannel++;
		}

		if(bDisplay)
		wcout << keyboard << endl;

		//wstring knob = L"";
		//pSelectedKnob = nullptr;

		//if (GetAsyncKeyState(VK_F1) & 0x8000)
		//{
		//	pSelectedKnob = &knobFilterLP;
		//	knob = L"Low Pass: ";
		//}

		//if (GetAsyncKeyState(VK_F2) & 0x8000)
		//{
		//	pSelectedKnob = &knobFilterHP;
		//	knob = L"High Pass: ";
		//}

		//if (GetAsyncKeyState(VK_F3) & 0x8000)
		//{
		//	pSelectedKnob = &knobWaveform;
		//	knob = L"Waveform: ";
		//}


		//if (pSelectedKnob != nullptr)
		//{
		//	if (GetAsyncKeyState(VK_ADD) & 0x8000)
		//	{
		//		// Increase Knob Value
		//		pSelectedKnob->dValue = pSelectedKnob->dValue + pSelectedKnob->dStep;
		//		if (pSelectedKnob->dValue > pSelectedKnob->dMax)
		//			pSelectedKnob->dValue = pSelectedKnob->dMax;
		//	}

		//	if (GetAsyncKeyState(VK_SUBTRACT) & 0x8000)
		//	{
		//		// Decrease Knob Value
		//		pSelectedKnob->dValue = pSelectedKnob->dValue - pSelectedKnob->dStep;
		//		if (pSelectedKnob->dValue < pSelectedKnob->dMin)
		//			pSelectedKnob->dValue = pSelectedKnob->dMin;
		//	}

		//}

		
		
		//if (bResetTick)
		//{
		//	
		//}


		//for (int k = 0; k < 16; k++)
		//{
		//	short nKeyState = GetAsyncKeyState((unsigned char)("ZSXCFVGBNJMK\xbcL\xbe\xbf"[k]));
		//	
		//	// Check if note already exists in currently playing notes
		//	muxNotes.lock();
		//	auto noteFound = find_if(vecNotes.begin(), vecNotes.end(), [&k](synth::note const& item) { return item.id == k; });
		//	if (noteFound == vecNotes.end())
		//	{
		//		// Note not found in vector

		//		if (nKeyState & 0x8000)
		//		{
		//			// Key has been pressed so create a new note
		//			synth::note n;
		//			n.id = k;
		//			n.on = dTimeNow;
		//			n.active = true;

		//			// Add note to vector
		//			vecNotes.emplace_back(n);
		//		}
		//		else
		//		{
		//			// Note not in vector, but key has been released...
		//			// ...nothing to do
		//		}
		//	}
		//	else
		//	{
		//		// Note exists in vector
		//		if (nKeyState & 0x8000)
		//		{
		//			// Key is still held, so do nothing
		//			if (noteFound->off > noteFound->on)
		//			{
		//				// Key has been pressed again during release phase
		//				noteFound->on = dTimeNow;
		//				noteFound->active = true;
		//			}
		//		}
		//		else
		//		{
		//			// Key has been released, so switch off
		//			if (noteFound->off < noteFound->on)
		//			{
		//				noteFound->off = dTimeNow;
		//			}
		//		}
		//	}
		//	muxNotes.unlock();		
		//}
		//wcout << "\rNotes: " << vecNotes.size() << "    ";

		//this_thread::sleep_for(5ms);
	}

	return 0;
}
