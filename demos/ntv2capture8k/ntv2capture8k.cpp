/* SPDX-License-Identifier: MIT */
/**
	@file		ntv2capture8k.cpp
	@brief		Implementation of NTV2Capture class.
	@copyright	(C) 2012-2022 AJA Video Systems, Inc.  All rights reserved.
**/

#include "ntv2capture8k.h"
#include "ntv2devicescanner.h"
#include "ntv2utils.h"
#include "ntv2devicefeatures.h"
#include "ajabase/system/process.h"

using namespace std;


//////////////////////////////////////////////////////////////////////////////////////	NTV2Capture8K IMPLEMENTATION

NTV2Capture8K::NTV2Capture8K (const CaptureConfig & inConfig)
	:	mConsumerThread		(AJAThread()),
		mProducerThread		(AJAThread()),
		mDeviceID			(DEVICE_ID_NOTFOUND),
		mConfig				(inConfig),
		mVideoFormat		(NTV2_FORMAT_UNKNOWN),
		mSavedTaskMode		(NTV2_DISABLE_TASKS),
		mAudioSystem		(inConfig.fWithAudio ? NTV2_AUDIOSYSTEM_1 : NTV2_AUDIOSYSTEM_INVALID),
		mHostBuffers		(),
		mAVCircularBuffer	(),
		mGlobalQuit			(false)
{
}	//	constructor


NTV2Capture8K::~NTV2Capture8K ()
{
	//	Stop my capture and consumer threads, then destroy them...
	Quit();

	//	Unsubscribe from VBI events...
	mDevice.UnsubscribeInputVerticalEvent(mConfig.fInputChannel);
	mDevice.UnsubscribeOutputVerticalEvent(NTV2_CHANNEL1);

}	//	destructor


void NTV2Capture8K::Quit (void)
{
	//	Set the global 'quit' flag, and wait for the threads to go inactive...
	mGlobalQuit = true;

	while (mConsumerThread.Active())
		AJATime::Sleep(10);

	while (mProducerThread.Active())
		AJATime::Sleep(10);

	mDevice.DMABufferUnlockAll();

	//	Restore some of the device's former state...
	if (!mConfig.fDoMultiFormat)
	{
		mDevice.ReleaseStreamForApplication (kDemoAppSignature, int32_t(AJAProcess::GetPid()));
		mDevice.SetEveryFrameServices(mSavedTaskMode);		//	Restore prior task mode
	}

}	//	Quit


