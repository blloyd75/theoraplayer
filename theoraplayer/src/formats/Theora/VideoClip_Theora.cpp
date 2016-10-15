/// @file
/// @version 2.0
/// 
/// @section LICENSE
/// 
/// This program is free software; you can redistribute it and/or modify it under
/// the terms of the BSD license: http://opensource.org/licenses/BSD-3-Clause

#ifdef _USE_THEORA
#include <algorithm>
#include <memory.h>
#include <string>
#include <map>

#include "AudioInterface.h"
#include "AudioInterfaceFactory.h"
#include "DataSource.h"
#include "Exception.h"
#include "FrameQueue.h"
#include "Manager.h"
#include "Mutex.h"
#include "PixelTransform.h"
#include "Timer.h"
#include "theoraplayer.h"
#include "Utility.h"
#include "VideoClip_Theora.h"
#include "VideoFrame.h"

#define BUFFER_SIZE 4096

namespace theoraplayer
{

	TheoraVorbisInfoStruct::TheoraVorbisInfoStruct()
	{
		memset(&VorbisStreamState, 0, sizeof(ogg_stream_state));
		memset(&VorbisInfo, 0, sizeof(vorbis_info));
		memset(&VorbisDSPState, 0, sizeof(vorbis_dsp_state));
		memset(&VorbisBlock, 0, sizeof(vorbis_block));
		memset(&VorbisComment, 0, sizeof(vorbis_comment));
		vorbis_info_init(&VorbisInfo);
		vorbis_comment_init(&VorbisComment);
		streamOrder = 0;
		vorbisHeaders = 0;

	}
	TheoraVorbisInfoStruct::~TheoraVorbisInfoStruct()
	{
		ogg_stream_clear(&VorbisStreamState);
		vorbis_comment_clear(&VorbisComment);
		vorbis_info_clear(&VorbisInfo);
	}


	VideoClip_Theora::VideoClip_Theora(DataSource* data_source, OutputMode output_mode, int nPrecachedFrames, bool usePower2Stride) :
		VideoClip(data_source, output_mode, nPrecachedFrames, usePower2Stride),
		AudioPacketQueue()
	{
		this->info.VorbisData = NULL;
		this->info.TheoraDecoder = NULL;
		this->info.TheoraSetup = NULL;
		this->theoraStreams = 0;
		this->readAudioSamples = 0;
		this->lastDecodedFrameNumber = 0;
	}

	VideoClip* VideoClip_Theora::create(DataSource* dataSource, OutputMode outputMode, int precachedFramesCount, bool usePotStride)
	{
		return new VideoClip_Theora(dataSource, outputMode, precachedFramesCount, usePotStride);
	}

	VideoClip_Theora::~VideoClip_Theora()
	{
		if (this->info.TheoraDecoder != NULL)
		{
			th_decode_free(this->info.TheoraDecoder);
			th_setup_free(this->info.TheoraSetup);
			if ((this->audioInterface != NULL) && (this->info.VorbisData != NULL))
			{
				vorbis_dsp_clear(&this->info.VorbisData->VorbisDSPState);
				vorbis_block_clear(&this->info.VorbisData->VorbisBlock);
			}
			ogg_stream_clear(&this->info.TheoraStreamState);
			th_comment_clear(&this->info.TheoraComment);
			th_info_clear(&this->info.TheoraInfo);
			delete this->info.VorbisData;
			this->info.VorbisData = NULL;
			ogg_sync_clear(&this->info.OggSyncState);
		}
	}

