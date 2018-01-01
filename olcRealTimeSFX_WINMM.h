#pragma once

#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <condition_variable>
using namespace std;

#include <Windows.h>
#pragma comment(lib, "winmm.lib")

class olcRealTimeSFX_WINMM
{
private:
	unsigned int m_nSampleRate = 44100;
	unsigned int m_nChannels = 1;

	HWAVEOUT m_hwDeviceOut = nullptr;
	HWAVEIN  m_hwDeviceIn = nullptr;

	short*		m_pBlockInMemory = nullptr;
	WAVEHDR*	m_pWaveInHeaders = nullptr;
	unsigned int m_nInputBlockCurrent = 0;
	atomic<unsigned int> m_nInputBlockFree = 0;
	condition_variable m_cvInputBufferNotEmpty;
	mutex m_muxInputBufferNotEmpty;

	short*		m_pBlockOutMemory = nullptr;
	WAVEHDR*	m_pWaveOutHeaders = nullptr;
	unsigned int m_nOutputBlockCurrent = 0;
	atomic<unsigned int> m_nOutputBlockFree = 0;
	condition_variable m_cvOutputBufferNotFull;
	mutex m_muxOutputBufferNotFull;

	unsigned int m_nBlockInCount = 0;
	unsigned int m_nBlockOutCount = 0;
	unsigned int m_nBlockInSamples = 0;
	unsigned int m_nBlockOutSamples = 0;
	unsigned int m_nBlockInCurrent = 0;
	unsigned int m_nBlockOutCurrent = 0;

	thread m_thread;
	atomic<bool> m_atomActive = false;
	atomic<WAVEHDR*> m_atomHeaderIn = nullptr;

public:
	olcRealTimeSFX_WINMM()
	{

	}

	bool Create(wstring sOutputDevice, wstring sInputDevice, unsigned int nSampleRate = 44100, unsigned int nChannels = 1, unsigned int nBlocks = 8, unsigned int nBlockSamples = 512)
	{
		m_nSampleRate = nSampleRate;
		m_nChannels = nChannels;
		m_nBlockInCount = nBlocks;
		m_nBlockOutCount = nBlocks;
		m_nBlockInSamples = nBlockSamples;
		m_nBlockOutSamples = nBlockSamples;

		// Define Audio Format
		WAVEFORMATEX waveFormat;
		waveFormat.wFormatTag = WAVE_FORMAT_PCM;
		waveFormat.nSamplesPerSec = m_nSampleRate;
		waveFormat.wBitsPerSample = sizeof(short) * 8;
		waveFormat.nChannels = m_nChannels;
		waveFormat.nBlockAlign = (waveFormat.wBitsPerSample / 8) * waveFormat.nChannels;
		waveFormat.nAvgBytesPerSec = waveFormat.nSamplesPerSec * waveFormat.nBlockAlign;
		waveFormat.cbSize = 0;

		// Validate Output Device
		vector<wstring> devicesout = EnumerateOutputDevices();
		auto dout = std::find(devicesout.begin(), devicesout.end(), sOutputDevice);
		if (dout != devicesout.end())
		{
			// Device is available
			int nDeviceID = distance(devicesout.begin(), dout);

			// Open Device if valid
			if (waveOutOpen(&m_hwDeviceOut, nDeviceID, &waveFormat, (DWORD_PTR)waveOutProcWrap, (DWORD_PTR)this, CALLBACK_FUNCTION) != S_OK)
				return Destroy();
		}

		vector<wstring> devicesin = EnumerateInputDevices();
		auto din = std::find(devicesin.begin(), devicesin.end(), sInputDevice);
		if (din != devicesin.end())
		{
			// Device is available
			int nDeviceID = distance(devicesin.begin(), din);

			// Open Device if valid
			if (waveInOpen(&m_hwDeviceIn, nDeviceID, &waveFormat, (DWORD_PTR)waveInProcWrap, (DWORD_PTR)this, CALLBACK_FUNCTION) != S_OK)
				return Destroy();
		}

		// If here, then both devices created - allocate memory
		m_pBlockOutMemory = new short[m_nBlockOutCount * m_nBlockOutSamples];
		for (int i = 0; i < m_nBlockOutCount * m_nBlockOutSamples; i++)
			m_pBlockOutMemory[i] = 0;

		m_pBlockInMemory = new short[m_nBlockInCount * m_nBlockInSamples];
		for (int i = 0; i < m_nBlockInCount * m_nBlockInSamples; i++)
			m_pBlockInMemory[i] = 0;

		m_pWaveOutHeaders = new WAVEHDR[m_nBlockOutCount];
		memset(m_pWaveOutHeaders, 0, sizeof(WAVEHDR) * m_nBlockOutCount);

		m_pWaveInHeaders = new WAVEHDR[m_nBlockInCount];
		memset(m_pWaveInHeaders, 0, sizeof(WAVEHDR) * m_nBlockInCount);

		// Link headers to block memory
		for (unsigned int n = 0; n < m_nBlockOutCount; n++)
		{
			m_pWaveOutHeaders[n].dwBufferLength = m_nBlockOutSamples * sizeof(short);
			m_pWaveOutHeaders[n].lpData = (LPSTR)(m_pBlockOutMemory + (n * m_nBlockOutSamples));
		}

		for (unsigned int n = 0; n < m_nBlockInCount; n++)
		{
			m_pWaveInHeaders[n].dwBufferLength = m_nBlockInSamples * sizeof(short);
			m_pWaveInHeaders[n].lpData = (LPSTR)(m_pBlockInMemory + (n * m_nBlockInSamples));
		}

		m_nOutputBlockFree = m_nBlockOutCount;
		m_nInputBlockFree = m_nBlockInCount;

		m_thread = thread(&olcRealTimeSFX_WINMM::MainThread, this);
		m_atomActive = true;
	}