AJAStatus NTV2Capture8K::Init (void)
{
	AJAStatus status (AJA_STATUS_SUCCESS);

	//	Open the device...
	if (!CNTV2DeviceScanner::GetFirstDeviceFromArgument (mConfig.fDeviceSpec, mDevice))
		{cerr << "## ERROR:  Device '" << mConfig.fDeviceSpec << "' not found" << endl;  return AJA_STATUS_OPEN;}

	if (!mDevice.IsDeviceReady())
		{cerr << "## ERROR:  '" << mDevice.GetDisplayName() << "' not ready" << endl;  return AJA_STATUS_INITIALIZE;}

	mDeviceID = mDevice.GetDeviceID();						//	Keep the device ID handy, as it's used frequently
	if (!::NTV2DeviceCanDoCapture(mDeviceID))
		{cerr << "## ERROR:  '" << mDevice.GetDisplayName() << "' is playback-only" << endl;  return AJA_STATUS_FEATURE;}
	if (!::NTV2DeviceCanDo12gRouting(mDeviceID))
        {cerr << "## ERROR: '" << ::NTV2DeviceIDToString(mDeviceID,true) << "' lacks 12G SDI";  return AJA_STATUS_FEATURE;}

	if (!::NTV2DeviceCanDoFrameBufferFormat (mDeviceID, mConfig.fPixelFormat))
	{	cerr	<< "## ERROR:  '" << mDevice.GetDisplayName() << "' doesn't support '"
				<< ::NTV2FrameBufferFormatToString(mConfig.fPixelFormat, true) << "' ("
				<< ::NTV2FrameBufferFormatToString(mConfig.fPixelFormat, false) << ", " << DEC(mConfig.fPixelFormat) << ")" << endl;
		return AJA_STATUS_UNSUPPORTED;
	}

	ULWord	appSignature	(0);
	int32_t	appPID			(0);
	mDevice.GetStreamingApplication (appSignature, appPID);	//	Who currently "owns" the device?
	mDevice.GetEveryFrameServices(mSavedTaskMode);			//	Save the current device state
	if (!mConfig.fDoMultiFormat)
	{
		if (!mDevice.AcquireStreamForApplication (kDemoAppSignature, int32_t(AJAProcess::GetPid())))
		{
			cerr << "## ERROR:  Unable to acquire '" << mDevice.GetDisplayName() << "' because another app (pid " << appPID << ") owns it" << endl;
			return AJA_STATUS_BUSY;		//	Another app is using the device
		}
		mDevice.GetEveryFrameServices(mSavedTaskMode);		//	Save the current state before we change it
	}
	mDevice.SetEveryFrameServices(NTV2_OEM_TASKS);			//	Prevent interference from AJA retail services

	if (::NTV2DeviceCanDoMultiFormat(mDeviceID))
		mDevice.SetMultiFormatMode(mConfig.fDoMultiFormat);

	//	This demo permits only the input channel/frameStore to be specified.  Set the input source here...
	const NTV2Channel origCh (mConfig.fInputChannel);
	if (UWord(origCh) >= ::NTV2DeviceGetNumFrameStores(mDeviceID))
	{
		cerr << "## ERROR: No such Ch" << DEC(origCh+1) << " for '" << ::NTV2DeviceIDToString(mDeviceID,true) << "'" << endl;
		return AJA_STATUS_BAD_PARAM;
	}
	//	Must use Ch1/Ch3/Ch5/Ch7;  Must use Ch1 for squares...
	if ((origCh & 1)  ||  (origCh && !mConfig.fDoTSIRouting))
	{
		cerr << "## ERROR: Cannot use Ch" << DEC(origCh+1) << " for '" << ::NTV2DeviceIDToString(mDeviceID,true) << "'" << endl;
		return AJA_STATUS_BAD_PARAM;
	}
	mConfig.fInputSource = ::NTV2ChannelToInputSource(mConfig.fInputChannel);	//	Must use corresponding SDI inputs

	//	Determine input connectors and frameStores to be used...
	mActiveFrameStores = ::NTV2MakeChannelSet (mConfig.fInputChannel, mConfig.fDoTSIRouting ? 2 : 4);
	mActiveSDIs        = ::NTV2MakeChannelSet (mConfig.fInputChannel, mConfig.fDoTSIRouting ? (::IsRGBFormat(mConfig.fPixelFormat) ? 4 : 2) : 4);
	//	Note for TSI into YUV framestore:  if input signal is QuadQuadHFR, we'll add 2 more SDIs (see below in SetupVideo)

	//	Set up the video and audio...
	status = SetupVideo();
	if (AJA_FAILURE(status))
		return status;

	if (mConfig.fWithAudio)
		status = SetupAudio();
	if (AJA_FAILURE(status))
		return status;

	//	Set up the circular buffers, the device signal routing, and both playout and capture AutoCirculate...
	SetupHostBuffers();
	if (!RouteInputSignal())
		return AJA_STATUS_FAIL;

	#if defined(_DEBUG)
		cerr	<< mConfig << endl << "FrameStores: " << ::NTV2ChannelSetToStr(mActiveFrameStores) << endl
				<< "Inputs: " << ::NTV2ChannelSetToStr(mActiveSDIs) << endl;
		if (mDevice.IsRemote())
			cerr	<< "Device Description:  " << mDevice.GetDescription() << endl << endl;
	#endif	//	defined(_DEBUG)
	return AJA_STATUS_SUCCESS;

}	//	Init