	void VideoClip_Theora::_load(DataSource* source)
	{
#ifdef _DEBUG
		log("-----");
#endif
		this->stream = source;
		this->_readTheoraVorbisHeaders();
		this->info.TheoraDecoder = th_decode_alloc(&this->info.TheoraInfo, this->info.TheoraSetup);
		this->width = this->info.TheoraInfo.frame_width;
		this->height = this->info.TheoraInfo.frame_height;
		this->subFrameWidth = this->info.TheoraInfo.pic_width;
		this->subFrameHeight = this->info.TheoraInfo.pic_height;
		this->subFrameX = this->info.TheoraInfo.pic_x;
		this->subFrameY = this->info.TheoraInfo.pic_y;
		this->stride = this->getWidth();
		if (this->useStride)
		{
			this->stride = potCeil(this->stride);
		}
		this->fps = this->info.TheoraInfo.fps_numerator / (float)this->info.TheoraInfo.fps_denominator;
#ifdef _DEBUG
		log("width: " + str(this->width) + ", height: " + str(this->height) + ", fps: " + str((int)this->getFps()));
#endif
		this->frameQueue = new FrameQueue(this);
		this->frameQueue->setSize(this->precachedFramesCount);
		// find out the duration of the file by seeking to the end
		// having ogg decode pages, extract the granule pos from
		// the last theora page and seek back to beginning of the file
		char* buffer = NULL;
		int bytesRead = 0;
		uint64_t streamSize = this->stream->getSize();
		uint64_t seekPos = 0;
		int result = 0;
		ogg_int64_t granule = 0;
		for (unsigned int i = 1; i <= 50; ++i)
		{
			ogg_sync_reset(&this->info.OggSyncState);
			seekPos = (BUFFER_SIZE * i > streamSize ? 0 : streamSize - BUFFER_SIZE * i);
			this->stream->seek(seekPos);
			buffer = ogg_sync_buffer(&this->info.OggSyncState, BUFFER_SIZE * i);
			bytesRead = this->stream->read(buffer, BUFFER_SIZE * i);
			ogg_sync_wrote(&this->info.OggSyncState, bytesRead);
			ogg_sync_pageseek(&this->info.OggSyncState, &this->info.OggPage);
			while (true)
			{
				result = ogg_sync_pageout(&this->info.OggSyncState, &this->info.OggPage);
				if (result == 0)
				{
					break;
				}
				// if page is not a theora page or page is unsynced(-1), skip it
				if (result == -1 || ogg_page_serialno(&this->info.OggPage) != this->info.TheoraStreamState.serialno)
				{
					continue;
				}
				granule = ogg_page_granulepos(&this->info.OggPage);
				if (granule >= 0)
				{
					this->framesCount = (int)th_granule_frame(this->info.TheoraDecoder, granule) + 1;
				}
				else if (this->framesCount > 0)
				{
					++this->framesCount; // append delta frames at the end to get the exact number
				}
			}
			if (this->framesCount > 0 || streamSize < BUFFER_SIZE * i)
			{
				break;
			}
		}
		if (this->framesCount < 0)
		{
			log("unable to determine file duration!");
		}
		else
		{
			this->duration = this->framesCount / this->fps;
#ifdef _DEBUG
			log("duration: " + strf(this->duration) + " seconds");
#endif
		}
		// restore to beginning of stream.
		ogg_sync_reset(&this->info.OggSyncState);
		this->stream->seek(0);
		if (this->info.VorbisData != NULL) // if there is no audio interface factory defined, even though the video clip might have audio, it will be ignored
		{
			vorbis_synthesis_init(&this->info.VorbisData->VorbisDSPState, &this->info.VorbisData->VorbisInfo);
			vorbis_block_init(&this->info.VorbisData->VorbisDSPState, &this->info.VorbisData->VorbisBlock);
			this->audioChannelsCount = this->info.VorbisData->VorbisInfo.channels;
			this->audioFrequency = (int) this->info.VorbisData->VorbisInfo.rate;
			// create an audio interface instance if available
			AudioInterfaceFactory* audioInterfaceFactory = theoraplayer::manager->getAudioInterfaceFactory();
			if (audioInterfaceFactory != NULL)
			{
				this->setAudioInterface(audioInterfaceFactory->createInstance(this, this->audioChannelsCount, this->audioFrequency));
			}
		}
		this->frameDuration = 1.0f / this->getFps();
#ifdef _DEBUG
		log("-----");
#endif
	}

