#pragma once

#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <Audioclient.h>
#include <audiopolicy.h>
#include <Mmdeviceapi.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <avrt.h>

#pragma comment(lib, "avrt.lib")

class olcRealTimeSFX_WASAPI
{
private:
	unsigned int m_nSampleRate = 44100;
	unsigned int m_nChannels = 1;

	HANDLE				m_hEventOutput = nullptr;
	IMMDevice			*m_hwDeviceOut = nullptr;
	IAudioClient		*m_pAudioOutClient = nullptr;
	IAudioRenderClient	*m_pAudioRenderClient = nullptr;

	HANDLE				m_hEventInput = nullptr;
	IMMDevice			*m_hwDeviceIn = nullptr;
	IAudioClient		*m_pAudioInClient = nullptr;
	IAudioCaptureClient *m_pAudioCaptureClient = nullptr;

	unsigned int			m_nBlockInCount = 0;
	unsigned int			m_nBlockInSize = 0;
	short*					m_pBlockInMemory = nullptr;
	unsigned int			m_nBlockInWrite = 0;
	unsigned int			m_nBlockInRead = 0;
	std::atomic<unsigned int>	m_nBlockInAvailable = 0;

	unsigned int			m_nBlockOutCount = 0;
	unsigned int			m_nBlockOutSize = 0;
	short*					m_pBlockOutMemory = nullptr;
	unsigned int			m_nBlockOutWrite = 0;
	unsigned int			m_nBlockOutRead = 0;
	std::atomic<unsigned int>	m_nBlockOutAvailable = 0;

	std::condition_variable		m_cvBlockAvailableToProcess;
	std::mutex					m_muxBlockAvailableToProcess;

	std::thread m_threadInput, m_threadOutput, m_threadProcess;
	std::atomic<bool> m_atomActive = false;
	std::atomic<WAVEHDR*> m_atomHeaderIn = nullptr;

public:
	olcRealTimeSFX_WASAPI()
	{

	}