AJAStatus NTV2Capture8K::SetupVideo (void)
{
	//	Enable the FrameStores we intend to use...
	mDevice.EnableChannels (mActiveFrameStores, !mConfig.fDoMultiFormat);	//	Disable the rest if we own the device

	//	Enable and subscribe to VBIs (critical on Windows)...
	mDevice.EnableInputInterrupt(mConfig.fInputChannel);
	mDevice.SubscribeInputVerticalEvent(mConfig.fInputChannel);
	mDevice.SubscribeOutputVerticalEvent(NTV2_CHANNEL1);

	//	If the device supports bi-directional SDI and the requested input is SDI,
	//	ensure the SDI connector(s) are configured to receive...
	if (::NTV2DeviceHasBiDirectionalSDI(mDeviceID) && NTV2_INPUT_SOURCE_IS_SDI(mConfig.fInputSource))
	{
		mDevice.SetSDITransmitEnable (mActiveSDIs, false);				//	Set SDI connector(s) to receive
		mDevice.WaitForOutputVerticalInterrupt (NTV2_CHANNEL1, 10);		//	Wait 10 VBIs to allow reciever to lock
	}

	//	Determine the input video signal format...
	mVideoFormat = mDevice.GetInputVideoFormat(mConfig.fInputSource);
	if (mVideoFormat == NTV2_FORMAT_UNKNOWN)
		{cerr << "## ERROR:  No input signal or unknown format" << endl;  return AJA_STATUS_NOINPUT;}
	CNTV2DemoCommon::Get8KInputFormat(mVideoFormat);    //  Convert to 8K format
	mFormatDesc = NTV2FormatDescriptor(mVideoFormat, mConfig.fPixelFormat);
	if (mConfig.fDoTSIRouting  &&  !::IsRGBFormat(mConfig.fPixelFormat)  &&  NTV2_IS_QUAD_QUAD_HFR_VIDEO_FORMAT(mVideoFormat))
	{
		mActiveSDIs = ::NTV2MakeChannelSet (mConfig.fInputChannel, 4);	//	Add 2 more SDIs for TSI + YUV frameStores + QuadQuadHFR
		mDevice.SetSDITransmitEnable (mActiveSDIs, false);				//	Set SDIs to receive
	}

	//	Setting SDI output clock timing/reference is unimportant for capture-only apps...
	if (!mConfig.fDoMultiFormat)						//	...if not sharing the device...
		mDevice.SetReference(NTV2_REFERENCE_FREERUN);	//	...let it free-run

	//	Set the device video format to whatever was detected at the input(s)...
	mDevice.SetVideoFormat (mVideoFormat, false, false, mConfig.fInputChannel);
	mDevice.SetVANCMode (mActiveFrameStores, NTV2_VANCMODE_OFF);	//	Disable VANC
    mDevice.SetQuadQuadFrameEnable (true, mConfig.fInputChannel);						//  UHD2/8K requires QuadQuad mode
    mDevice.SetQuadQuadSquaresEnable (!mConfig.fDoTSIRouting, mConfig.fInputChannel);	//	Set QuadQuadSquares mode if not TSI

	//	Set the frame buffer pixel format for the FrameStore(s) to be used on the device...
	mDevice.SetFrameBufferFormat (mActiveFrameStores, mConfig.fPixelFormat);
	return AJA_STATUS_SUCCESS;

}	//	SetupVideo


AJAStatus NTV2Capture8K::SetupAudio (void)
{
	//	In multiformat mode, base the audio system on the channel...
	if (mConfig.fDoMultiFormat)
		if (::NTV2DeviceGetNumAudioSystems(mDeviceID) > 1)
			if (UWord(mConfig.fInputChannel) < ::NTV2DeviceGetNumAudioSystems(mDeviceID))
				mAudioSystem = ::NTV2ChannelToAudioSystem(mConfig.fInputChannel);

	NTV2AudioSystemSet audSystems (::NTV2MakeAudioSystemSet (mAudioSystem, 1));
	CNTV2DemoCommon::ConfigureAudioSystems (mDevice, mConfig, audSystems);
	return AJA_STATUS_SUCCESS;

}	//	SetupAudio