	bool VideoClip_Theora::_readData()
	{
		int audioEos = 0;
		int serno = 0;
		float audioTime = 0;
		float time = this->timer->getTime();
		if (this->restarted)
		{
			time = 0.0f;
		}
		char* buffer = NULL;
		int bytesRead = 0;
		ogg_int64_t granule = 0;
		do
		{
			buffer = ogg_sync_buffer(&this->info.OggSyncState, BUFFER_SIZE);
			bytesRead = this->stream->read(buffer, BUFFER_SIZE);
			ogg_sync_wrote(&this->info.OggSyncState, bytesRead);
			if (bytesRead == 0)
			{
				if (!this->autoRestart)
				{
					this->endOfFile = true;
					log(this->name + " finished playing");
				}
				return false;
			}
			// when we fill the stream with enough pages, it'll start spitting out packets
			// which contain key frames, delta frames or audio data
			while (ogg_sync_pageout(&this->info.OggSyncState, &this->info.OggPage) > 0)
			{
				serno = ogg_page_serialno(&this->info.OggPage);
				if (serno == this->info.TheoraStreamState.serialno)
				{
					ogg_stream_pagein(&this->info.TheoraStreamState, &this->info.OggPage);
				}
				if (this->audioInterface != NULL && this->info.VorbisData != NULL && serno == this->info.VorbisData->VorbisStreamState.serialno)
				{
					granule = ogg_page_granulepos(&this->info.OggPage);
					audioTime = (float)vorbis_granule_time(&this->info.VorbisData->VorbisDSPState, granule);
					audioEos = ogg_page_eos(&this->info.OggPage);
					ogg_stream_pagein(&this->info.VorbisData->VorbisStreamState, &this->info.OggPage);
				}
			}
		} while (this->audioInterface != NULL && this->info.VorbisData != NULL && audioEos == 0 && audioTime < time + 1.0f);
		return true;
	}

	bool VideoClip_Theora::_decodeNextFrame()
	{
		if (this->endOfFile)
		{
			return false;
		}
		VideoFrame* frame = this->frameQueue->requestEmptyFrame();
		if (frame == NULL)
		{
			return false; // max number of precached frames reached
		}
		bool shouldRestart = false;
		ogg_packet opTheora;
		ogg_int64_t granulePos;
		th_ycbcr_buffer buff;
		int result = 0;
		int attempts = 0;
		int status = 0;
		float time = 0.0f;
		unsigned long frameNumber = 0;
		while (true)
		{
			// ogg_stream_packetout can return -1 and the official docs suggest to do subsequent calls until it succeeds
			// because the data is out of sync. still will limit the number of attempts just in case
			for (result = -1, attempts = 0; result < 0 && attempts < 100; ++attempts)
			{
				result = ogg_stream_packetout(&this->info.TheoraStreamState, &opTheora);
			}
			if (result > 0)
			{
				status = th_decode_packetin(this->info.TheoraDecoder, &opTheora, &granulePos);
				if (status != 0 && status != TH_DUPFRAME) // 0 means success
				{
					continue;
				}
				time = (float)th_granule_time(this->info.TheoraDecoder, granulePos);
				frameNumber = (unsigned long)th_granule_frame(this->info.TheoraDecoder, granulePos);
				// %16 operation is here to prevent a playback halt during video playback if the decoder can't keep up with demand.
				if (time < this->timer->getTime() && !this->restarted && frameNumber % 16 != 0)
				{
#ifdef _DEBUG_FRAMEDROP
					log(mName + ": pre-dropped frame " + str((int)frameNumber));
#endif
					++this->droppedFramesCount;
					continue; // drop frame
				}
				this->_setVideoFrameTimeToDisplay(frame, time - this->frameDuration);
				this->_setVideoFrameIteration(frame, this->iteration);
				this->_setVideoFrameFrameNumber(frame, (int)frameNumber);
				this->lastDecodedFrameNumber = frameNumber;
				th_decode_ycbcr_out(this->info.TheoraDecoder, buff);
				Theoraplayer_PixelTransform pixelTransform;
				memset(&pixelTransform, 0, sizeof(Theoraplayer_PixelTransform));
				pixelTransform.y = buff[0].data;	pixelTransform.yStride = buff[0].stride;
				pixelTransform.u = buff[1].data;	pixelTransform.uStride = buff[1].stride;
				pixelTransform.v = buff[2].data;	pixelTransform.vStride = buff[2].stride;
				frame->decode(&pixelTransform);
				break;
			}
			if (!this->_readData())
			{
				this->_setVideoFrameInUse(frame, false);
				shouldRestart = this->autoRestart;
				break;
			}
		}
		if (this->audioInterface != NULL)
		{
			Mutex::ScopeLock lock(this->audioMutex);
			this->_decodeAudio();
		}
		if (shouldRestart)
		{
			++this->iteration;
			this->_executeRestart();
		}
		return true;
	}