	bool Create(std::wstring sOutputDevice, std::wstring sInputDevice, unsigned int nSampleRate = 44100, unsigned int nChannels = 1, unsigned int nBlocks = 8, unsigned int nBlockSamples = 512)
	{
		m_nSampleRate = nSampleRate;
		m_nChannels = nChannels;
		m_nBlockInCount = nBlocks;
		m_nBlockOutCount = nBlocks;

		// Define Audio Format
		WAVEFORMATEX waveFormat;
		waveFormat.wFormatTag = WAVE_FORMAT_PCM;
		waveFormat.nSamplesPerSec = m_nSampleRate;
		waveFormat.wBitsPerSample = 3 * 8;
		waveFormat.nChannels = m_nChannels;
		waveFormat.nBlockAlign = (waveFormat.wBitsPerSample / 8) * waveFormat.nChannels;
		waveFormat.nAvgBytesPerSec = waveFormat.nSamplesPerSec * waveFormat.nBlockAlign;
		waveFormat.cbSize = 0;

		CoInitialize(nullptr);
		const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
		const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
		const IID IID_IAudioClient = __uuidof(IAudioClient);
		const IID IID_IAudioRenderClient = __uuidof(IAudioRenderClient);
		const IID IID_IAudioCaptureClient = __uuidof(IAudioCaptureClient);

		// Find and open output device
		IMMDeviceEnumerator *pEnumerator;
		HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)&pEnumerator);

		IMMDeviceCollection *pOutputDeviceCollection;
		hr = pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pOutputDeviceCollection);

		unsigned int nOutputDevices = 0;
		pOutputDeviceCollection->GetCount(&nOutputDevices);

		for (int i = 0; i < nOutputDevices; i++)
		{
			IMMDevice *pDevice;
			hr = pOutputDeviceCollection->Item(i, &pDevice);

			WCHAR *sDeviceID;
			hr = pDevice->GetId(&sDeviceID);

			IPropertyStore *pProperties;
			hr = pDevice->OpenPropertyStore(STGM_READ, &pProperties);

			PROPVARIANT varName;
			PropVariantInit(&varName);
			hr = pProperties->GetValue(PKEY_Device_FriendlyName, &varName);

			if (varName.pwszVal == sOutputDevice)
			{
				m_hwDeviceOut = pDevice;
				PropVariantClear(&varName);
				pProperties->Release();
				CoTaskMemFree(sDeviceID);
				break;
			}
			else
			{
				PropVariantClear(&varName);
				pProperties->Release();
				CoTaskMemFree(sDeviceID);
				pDevice->Release();
				m_hwDeviceOut = nullptr;
			}
		}


		// Find and open input device
		IMMDeviceCollection *pInputDeviceCollection;
		hr = pEnumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &pInputDeviceCollection);

		unsigned int nInputDevices = 0;
		pInputDeviceCollection->GetCount(&nInputDevices);

		for (int i = 0; i < nInputDevices; i++)
		{
			IMMDevice *pDevice;
			hr = pInputDeviceCollection->Item(i, &pDevice);

			WCHAR *sDeviceID;
			hr = pDevice->GetId(&sDeviceID);

			IPropertyStore *pProperties;
			hr = pDevice->OpenPropertyStore(STGM_READ, &pProperties);

			PROPVARIANT varName;
			PropVariantInit(&varName);
			hr = pProperties->GetValue(PKEY_Device_FriendlyName, &varName);

			if (varName.pwszVal == sInputDevice)
			{
				m_hwDeviceIn = pDevice;
				PropVariantClear(&varName);
				pProperties->Release();
				CoTaskMemFree(sDeviceID);
				break;
			}
			else
			{
				PropVariantClear(&varName);
				pProperties->Release();
				CoTaskMemFree(sDeviceID);
				pDevice->Release();
				m_hwDeviceIn = nullptr;
			}
		}

		WAVEFORMATEXTENSIBLE wfx;
		ZeroMemory(&wfx, sizeof(WAVEFORMATEXTENSIBLE));
		wfx.Format.cbSize = 22; // minimum required, and all that is needed (skips the SubFormat)
		wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
		wfx.Format.nChannels = 2;
		wfx.Format.nSamplesPerSec = 44100;
		wfx.Format.wBitsPerSample = 16;
		wfx.Format.nBlockAlign = (wfx.Format.nChannels * wfx.Format.wBitsPerSample) / 8;
		wfx.Format.nAvgBytesPerSec = wfx.Format.nSamplesPerSec * wfx.Format.nBlockAlign;
		wfx.Samples.wValidBitsPerSample = 16;
		wfx.dwChannelMask = KSAUDIO_SPEAKER_MONO;
		wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

		// Buffers - WASAPI works with 100ns Times slices
		// This can lead to frame packets that are not completely whole.
		// Lets say we want 10ms buffers, these give a size of 441 samples - nice
		float fBufferTime = 0.02f;
		m_nBlockOutSize = fBufferTime * (float)wfx.Format.nSamplesPerSec;
		m_nBlockInSize = fBufferTime * (float)wfx.Format.nSamplesPerSec;

		// On my Roland Quad Capture, minimum buffer size is 3ms, which is 30000 * 100ns
		REFERENCE_TIME nDevicePeriod;
		nDevicePeriod = (fBufferTime * 10000000.0f);

		// Initialise Output
		hr = m_hwDeviceOut->Activate(IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&m_pAudioOutClient);
		hr = m_pAudioOutClient->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &wfx.Format, nullptr);
		//hr = m_pAudioOutClient->GetDevicePeriod(nullptr, &nDevicePeriod);
		hr = m_pAudioOutClient->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, nDevicePeriod, nDevicePeriod, &wfx.Format, nullptr);
		hr = m_pAudioOutClient->GetService(IID_IAudioRenderClient, (void**)&m_pAudioRenderClient);

		// Initialise Input
		hr = m_hwDeviceIn->Activate(IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&m_pAudioInClient);
		hr = m_pAudioInClient->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &wfx.Format, nullptr);
		//hr = m_pAudioInClient->GetDevicePeriod(nullptr, &nDevicePeriod);
		hr = m_pAudioInClient->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, nDevicePeriod, nDevicePeriod, &wfx.Format, nullptr);
		hr = m_pAudioInClient->GetService(IID_IAudioCaptureClient, (void**)&m_pAudioCaptureClient);


		// Create Callback Event
		m_hEventOutput = CreateEvent(NULL, FALSE, FALSE, NULL);
		hr = m_pAudioOutClient->SetEventHandle(m_hEventOutput);

		m_hEventInput = CreateEvent(NULL, FALSE, FALSE, NULL);
		hr = m_pAudioInClient->SetEventHandle(m_hEventInput);


		m_pBlockInMemory = new short[m_nBlockInCount * m_nBlockInSize * 2];
		m_nBlockInWrite = 0;
		m_nBlockInRead = 0;
		m_nBlockInAvailable = 0;

		m_pBlockOutMemory = new short[m_nBlockInCount * m_nBlockOutSize * 2];
		m_nBlockOutWrite = 0;
		m_nBlockOutRead = 0;
		m_nBlockOutAvailable = 0;


		m_atomActive = true;
		m_threadProcess = std::thread(&olcRealTimeSFX_WASAPI::ThreadProcess, this);
		m_threadOutput = std::thread(&olcRealTimeSFX_WASAPI::ThreadOutput, this);
		m_threadInput = std::thread(&olcRealTimeSFX_WASAPI::ThreadInput, this);



		return true;
	}

	bool Destroy()
	{
		return false;
	}

	void ThreadInput()
	{
		DWORD taskIndex = 0;
		HANDLE hTask = AvSetMmThreadCharacteristics(TEXT("Pro Audio"), &taskIndex);

		unsigned char *inBuffer;
		UINT32 nFrameCount = 0;
		HRESULT hr = m_pAudioInClient->GetBufferSize(&nFrameCount);
		float fTime = 0.0f;
		float fTimeStep = 1.0f / 44100.0f;

		hr = m_pAudioInClient->Start();


		while (m_atomActive)
		{
			DWORD retval = WaitForSingleObject(m_hEventInput, 2000);
			if (retval != WAIT_OBJECT_0)
			{
				// Timeout
				m_atomActive = false;
			}
			else
			{
				UINT32 nPacketSize;
				DWORD flags;
				hr = m_pAudioCaptureClient->GetNextPacketSize(&nPacketSize);

				if (nPacketSize > 0)
				{
					UINT32 nInputFramesAvailable;
					hr = m_pAudioCaptureClient->GetBuffer(&inBuffer, &nInputFramesAvailable, &flags, nullptr, nullptr);

					for (int n = 0; n < nInputFramesAvailable; n++)
					{
						m_pBlockInMemory[m_nBlockInWrite * m_nBlockInSize * 2 + 2 * n + 0] = ((short*)inBuffer)[2 * n + 0];
						m_pBlockInMemory[m_nBlockInWrite * m_nBlockInSize * 2 + 2 * n + 1] = ((short*)inBuffer)[2 * n + 1];
					}

					//for (int n = 0; n < nInputFramesAvailable; n++)
					//{
					//float dFreq = 220.0f * 3.14159f * 2.0f * fTime + 1.0f * 220.0f * (sin(0.5f * 3.14159f * 2.0f * fTime));

					//m_pBlockInMemory[m_nBlockInWrite * 441 * 2 + 2 * n + 0] = (short)(1000.0f * sinf(dFreq));
					//m_pBlockInMemory[m_nBlockInWrite * 441 * 2 + 2 * n + 1] = (short)(1000.0f * sinf(dFreq));
					//fTime += fTimeStep;
					//}	


					m_nBlockInWrite++;
					m_nBlockInWrite = m_nBlockInWrite % m_nBlockInCount;

					hr = m_pAudioCaptureClient->ReleaseBuffer(nInputFramesAvailable);

					m_nBlockInAvailable++;
					std::unique_lock<std::mutex> lm(m_muxBlockAvailableToProcess);
					m_cvBlockAvailableToProcess.notify_one();
				}

			}

		}

		m_pAudioInClient->Stop();
		std::cout << "Input Stopped" << std::endl;
	}

	void ThreadOutput()
	{
		DWORD taskIndex = 0;
		HANDLE hTask = AvSetMmThreadCharacteristics(TEXT("Pro Audio"), &taskIndex);
		float fTime = 0.0f;
		float fTimeStep = 1.0f / 44100.0f;
		unsigned char *inBuffer, *outBuffer;
		UINT32 nFrameCount = 0;
		HRESULT hr = m_pAudioOutClient->GetBufferSize(&nFrameCount);

		// Hmm, have to fill one buffer before calling start or regular glitching
		hr = m_pAudioRenderClient->GetBuffer(nFrameCount, &outBuffer);
		for (int i = 0; i < nFrameCount * 2; i += 2)
		{
			((short*)outBuffer)[i + 0] = 0.0f; // 1000.0f * sinf(fTime * 3.14159f * 220.0f * 2.0f);
			((short*)outBuffer)[i + 1] = 0.0f; // 1000.0f * sinf(fTime * 3.14159f * 220.0f * 2.0f);
			fTime += fTimeStep;
		}
		hr = m_pAudioRenderClient->ReleaseBuffer(nFrameCount, 0);

		hr = m_pAudioOutClient->Start();

		while (m_atomActive)
		{
			DWORD retval = WaitForSingleObject(m_hEventOutput, 2000);
			if (retval != WAIT_OBJECT_0)
			{
				// Timeout
				m_atomActive = false;
			}
			else
			{
				// Request for more audio has occured
				hr = m_pAudioRenderClient->GetBuffer(nFrameCount, &outBuffer);
				if (m_nBlockOutAvailable > 0)
				{
					for (int n = 0; n < nFrameCount; n++)
					{
						((short*)outBuffer)[2 * n + 0] = m_pBlockOutMemory[m_nBlockOutRead * m_nBlockOutSize * 2 + 2 * n + 0];
						((short*)outBuffer)[2 * n + 1] = m_pBlockOutMemory[m_nBlockOutRead * m_nBlockOutSize * 2 + 2 * n + 1];
					}


					m_nBlockOutRead++;
					m_nBlockOutRead = m_nBlockOutRead % m_nBlockOutCount;

					/*for (int i = 0; i < nFrameCount * 2; i += 2)
					{
					float dFreq = 220.0f * 3.14159f * 2.0f * fTime + 1.0f * 220.0f * (sin(0.5f * 3.14159f * 2.0f * fTime));

					((short*)outBuffer)[i + 0] = 1000.0f * sinf(dFreq);
					((short*)outBuffer)[i + 1] = 1000.0f * sinf(dFreq);
					fTime += fTimeStep;
					}	*/

					hr = m_pAudioRenderClient->ReleaseBuffer(nFrameCount, 0);
					m_nBlockOutAvailable--;
				}
				else // output silence
					hr = m_pAudioRenderClient->ReleaseBuffer(nFrameCount, AUDCLNT_BUFFERFLAGS_SILENT);
			}

		}

		m_pAudioOutClient->Stop();
		std::cout << "Output Stopped" << std::endl;
	}

	void ThreadProcess()
	{
		DWORD taskIndex = 0;
		HANDLE hTask = AvSetMmThreadCharacteristics(TEXT("Pro Audio"), &taskIndex);

		float *fBufferInL = new float[m_nBlockOutSize];
		float *fBufferInR = new float[m_nBlockOutSize];

		float *fBufferOutL = new float[m_nBlockOutSize];
		float *fBufferOutR = new float[m_nBlockOutSize];

		float fGlobalTime = 0.0f;
		float fTimeStep = 1.0f / 44100.0f;

		while (m_atomActive)
		{
			// If block available for processing
			// Wait for input data block to arrive
			while (m_nBlockInAvailable == 0)
			{
				std::unique_lock<std::mutex> lm(m_muxBlockAvailableToProcess);
				m_cvBlockAvailableToProcess.wait(lm);
			}

			for (int n = 0; n < m_nBlockInSize; n++)
			{
				fBufferInL[n] = (float)m_pBlockInMemory[m_nBlockInRead * m_nBlockInSize * 2 + 2 * n + 0] / (float)MAXSHORT;
				fBufferInR[n] = (float)m_pBlockInMemory[m_nBlockInRead * m_nBlockInSize * 2 + 2 * n + 1] / (float)MAXSHORT;
			}

			m_nBlockInRead++;
			m_nBlockInRead = m_nBlockInRead % m_nBlockInCount;
			m_nBlockInAvailable--;

			Process(fGlobalTime, fTimeStep, m_nBlockOutSize, fBufferInL, fBufferInR, fBufferOutL, fBufferOutR);
			fGlobalTime += fTimeStep * 441.0f;

			for (int n = 0; n < m_nBlockOutSize; n++)
			{
				m_pBlockOutMemory[m_nBlockOutWrite * m_nBlockOutSize * 2 + 2 * n + 0] = (short)(fBufferOutL[n] * (float)MAXSHORT);
				m_pBlockOutMemory[m_nBlockOutWrite * m_nBlockOutSize * 2 + 2 * n + 1] = (short)(fBufferOutR[n] * (float)MAXSHORT);
			}

			m_nBlockOutWrite++;
			m_nBlockOutWrite = m_nBlockOutWrite % m_nBlockOutCount;
			m_nBlockOutAvailable++;

		}
	}


	void Process(float fTime, float fTimeStep, int nSamples, float *pSamplesInL, float *pSamplesInR, float *pSamplesOutL, float *pSamplesOutR)
	{
		// Pass Through
		for (int n = 0; n < nSamples; n++)
		{
			float fSample = pSamplesInL[n] * 10.0f;
			if (fSample > 1.0f) fSample = 1.0f;
			if (fSample < -1.0f) fSample = -1.0f;

			pSamplesOutL[n] = fSample;
			pSamplesOutR[n] = fSample;
		}

		//cout << "Procesing" << endl;
	}