void NTV2Capture8K::SetupHostBuffers (void)
{
	//	Let my circular buffer know when it's time to quit...
	mAVCircularBuffer.SetAbortFlag(&mGlobalQuit);

	ULWord F1AncSize(0), F2AncSize(0);
	if (mConfig.fWithAnc)
	{	//	Use the max anc size stipulated by the AncFieldOffset VReg values...
		ULWord	F1OffsetFromEnd(0), F2OffsetFromEnd(0);
		mDevice.ReadRegister(kVRegAncField1Offset, F1OffsetFromEnd);	//	# bytes from end of 8MB/16MB frame
		mDevice.ReadRegister(kVRegAncField2Offset, F2OffsetFromEnd);	//	# bytes from end of 8MB/16MB frame
		//	Based on the offsets, calculate the max anc capacity
		F1AncSize = F2OffsetFromEnd > F1OffsetFromEnd ? 0 : F1OffsetFromEnd - F2OffsetFromEnd;
		F2AncSize = F2OffsetFromEnd > F1OffsetFromEnd ? F2OffsetFromEnd - F1OffsetFromEnd : F2OffsetFromEnd;
	}

	//	Allocate and add each in-host NTV2FrameData to my circular buffer member variable...
	const size_t audioBufferSize (NTV2_AUDIOSIZE_MAX);
	mHostBuffers.reserve(size_t(CIRCULAR_BUFFER_SIZE));
	cout << "## NOTE: Buffer size:  vid=" << mFormatDesc.GetVideoWriteSize() << " aud=" << audioBufferSize << " anc=" << F1AncSize << endl;
	while (mHostBuffers.size() < size_t(CIRCULAR_BUFFER_SIZE))
	{
		mHostBuffers.push_back(NTV2FrameData());
		NTV2FrameData & frameData(mHostBuffers.back());
		frameData.fVideoBuffer.Allocate(mFormatDesc.GetVideoWriteSize());
		frameData.fAudioBuffer.Allocate(NTV2_IS_VALID_AUDIO_SYSTEM(mAudioSystem) ? audioBufferSize : 0);
		frameData.fAncBuffer.Allocate(F1AncSize);
		frameData.fAncBuffer2.Allocate(F2AncSize);
		mAVCircularBuffer.Add(&frameData);

		//	8K requires page-locked buffers
		if (frameData.fVideoBuffer)
			mDevice.DMABufferLock(frameData.fVideoBuffer, true);
		if (frameData.fAudioBuffer)
			mDevice.DMABufferLock(frameData.fAudioBuffer, true);
		if (frameData.fAncBuffer)
			mDevice.DMABufferLock(frameData.fAncBuffer, true);
	}	//	for each NTV2FrameData

}	//	SetupHostBuffers


bool NTV2Capture8K::RouteInputSignal (void)
{
	NTV2XptConnections connections;
	return CNTV2DemoCommon::GetInputRouting8K (connections, mConfig, mVideoFormat)
		&&  mDevice.ApplySignalRoute(connections, !mConfig.fDoMultiFormat);

}	//	RouteInputSignal


AJAStatus NTV2Capture8K::Run (void)
{
	//	Start the playout and capture threads...
	StartConsumerThread();
	StartProducerThread();
	return AJA_STATUS_SUCCESS;

}	//	Run


//////////////////////////////////////////////////////////////////////////////////////////////////////////

//	This starts the consumer thread
void NTV2Capture8K::StartConsumerThread (void)
{
	//	Create and start the consumer thread...
	mConsumerThread.Attach(ConsumerThreadStatic, this);
	mConsumerThread.SetPriority(AJA_ThreadPriority_High);
	mConsumerThread.Start();

}	//	StartConsumerThread


//	The consumer thread function
void NTV2Capture8K::ConsumerThreadStatic (AJAThread * pThread, void * pContext)		//	static
{
	(void) pThread;

	//	Grab the NTV2Capture instance pointer from the pContext parameter,
	//	then call its ConsumeFrames method...
	NTV2Capture8K *	pApp (reinterpret_cast<NTV2Capture8K*>(pContext));
	pApp->ConsumeFrames();

}	//	ConsumerThreadStatic


void NTV2Capture8K::ConsumeFrames (void)
{
	CAPNOTE("Thread started");
	while (!mGlobalQuit)
	{
		//	Wait for the next frame to become ready to "consume"...
		NTV2FrameData *	pFrameData(mAVCircularBuffer.StartConsumeNextBuffer());
		if (pFrameData)
		{
			//	Do something useful with the frame data...
			//	. . .		. . .		. . .		. . .
			//		. . .		. . .		. . .		. . .
			//			. . .		. . .		. . .		. . .

			//	Now release and recycle the buffer...
			mAVCircularBuffer.EndConsumeNextBuffer();
		}	//	if pFrameData
	}	//	loop til quit signaled
	CAPNOTE("Thread completed, will exit");

}	//	ConsumeFrames


//////////////////////////////////////////////////////////////////////////////////////////////////////////

//	This starts the capture (producer) thread
void NTV2Capture8K::StartProducerThread (void)
{
	//	Create and start the capture thread...
	mProducerThread.Attach(ProducerThreadStatic, this);
	mProducerThread.SetPriority(AJA_ThreadPriority_High);
	mProducerThread.Start();

}	//	StartProducerThread