	float VideoClip_Theora::_decodeAudio()
	{
		if (this->restarted)
		{
			return -1.0f;
		}
		ogg_packet opVorbis;
		float** pcm;
		int length = 0;
		float timeStamp = -1.0f;
		bool readPastTimestamp = false;
		float factor = 1.0f / this->audioFrequency;
		float videoTime = (float)this->lastDecodedFrameNumber / this->fps;
		float min = this->frameQueue->getSize() / this->fps + 1.0f;
		float audioTime = 0.0f;
		while (true)
		{
			length = vorbis_synthesis_pcmout(&this->info.VorbisData->VorbisDSPState, &pcm);
			if (length == 0)
			{
				if (ogg_stream_packetout(&this->info.VorbisData->VorbisStreamState, &opVorbis) > 0)
				{
					if (vorbis_synthesis(&this->info.VorbisData->VorbisBlock, &opVorbis) == 0)
					{
						if (timeStamp < 0 && opVorbis.granulepos >= 0)
						{
							timeStamp = (float)vorbis_granule_time(&this->info.VorbisData->VorbisDSPState, opVorbis.granulepos);
						}
						else if (timeStamp >= 0)
						{
							readPastTimestamp = true;
						}
						vorbis_synthesis_blockin(&this->info.VorbisData->VorbisDSPState, &this->info.VorbisData->VorbisBlock);
					}
					continue;
				}
				audioTime = this->readAudioSamples * factor;
				// always buffer up of audio ahead of the frames
				if (audioTime - videoTime >= min || !this->_readData())
				{
					break;
				}
			}
			if (length > 0)
			{
				this->addAudioPacket(pcm, length, this->audioGain);
				this->readAudioSamples += length;
				if (readPastTimestamp)
				{
					timeStamp += (float)length / this->info.VorbisData->VorbisInfo.rate;
				}
				vorbis_synthesis_read(&this->info.VorbisData->VorbisDSPState, length); // tell vorbis we read a number of samples
			}
		}
		return timeStamp;
	}

	void VideoClip_Theora::_decodedAudioCheck()
	{
		if (this->audioInterface != NULL && !this->timer->isPaused())
		{
			this->_flushSynchronizedAudioPackets(this->audioInterface, this->audioMutex);
		}
	}