public:
	static std::vector<std::wstring> EnumerateOutputDevices()
	{
		CoInitialize(nullptr);
		std::vector<std::wstring> sDevices;

		const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
		const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);

		IMMDeviceEnumerator *pEnumerator;
		HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)&pEnumerator);

		IMMDeviceCollection *pDeviceCollection;
		hr = pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pDeviceCollection);

		unsigned int nOutputDevices = 0;
		pDeviceCollection->GetCount(&nOutputDevices);

		for (int i = 0; i < nOutputDevices; i++)
		{
			IMMDevice *pDevice;
			hr = pDeviceCollection->Item(i, &pDevice);

			WCHAR *sDeviceID;
			hr = pDevice->GetId(&sDeviceID);

			IPropertyStore *pProperties;
			hr = pDevice->OpenPropertyStore(STGM_READ, &pProperties);

			PROPVARIANT varName;
			PropVariantInit(&varName);
			hr = pProperties->GetValue(PKEY_Device_FriendlyName, &varName);

			sDevices.emplace_back(varName.pwszVal);

			PropVariantClear(&varName);
			pProperties->Release();
			CoTaskMemFree(sDeviceID);
			pDevice->Release();
		}

		pDeviceCollection->Release();
		pEnumerator->Release();
		return sDevices;
	}

	static std::vector<std::wstring> EnumerateInputDevices()
	{
		CoInitialize(nullptr);
		std::vector<std::wstring> sDevices;

		const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
		const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);


		IMMDeviceEnumerator *pEnumerator;
		HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)&pEnumerator);

		IMMDeviceCollection *pDeviceCollection;
		hr = pEnumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &pDeviceCollection);

		unsigned int nInputDevices = 0;
		pDeviceCollection->GetCount(&nInputDevices);

		for (int i = 0; i < nInputDevices; i++)
		{
			IMMDevice *pDevice;
			hr = pDeviceCollection->Item(i, &pDevice);

			WCHAR *sDeviceID;
			hr = pDevice->GetId(&sDeviceID);

			IPropertyStore *pProperties;
			hr = pDevice->OpenPropertyStore(STGM_READ, &pProperties);

			PROPVARIANT varName;
			PropVariantInit(&varName);
			hr = pProperties->GetValue(PKEY_Device_FriendlyName, &varName);

			sDevices.emplace_back(varName.pwszVal);

			PropVariantClear(&varName);
			pProperties->Release();
			CoTaskMemFree(sDeviceID);
			pDevice->Release();
		}

		pDeviceCollection->Release();
		pEnumerator->Release();

		return sDevices;
	}
};