//	The capture thread function
void NTV2Capture8K::ProducerThreadStatic (AJAThread * pThread, void * pContext)		//	static
{
	(void) pThread;

	//	Grab the NTV2Capture instance pointer from the pContext parameter,
	//	then call its CaptureFrames method...
	NTV2Capture8K *	pApp (reinterpret_cast<NTV2Capture8K*>(pContext));
	pApp->CaptureFrames();

}	//	ProducerThreadStatic


void NTV2Capture8K::CaptureFrames (void)
{
	AUTOCIRCULATE_TRANSFER	inputXfer;	//	AutoCirculate input transfer info
	CAPNOTE("Thread started");

	//	Tell capture AutoCirculate to use frame buffers 0 thru 6 (7 frames) on the device...
	mConfig.fFrames.setExactRange (0, 6);
	mDevice.AutoCirculateStop(mActiveFrameStores);	//	Just in case
	if (!mDevice.AutoCirculateInitForInput (mConfig.fInputChannel,		//	primary channel
											mConfig.fFrames.count(),	//	numFrames (zero if exact range)
											mAudioSystem,				//	audio system (if any)
											AUTOCIRCULATE_WITH_RP188 | AUTOCIRCULATE_WITH_ANC,	//	AutoCirculate options
											1,							//	numChannels to gang
											mConfig.fFrames.firstFrame(), mConfig.fFrames.lastFrame()))
		mGlobalQuit = true;
	if (!mGlobalQuit  &&  !mDevice.AutoCirculateStart(mConfig.fInputChannel))
		mGlobalQuit = true;

	//	Ingest frames til Quit signaled...
	while (!mGlobalQuit)
	{
		AUTOCIRCULATE_STATUS acStatus;
		mDevice.AutoCirculateGetStatus (mConfig.fInputChannel, acStatus);

		if (acStatus.IsRunning()  &&  acStatus.HasAvailableInputFrame())
		{
			//	At this point, there's at least one fully-formed frame available in the device's
			//	frame buffer to transfer to the host. Reserve an NTV2FrameData to "produce", and
			//	use it in the next transfer from the device...
			NTV2FrameData *	pFrameData (mAVCircularBuffer.StartProduceNextBuffer());
			if (!pFrameData)
				continue;

			NTV2FrameData & frameData (*pFrameData);
			inputXfer.SetVideoBuffer (frameData.VideoBuffer(), frameData.VideoBufferSize());
			if (acStatus.WithAudio())
				inputXfer.SetAudioBuffer (frameData.AudioBuffer(), frameData.AudioBufferSize());
			if (acStatus.WithCustomAnc())
				inputXfer.SetAncBuffers (frameData.AncBuffer(), frameData.AncBufferSize(),
										frameData.AncBuffer2(), frameData.AncBuffer2Size());

			//	Transfer video/audio/anc from the device into our host buffers...
			mDevice.AutoCirculateTransfer (mConfig.fInputChannel, inputXfer);

			//	Remember the actual amount of audio captured...
			if (acStatus.WithAudio())
				frameData.fNumAudioBytes = inputXfer.GetCapturedAudioByteCount();

			//	Grab all valid timecodes that were captured...
			inputXfer.GetInputTimeCodes (frameData.fTimecodes, mConfig.fInputChannel, /*ValidOnly*/ true);

			//	Signal that we're done "producing" the frame, making it available for future "consumption"...
			mAVCircularBuffer.EndProduceNextBuffer();
		}	//	if A/C running and frame(s) are available for transfer
		else
		{
			//	Either AutoCirculate is not running, or there were no frames available on the device to transfer.
			//	Rather than waste CPU cycles spinning, waiting until a frame becomes available, it's far more
			//	efficient to wait for the next input vertical interrupt event to get signaled...
			mDevice.WaitForInputVerticalInterrupt (mConfig.fInputChannel);
		}
	}	//	loop til quit signaled

	//	Stop AutoCirculate...
	mDevice.AutoCirculateStop (mConfig.fInputChannel);
	CAPNOTE("Thread completed, will exit");

}	//	CaptureFrames


void NTV2Capture8K::GetACStatus (ULWord & outGoodFrames, ULWord & outDroppedFrames, ULWord & outBufferLevel)
{
	AUTOCIRCULATE_STATUS status;
	mDevice.AutoCirculateGetStatus(mConfig.fInputChannel, status);
	outGoodFrames    = status.GetProcessedFrameCount();
	outDroppedFrames = status.GetDroppedFrameCount();
	outBufferLevel   = status.GetBufferLevel();
}