	void VideoClip_Theora::_executeSeek()
	{
#if _DEBUG
		log(this->name + " [seek]: seeking to frame " + str(this->seekFrame));
#endif
		int frame = 0;
		float time = this->seekFrame / getFps();
		this->timer->seek(time);
		bool paused = this->timer->isPaused();
		if (!paused)
		{
			this->timer->pause(); // pause until seeking is done
		}
		this->endOfFile = false;
		this->restarted = false;
		this->_resetFrameQueue();
		// reset the video decoder.
		ogg_stream_reset(&this->info.TheoraStreamState);
		th_decode_free(this->info.TheoraDecoder);
		this->info.TheoraDecoder = th_decode_alloc(&this->info.TheoraInfo, this->info.TheoraSetup);
		Mutex::ScopeLock audioMutexLock;
		if (this->audioInterface != NULL)
		{
			audioMutexLock.acquire(this->audioMutex);
			ogg_stream_reset(&this->info.VorbisData->VorbisStreamState);
			vorbis_synthesis_restart(&this->info.VorbisData->VorbisDSPState);
			this->destroyAllAudioPackets();
		}
		// first seek to desired frame, then figure out the location of the
		// previous key frame and seek to it.
		// then by setting the correct time, the decoder will skip N frames untill
		// we get the frame we want.
		frame = (int)this->_seekPage(this->seekFrame, 1); // find the key frame nearest to the target frame
#ifdef _DEBUG
	//		log(mName + " [seek]: nearest key frame for frame " + str(mSeekFrame) + " is frame: " + str(frame));
#endif
		this->_seekPage(std::max(0, frame - 1), 0);

		ogg_packet opTheora;
		ogg_int64_t granulePos;
		bool granuleSet = false;
		if (frame <= 1)
		{
			if (this->info.TheoraInfo.version_major == 3 && this->info.TheoraInfo.version_minor == 2 && this->info.TheoraInfo.version_subminor == 0)
			{
				granulePos = 0;
			}
			else
			{
				granulePos = 1; // because of difference in granule interpretation in theora streams 3.2.0 and newer ones
			}
			th_decode_ctl(this->info.TheoraDecoder, TH_DECCTL_SET_GRANPOS, &granulePos, sizeof(granulePos));
			granuleSet = true;
		}
		// now that we've found the key frame that preceeds our desired frame, lets keep on decoding frames until we
		// reach our target frame.
		int status = 0;
		while (this->seekFrame != 0)
		{
			if (ogg_stream_packetout(&this->info.TheoraStreamState, &opTheora) > 0)
			{
				if (!granuleSet)
				{
					// theora decoder requires to set the granule pos after seek to be able to determine the current frame
					if (opTheora.granulepos < 0)
					{
						continue; // ignore prev delta frames until we hit a key frame
					}
					th_decode_ctl(this->info.TheoraDecoder, TH_DECCTL_SET_GRANPOS, &opTheora.granulepos, sizeof(opTheora.granulepos));
					granuleSet = true;
				}
				status = th_decode_packetin(this->info.TheoraDecoder, &opTheora, &granulePos);
				if (status != 0 && status != TH_DUPFRAME)
				{
					continue;
				}
				frame = (int)th_granule_frame(this->info.TheoraDecoder, granulePos);
				if (frame >= this->seekFrame - 1)
				{
					break;
				}
			}
			else if (!this->_readData())
			{
				log(this->name + " [seek]: fineseeking failed, _readData failed!");
				return;
			}
		}
#ifdef _DEBUG
		//	log(mName + " [seek]: fineseeked to frame " + str(frame + 1) + ", requested: " + str(mSeekFrame));
#endif
		if (this->audioInterface != NULL)
		{
			// read audio data until we reach a timeStamp. this usually takes only one iteration, but just in case let's
			// wrap it in a loop
			float timeStamp = 0.0f;
			while (true)
			{
				timeStamp = this->_decodeAudio();
				if (timeStamp >= 0)
				{
					break;
				}
				this->_readData();
			}
			float rate = (float)this->audioFrequency * this->audioChannelsCount;
			float queuedTime = this->getAudioPacketQueueLength();
			int trimmedCount = 0;
			// at this point there are only 2 possibilities: either we have too much packets and we have to delete
			// the first N ones, or we don't have enough, so let's fill the gap with silence.
			if (time > timeStamp - queuedTime)
			{
				while (this->audioPacketQueue != NULL)
				{
					if (time <= timeStamp - queuedTime + this->audioPacketQueue->samplesCount / rate)
					{
						trimmedCount = (int)((timeStamp - queuedTime + this->audioPacketQueue->samplesCount / rate - time) * rate);
						if (this->audioPacketQueue->samplesCount - trimmedCount <= 0)
						{
							this->destroyAudioPacket(this->popAudioPacket()); // if there's no data to be left, just destroy it
						}
						else
						{
							for (int i = trimmedCount, j = 0; i < this->audioPacketQueue->samplesCount; ++i, ++j)
							{
								this->audioPacketQueue->pcmData[j] = this->audioPacketQueue->pcmData[i];
							}
							this->audioPacketQueue->samplesCount -= trimmedCount;
						}
						break;
					}
					queuedTime -= this->audioPacketQueue->samplesCount / rate;
					this->destroyAudioPacket(this->popAudioPacket());
				}
			}
			// expand the first packet with silence.
			else if (this->audioPacketQueue != NULL)
			{
				int i = 0;
				int j = 0;
				int missingCount = (int)((timeStamp - queuedTime - time) * rate);
				if (missingCount > 0)
				{
					float* samples = new float[missingCount + this->audioPacketQueue->samplesCount];
					if (missingCount > 0)
					{
						memset(samples, 0, missingCount * sizeof(float));
					}
					for (j = 0; i < missingCount + this->audioPacketQueue->samplesCount; ++i, ++j)
					{
						samples[i] = this->audioPacketQueue->pcmData[j];
					}
					delete[] this->audioPacketQueue->pcmData;
					this->audioPacketQueue->pcmData = samples;
				}
			}
			this->lastDecodedFrameNumber = this->seekFrame;
			this->readAudioSamples = (unsigned int)(timeStamp * this->audioFrequency);
			audioMutexLock.release();
		}
		if (!paused)
		{
			this->timer->play();
		}
		this->seekFrame = -1;
	}