	bool Destroy()
	{
		return false;
	}

	void MainThread()
	{
		// Add Buffers
		for (int n = 0; n < m_nBlockInCount; n++)
		{
			waveInPrepareHeader(m_hwDeviceIn, &m_pWaveInHeaders[n], sizeof(WAVEHDR));
			waveInAddBuffer(m_hwDeviceIn, &m_pWaveInHeaders[n], sizeof(WAVEHDR));
		}

		m_nInputBlockCurrent = m_nBlockInCount - 1;

		// Start recording stream
		if (waveInStart(m_hwDeviceIn) != S_OK)
			m_atomActive = false;

		float *fBlockSamples = new float[m_nBlockInSamples];

		float *fDelayBuffer = new float[22050]();
		int nDelayWrite = 0;
		int nDelayRead = 1000;

		while (m_atomActive)
		{
			// Wait for input data block to arrive
			unique_lock<mutex> lm(m_muxInputBufferNotEmpty);
			m_cvInputBufferNotEmpty.wait(lm);

			//wcout << L"Data Arrived" << endl;

			// Header has data
			for (int n = 0; n < m_nBlockInSamples; n++)
				fBlockSamples[n] = ((float)((short*)m_pWaveInHeaders[m_nInputBlockCurrent].lpData)[n]) / (float)MAXSHORT;

			// Prepare new input header block
			m_nInputBlockCurrent++;
			m_nInputBlockCurrent %= m_nBlockInCount;
			waveInPrepareHeader(m_hwDeviceIn, &m_pWaveInHeaders[m_nInputBlockCurrent], sizeof(WAVEHDR));
			waveInAddBuffer(m_hwDeviceIn, &m_pWaveInHeaders[m_nInputBlockCurrent], sizeof(WAVEHDR));
			m_nInputBlockFree++;

			// Process Data - Distortion!
			for (int n = 0; n < m_nBlockOutSamples; n++)
			{
				float out = fBlockSamples[n] * 40.0f;
				out += fDelayBuffer[nDelayRead] * 0.05f;
				fDelayBuffer[nDelayWrite] = out;


				nDelayRead++; nDelayRead %= 22050;
				nDelayWrite++; nDelayWrite %= 22050;

				if (out < -1.0f)
					out = -1.0f;
				if (out > 1.0f)
					out = 1.0f;
				((short*)m_pWaveOutHeaders[m_nOutputBlockCurrent].lpData)[n] = (short)(out * (float)MAXSHORT);
			}

			// Send block to sound device
			waveOutPrepareHeader(m_hwDeviceOut, &m_pWaveOutHeaders[m_nOutputBlockCurrent], sizeof(WAVEHDR));
			waveOutWrite(m_hwDeviceOut, &m_pWaveOutHeaders[m_nOutputBlockCurrent], sizeof(WAVEHDR));
			m_nOutputBlockCurrent++;
			m_nOutputBlockCurrent %= m_nBlockOutCount;
			m_nOutputBlockFree--;
		}
	}

	// Handler for soundcard request for more data
	void waveOutProc(HWAVEOUT hWaveOut, UINT uMsg, DWORD dwParam1, DWORD dwParam2)
	{
		if (uMsg != WOM_DONE) return;
		m_nOutputBlockFree++;
		unique_lock<mutex> lm(m_muxOutputBufferNotFull);
		m_cvOutputBufferNotFull.notify_one();
	}

	// Handler for soundcard delivery of more data
	void waveInProc(HWAVEIN hWaveIn, UINT uMsg, DWORD dwParam1, DWORD dwParam2)
	{
		if (uMsg != WIM_DATA) return;
		m_nInputBlockFree--;
		unique_lock<mutex> lm(m_muxInputBufferNotEmpty);
		m_cvInputBufferNotEmpty.notify_one();
	}

	// Static wrapper for sound card handler
	static void CALLBACK waveInProcWrap(HWAVEIN hWaveIn, UINT uMsg, DWORD dwInstance, DWORD dwParam1, DWORD dwParam2)
	{
		((olcRealTimeSFX_WINMM*)dwInstance)->waveInProc(hWaveIn, uMsg, dwParam1, dwParam2);
	}

	// Static wrapper for sound card handler
	static void CALLBACK waveOutProcWrap(HWAVEOUT hWaveOut, UINT uMsg, DWORD dwInstance, DWORD dwParam1, DWORD dwParam2)
	{
		((olcRealTimeSFX_WINMM*)dwInstance)->waveOutProc(hWaveOut, uMsg, dwParam1, dwParam2);
	}

	void Process(float fTime, int nSamples, float *pSamplesIn, float *pSamplesOut)
	{

	}

public:
	static vector<wstring> EnumerateOutputDevices()
	{
		int nDeviceCount = waveOutGetNumDevs();
		vector<wstring> sDevices;
		WAVEOUTCAPS woc;
		for (int n = 0; n < nDeviceCount; n++)
			if (waveOutGetDevCaps(n, &woc, sizeof(WAVEOUTCAPS)) == S_OK)
				sDevices.push_back(woc.szPname);
		return sDevices;
	}

	static vector<wstring> EnumerateInputDevices()
	{
		int nDeviceCount = waveInGetNumDevs();
		vector<wstring> sDevices;
		WAVEINCAPS wic;
		for (int n = 0; n < nDeviceCount; n++)
			if (waveInGetDevCaps(n, &wic, sizeof(WAVEINCAPS)) == S_OK)
				sDevices.push_back(wic.szPname);
		return sDevices;
	}

};