	void VideoClip_Theora::_executeRestart()
	{
		bool paused = this->timer->isPaused();
		if (!paused)
		{
			this->timer->pause();
		}
		long initialGranule = 0;
		th_decode_ctl(this->info.TheoraDecoder, TH_DECCTL_SET_GRANPOS, &initialGranule, sizeof(initialGranule));
		th_decode_free(this->info.TheoraDecoder);
		this->info.TheoraDecoder = th_decode_alloc(&this->info.TheoraInfo, this->info.TheoraSetup);
		ogg_stream_reset(&this->info.TheoraStreamState);
		if (this->audioInterface != NULL)
		{
			// empty the DSP buffer
			ogg_packet opVorbis;
			this->readAudioSamples = 0;
			while (ogg_stream_packetout(&this->info.VorbisData->VorbisStreamState, &opVorbis) > 0)
			{
				if (vorbis_synthesis(&this->info.VorbisData->VorbisBlock, &opVorbis) == 0)
				{
					vorbis_synthesis_blockin(&this->info.VorbisData->VorbisDSPState, &this->info.VorbisData->VorbisBlock);
				}
			}
			ogg_stream_reset(&this->info.VorbisData->VorbisStreamState);
		}
		ogg_sync_reset(&this->info.OggSyncState);
		this->stream->seek(0);
		ogg_int64_t granulePos = 0;
		th_decode_ctl(this->info.TheoraDecoder, TH_DECCTL_SET_GRANPOS, &granulePos, sizeof(granulePos));
		this->endOfFile = false;
		this->restarted = true;
		if (!paused)
		{
			this->timer->play();
		}
	}

	long VideoClip_Theora::_seekPage(long targetFrame, bool returnKeyFrame)
	{
		uint64_t seekMin = 0;
		uint64_t seekMax = this->stream->getSize();
		long frame = 0;
		ogg_int64_t granule = 0;
		if (targetFrame == 0)
		{
			this->stream->seek(0);
		}
		char* buffer = NULL;
		int bytesRead = 0;
		if (targetFrame != 0)
		{
			for (int i = 0; i < 100; ++i)
			{
				ogg_sync_reset(&this->info.OggSyncState);
				this->stream->seek(seekMin / 2 + seekMax / 2); // do a binary search
				memset(&this->info.OggPage, 0, sizeof(ogg_page));
				ogg_sync_pageseek(&this->info.OggSyncState, &this->info.OggPage);
				while (i < 1000)
				{
					if (ogg_sync_pageout(&this->info.OggSyncState, &this->info.OggPage) == 1)
					{
						if (ogg_page_serialno(&this->info.OggPage) == this->info.TheoraStreamState.serialno)
						{
							granule = ogg_page_granulepos(&this->info.OggPage);
							if (granule >= 0)
							{
								frame = (long)th_granule_frame(this->info.TheoraDecoder, granule);
								if (frame < targetFrame && targetFrame - frame < 10)
								{
									// we're close enough, let's break this.
									i = 1000;
									break;
								}
								// we're not close enough, let's shorten the borders of the binary search
								if (targetFrame - 1 > frame)
								{
									seekMin = seekMin / 2 + seekMax / 2;
								}
								else
								{
									seekMax = seekMin / 2 + seekMax / 2;
								}
								break;
							}
						}
					}
					else
					{
						buffer = ogg_sync_buffer(&this->info.OggSyncState, BUFFER_SIZE);
						bytesRead = this->stream->read(buffer, BUFFER_SIZE);
						if (bytesRead == 0)
						{
							break;
						}
						ogg_sync_wrote(&this->info.OggSyncState, bytesRead);
					}
				}
			}
		}
		if (returnKeyFrame)
		{
			return (long)(granule >> this->info.TheoraInfo.keyframe_granule_shift);
		}
		ogg_sync_reset(&this->info.OggSyncState);
		memset(&this->info.OggPage, 0, sizeof(ogg_page));
		ogg_sync_pageseek(&this->info.OggSyncState, &this->info.OggPage);
		if (targetFrame == 0)
		{
			return -1;
		}
		this->stream->seek((seekMin + seekMax) / 2); // do a binary search
		return -1;
	}

	static std::string findlanguage(const vorbis_comment& comments)
	{
		for (int x = 0; x < comments.comments; ++x)
		{
			if (strncmp("LANGUAGE=", comments.user_comments[x], 9) == 0)
				return comments.user_comments[x] + 9;
		}
		return std::string();
	}

	void VideoClip_Theora::_readTheoraVorbisHeaders()
	{
		std::map<long, TheoraVorbisInfoStruct*> audioStreams;
		TheoraVorbisInfoStruct* vorbinfo=NULL;
		try {
		ogg_packet tempOggPacket;
		//init Vorbis/Theora Layer
		//Ensure all structures get cleared out.
		memset(&this->info.OggSyncState, 0, sizeof(ogg_sync_state));
		memset(&this->info.OggPage, 0, sizeof(ogg_page));
		memset(&this->info.TheoraStreamState, 0, sizeof(ogg_stream_state));
		memset(&this->info.TheoraInfo, 0, sizeof(th_info));
		memset(&this->info.TheoraComment, 0, sizeof(th_comment));
		this->info.VorbisData = NULL;
		// init all structures
		ogg_sync_init(&this->info.OggSyncState);
		th_comment_init(&this->info.TheoraComment);
		th_info_init(&this->info.TheoraInfo);
		// start
		ogg_stream_state oggStateTest;
		bool decodeAudio = (theoraplayer::manager->getAudioInterfaceFactory() != NULL);
		char* buffer = NULL;
		int bytesRead = 0;
		bool done = false;
		vorbinfo =new TheoraVorbisInfoStruct();
		while (!done)
		{
			buffer = ogg_sync_buffer(&this->info.OggSyncState, BUFFER_SIZE);
			bytesRead = this->stream->read(buffer, BUFFER_SIZE);
			ogg_sync_wrote(&this->info.OggSyncState, bytesRead);
			if (bytesRead == 0)
			{
				break;
			}
			while (ogg_sync_pageout(&this->info.OggSyncState, &this->info.OggPage) > 0)
			{
				memset(&oggStateTest, 0, sizeof(oggStateTest));
				//is this an initial header? If not, stop
				long serialnum = ogg_page_serialno(&this->info.OggPage);
				if (ogg_page_bos(&this->info.OggPage) == 0)
				{
					//This is done blindly, because stream only accept themselves
					if (this->theoraStreams > 0)
					{
						ogg_stream_pagein(&this->info.TheoraStreamState, &this->info.OggPage);
					}
					std::map<long, TheoraVorbisInfoStruct*>::iterator itr = audioStreams.find(serialnum);
					if (itr != audioStreams.end())
					{
						ogg_stream_pagein(&itr->second->VorbisStreamState, &this->info.OggPage);
					}
					done = true;
					break;
				}
				ogg_stream_init(&oggStateTest, serialnum);
				ogg_stream_pagein(&oggStateTest, &this->info.OggPage);
				ogg_stream_packetout(&oggStateTest, &tempOggPacket);
				// identify the codec
				if (this->theoraStreams == 0 && th_decode_headerin(&this->info.TheoraInfo, &this->info.TheoraComment, &this->info.TheoraSetup, &tempOggPacket) > 0)
				{
					// This is the Theora Header
					memcpy(&this->info.TheoraStreamState, &oggStateTest, sizeof(oggStateTest));
					this->theoraStreams = 1;
				}
				else if (decodeAudio && vorbis_synthesis_headerin(&vorbinfo->VorbisInfo, &vorbinfo->VorbisComment, &tempOggPacket) >= 0)
				{
					std::map<long, TheoraVorbisInfoStruct*>::iterator itr = audioStreams.find(serialnum);
					if (itr == audioStreams.end())
					{
						memcpy(&vorbinfo->VorbisStreamState, &oggStateTest, sizeof(oggStateTest));
						vorbinfo->vorbisHeaders = 1;
						vorbinfo->streamOrder = audioStreams.size();
						audioStreams[serialnum] = vorbinfo;
						vorbinfo = nullptr;
					}
					delete vorbinfo;
					vorbinfo = new TheoraVorbisInfoStruct();
				}
				else // Hm, guess it's not a header we support, so erase it
				{
					ogg_stream_clear(&oggStateTest);
				}
			}
		}
		int result = 0;
		done = false;
		while (!done)
		{
			done = true;
			// Check 2nd'dary headers... Theora First
			while (this->theoraStreams > 0 && this->theoraStreams < 3 && (result = ogg_stream_packetout(&this->info.TheoraStreamState, &tempOggPacket)))
			{
				if (result < 0)
				{
					throw TheoraplayerException("Error parsing Theora stream headers!");
				}
				if (th_decode_headerin(&this->info.TheoraInfo, &this->info.TheoraComment, &this->info.TheoraSetup, &tempOggPacket) == 0)
				{
					throw TheoraplayerException("Invalid theora stream!");
				}
				++this->theoraStreams;
			} // end while looking for more theora headers

			if (this->theoraStreams > 0 && this->theoraStreams < 3)
				done = false;

			// look 2nd vorbis header packets
			std::map<long, TheoraVorbisInfoStruct*>::iterator itr = audioStreams.begin();
			std::map<long, TheoraVorbisInfoStruct*>::iterator itr_e = audioStreams.end();
			while (itr != itr_e)
			{
				while (itr->second->vorbisHeaders < 3 && (result = ogg_stream_packetout(&itr->second->VorbisStreamState, &tempOggPacket)))
				{
					if (result < 0)
					{
						//throw TheoraplayerException("Error parsing vorbis stream headers!");
						break;
					}
					if (vorbis_synthesis_headerin(&itr->second->VorbisInfo, &itr->second->VorbisComment, &tempOggPacket) != 0)
					{
						//throw TheoraplayerException("Invalid stream!");
						break;
					}
					++itr->second->vorbisHeaders;
				} // end while looking for more vorbis headers
				if (itr->second->vorbisHeaders > 0 && itr->second->vorbisHeaders < 3)
					done = false;

				++itr;
			}

			// Not finished with Headers, get some more file data
			if (ogg_sync_pageout(&this->info.OggSyncState, &this->info.OggPage) > 0)
			{
				long serialnum = ogg_page_serialno(&this->info.OggPage);
				if (this->theoraStreams > 0)
				{
					ogg_stream_pagein(&this->info.TheoraStreamState, &this->info.OggPage);
				}
				itr = audioStreams.find(serialnum);
				if (itr != audioStreams.end())
				{
					ogg_stream_pagein(&itr->second->VorbisStreamState, &this->info.OggPage);
				}
			}
			else
			{
				buffer = ogg_sync_buffer(&this->info.OggSyncState, BUFFER_SIZE);
				bytesRead = this->stream->read(buffer, BUFFER_SIZE);
				ogg_sync_wrote(&this->info.OggSyncState, bytesRead);
				if (bytesRead == 0)
				{
					done = true;
					// Only treat a truncated video stream as a failure.  Fail more gracefully for bad audio stream(s)
					if (this->theoraStreams > 0 && this->theoraStreams < 3)
						throw TheoraplayerException("End of file found prematurely!");
				}
			}
		} // end while looking for all headers

		// Find the best audio source
		std::map<long, TheoraVorbisInfoStruct*>::iterator itr = audioStreams.begin();
		std::map<long, TheoraVorbisInfoStruct*>::iterator itr_e = audioStreams.end();
		std::string langPref = manager->getAudioLanguagePreference();
		int matchOrder = -1;
		AudioLanguageMatch curLangMatch = LANGUAGE_MATCH_NONE;
		while (itr != itr_e)
		{
			AudioLanguageMatch chk = LANGUAGE_MATCH_NONE;
			if (!langPref.empty())
			{
				std::string language=findlanguage(itr->second->VorbisComment);
				chk = checklanguage(langPref, language);
			}
			if (curLangMatch<chk || (curLangMatch==chk && matchOrder<itr->second->streamOrder))
			{
				delete this->info.VorbisData;
				this->info.VorbisData = itr->second;
				itr->second = NULL;
				curLangMatch = chk;
				matchOrder = this->info.VorbisData->streamOrder;
			}
			++itr;
		}
		//log("Vorbis Headers: " + str(mVorbisHeaders) + " Theora Headers : " + str(mTheoraHeaders));
		} catch (...) {
			delete vorbinfo;
			std::map<long, TheoraVorbisInfoStruct*>::iterator itr = audioStreams.begin();
			std::map<long, TheoraVorbisInfoStruct*>::iterator itr_e = audioStreams.end();
			while (itr != itr_e)
			{
				delete itr->second;
				itr->second=NULL;
				++itr;
			}
			throw;
		}
		delete vorbinfo;
		std::map<long, TheoraVorbisInfoStruct*>::iterator itr = audioStreams.begin();
		std::map<long, TheoraVorbisInfoStruct*>::iterator itr_e = audioStreams.end();
		while (itr != itr_e)
		{
			delete itr->second;
			itr->second=NULL;
			++itr;
		}
	}


}
#endif
