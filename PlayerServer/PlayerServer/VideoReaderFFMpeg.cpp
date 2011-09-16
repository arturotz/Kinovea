#pragma region License
/*
Copyright © Joan Charmant 2008-2009.
joan.charmant@gmail.com 
 
This file is part of Kinovea.

Kinovea is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License version 2 
as published by the Free Software Foundation.

Kinovea is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Kinovea. If not, see http://www.gnu.org/licenses/.

*/
#pragma endregion

#include <msclr\lock.h>
#include "VideoReaderFFMpeg.h"

using namespace System::Diagnostics;
using namespace System::Drawing;
using namespace System::Drawing::Drawing2D;
using namespace System::IO;
using namespace System::Runtime::InteropServices;
using namespace System::Collections::Generic;
using namespace System::Threading;
using namespace msclr;

using namespace Kinovea::Video::FFMpeg;

VideoReaderFFMpeg::VideoReaderFFMpeg()
{
	av_register_all();
	m_Locker = gcnew Object();
	m_ThreadCanceler = gcnew ThreadCanceler();
	VideoFrameDisposer^ disposer = gcnew VideoFrameDisposer(DisposeFrame);
	Cache = gcnew VideoFrameCache(disposer);
	DataInit();
}
VideoReaderFFMpeg::~VideoReaderFFMpeg()
{
	this->!VideoReaderFFMpeg();
}
VideoReaderFFMpeg::!VideoReaderFFMpeg()
{
	if(m_bIsLoaded) 
		Close();
}
OpenVideoResult VideoReaderFFMpeg::Open(String^ _filePath)
{
	OpenVideoResult result = Load(_filePath);
	DumpInfo();
	return result;
}
void VideoReaderFFMpeg::Close()
{
	// Unload the video and dispose unmanaged resources.
	if(!m_bIsLoaded)
		return;
	
	// Cache->Clear() will unblock the decoding thread which will then
	// notice that it has been cancelled.
	if(m_DecodingThread != nullptr && m_DecodingThread->IsAlive)
		m_ThreadCanceler->Cancel();
	
	Cache->Clear(); 
	DataInit();

	if(m_pCodecCtx != nullptr)
		avcodec_close(m_pCodecCtx);
	
	if(m_pFormatCtx != nullptr)
		av_close_input_file(m_pFormatCtx);
}
void VideoReaderFFMpeg::DataInit()
{
	m_bIsLoaded = false;
	m_iVideoStream = -1;
	m_iAudioStream = -1;
	m_iMetadataStream = -1;
	m_VideoInfo = VideoInfo::Empty;
	m_TimestampInfo = TimestampInfo::Empty;
	Cache->WorkingZone = VideoSection::Empty;
}
VideoSummary^ VideoReaderFFMpeg::ExtractSummary(String^ _filePath, int _thumbs, int _width)
{
	// Open the file and extract some info + a few thumbnails.
	
	OpenVideoResult loaded = Load(_filePath);
	if(loaded != OpenVideoResult::Success)
		return nullptr;

	bool isImage = m_VideoInfo.DurationTimeStamps == 1;
	int64_t durationMs = (int64_t)((m_VideoInfo.DurationTimeStamps / m_VideoInfo.AverageTimeStampsPerSeconds) * 1000.0);
	Size imageSize = m_VideoInfo.OriginalSize;
	bool hasKva = m_VideoInfo.HasKva;
	
	// Double check for companion file.
	if(!hasKva)
	{
		String^ kvaFile = String::Format("{0}\\{1}.kva", Path::GetDirectoryName(_filePath), Path::GetFileNameWithoutExtension(_filePath));
		hasKva = File::Exists(kvaFile);
	}

	// Read some frames (directly decode at small size).
	float fWidthRatio = (float)m_VideoInfo.OriginalSize.Width / _width;
	m_VideoInfo.DecodingSize.Width = _width;
	m_VideoInfo.DecodingSize.Height = (int)(m_VideoInfo.OriginalSize.Height / fWidthRatio);
			
	List<Bitmap^>^ images = gcnew List<Bitmap^>();
	int64_t step = (int64_t)Math::Ceiling(m_VideoInfo.DurationTimeStamps / (double)_thumbs);
	int64_t lastFrame = -1;
	for(int64_t ts = 0;ts<m_VideoInfo.DurationTimeStamps;ts+=step)
	{
		ReadResult read = ReadResult::FrameNotRead;
		if(ts == 0)
			read = ReadFrame(-1, 1, true);
		else
			read = ReadFrame(ts, 1, true);

		if(read == ReadResult::Success && 
			Cache->MoveTo(m_TimestampInfo.CurrentTimestamp) && 
			Cache->Current != nullptr &&
			m_TimestampInfo.CurrentTimestamp > lastFrame)
		{
			Bitmap^ bmp = AForge::Imaging::Image::Clone(Cache->Current->Image, Cache->Current->Image->PixelFormat);
			images->Add(bmp);
			lastFrame = m_TimestampInfo.CurrentTimestamp;
			Cache->Clear();
		}
		else
		{
			Cache->Clear();
			break;
		}
	}
	
	Close();
	return gcnew VideoSummary(isImage, hasKva, imageSize, durationMs, images);
}
bool VideoReaderFFMpeg::MoveNext(bool _async)
{
	if(!m_bIsLoaded)
		return false;

	if(!_async && !Cache->HasNext)
		ReadFrame(-1, 1, false);

	Cache->MoveNext();
	bool hasMore = Cache->Current == nullptr || Cache->Current->Timestamp < Cache->WorkingZone.End;
	return hasMore;
}
bool VideoReaderFFMpeg::MoveTo(int64_t _timestamp, bool _async)
{
	if(!m_bIsLoaded)
		return false;
	
	int64_t target = _timestamp;
	if(!_async && !Cache->Contains(_timestamp))
	{
		bool decoding = m_DecodingThread != nullptr && m_DecodingThread->IsAlive;
		if(decoding)
			m_ThreadCanceler->Cancel();
		
		// Out of segment jump. Unless it's a rollover, we can't keep the segment contiguous.
		// If it's not, we still need to properly unblock the decoding thread by removing a frame from the buffer,
		// so it can evaluate the cancellation flag and exit nicely (and make room for the frame to read).
		if(!Cache->IsRolloverJump(_timestamp))
		{
			log->DebugFormat("Out of segment jump, clear cache.");	
			Cache->Clear();
		}
		else
		{
			log->DebugFormat("RolloverJump, unblock decoding thread to cancel it");
			Cache->RemoveOldest();
		}

		ReadFrame(_timestamp, 1, false);
		target = m_TimestampInfo.CurrentTimestamp;
		
		if(decoding)
			StartAsyncDecoding();
	}

	Cache->MoveTo(target);
	bool hasMore = Cache->Current == nullptr || Cache->Current->Timestamp < Cache->WorkingZone.End;
	return hasMore;
}
String^ VideoReaderFFMpeg::ReadMetadata()
{
	if(m_iMetadataStream < 0)
		return "";
	
	String^ metadata = "";
	bool done = false;
	do
	{
		AVPacket InputPacket;
		if((av_read_frame( m_pFormatCtx, &InputPacket)) < 0)
			break;
		
		if(InputPacket.stream_index != m_iMetadataStream)
			continue;

		metadata = gcnew String((char*)InputPacket.data);
		done = true;
	}
	while(!done);
	
	// Back to start.
	avformat_seek_file(m_pFormatCtx, m_iVideoStream, 0, 0, 0, AVSEEK_FLAG_BACKWARD); 
	
	return metadata;
}

bool VideoReaderFFMpeg::ChangeAspectRatio(ImageAspectRatio _ratio)
{
	// Decoding thread should be stopped at this point.
	Options->ImageAspectRatio = _ratio;
	SetDecodingSize(_ratio);
	Cache->Clear();
	return true;
}
bool VideoReaderFFMpeg::ChangeDeinterlace(bool _deint)
{
	// Decoding thread should be stopped at this point.
	Options->Deinterlace = _deint;
	Cache->Clear();
	return true;
}
bool VideoReaderFFMpeg::CanCacheWorkingZone(VideoSection _newZone, int _maxSeconds, int _maxMemory)
{
	double durationSeconds = (double)(_newZone.End - _newZone.Start) / m_VideoInfo.AverageTimeStampsPerSeconds;

	int64_t frameBytes = avpicture_get_size(m_PixelFormatFFmpeg, m_VideoInfo.DecodingSize.Width, m_VideoInfo.DecodingSize.Height);
	double frameMegaBytes = (double)frameBytes / 1048576;
	double durationMegaBytes = durationSeconds * m_VideoInfo.FramesPerSeconds * frameMegaBytes;
	
	return durationSeconds > 0 && durationSeconds <= _maxSeconds && durationMegaBytes <= _maxMemory;
}
bool VideoReaderFFMpeg::ReadMany(BackgroundWorker^ _bgWorker, VideoSection _section, bool _prepend)
{
	// Load the asked section to cache (doesn't move the playhead).
	// Called when filling the cache with the Working Zone.

	Thread::CurrentThread->Name = "Caching";
	log->DebugFormat("Caching section {0}, prepend:{1}", _section, _prepend);

	Cache->SetPrependBlock(_prepend);
	Cache->DisableCapacityCheck();
	
	bool success = true;
	int read = 0;
	int total = (int)((_section.End - _section.Start + m_VideoInfo.AverageTimeStampsPerFrame)/m_VideoInfo.AverageTimeStampsPerFrame);
	
	ReadResult res = ReadFrame(_section.Start, 1, false);
	while(m_TimestampInfo.CurrentTimestamp < _section.End && read < total && res == ReadResult::Success)
	{
		if(_bgWorker->CancellationPending)
        {
			log->DebugFormat("Cancellation at frame [{0}]", m_TimestampInfo.CurrentTimestamp);
			Cache->Clear();
			success = false;
            break;
        }
		
		res = ReadFrame(-1, 1, false);
		success = res == ReadResult::Success;
		_bgWorker->ReportProgress(read++, total);
	}

	Cache->SetPrependBlock(false);
	return success;
}
void VideoReaderFFMpeg::StartAsyncDecoding()
{
	if(Caching)
		return;

	// TODO: Check if we are already started, in which case cancel and clear.
	log->Debug("Starting decoding thread.");
	ParameterizedThreadStart^ pts = gcnew ParameterizedThreadStart(this, &VideoReaderFFMpeg::Prefetch);
	m_DecodingThread = gcnew Thread(pts);
	m_ThreadCanceler->Reset();
	m_DecodingThread->Start(m_ThreadCanceler);
}
void VideoReaderFFMpeg::CancelAsyncDecoding()
{
	m_ThreadCanceler->Cancel();
}
OpenVideoResult VideoReaderFFMpeg::Load(String^ _filePath)
{
	OpenVideoResult result = OpenVideoResult::Success;

	if(m_bIsLoaded) 
		Close();

	m_VideoInfo.FilePath = _filePath;
	if(Options == nullptr)
		Options = Options->Default;
	
	do
	{
		// Open file and get info on format (muxer).
		AVFormatContext* pFormatCtx = nullptr;
		char* pszFilePath = static_cast<char *>(Marshal::StringToHGlobalAnsi(_filePath).ToPointer());
		if(av_open_input_file(&pFormatCtx, pszFilePath , NULL, 0, NULL) != 0)
		{
			result = OpenVideoResult::FileNotOpenned;
			log->Debug("The file could not be openned. (Wrong path or not a video/image.)");
			break;
		}
		Marshal::FreeHGlobal(safe_cast<IntPtr>(pszFilePath));
		
		// Info on streams.
		if(av_find_stream_info(pFormatCtx) < 0 )
		{
			result = OpenVideoResult::StreamInfoNotFound;
			log->Debug("The streams Infos were not Found.");
			break;
		}
		
		// Check for muxed KVA.
		m_iMetadataStream = GetStreamIndex(pFormatCtx, AVMEDIA_TYPE_SUBTITLE);
		if(m_iMetadataStream >= 0)
		{
			AVMetadataTag* pMetadataTag = av_metadata_get(pFormatCtx->streams[m_iMetadataStream]->metadata, "language", nullptr, 0);

			if(pFormatCtx->streams[m_iMetadataStream]->codec->codec_id == CODEC_ID_TEXT &&
				pMetadataTag != nullptr &&
				strcmp((char*)pMetadataTag->value, "XML") == 0)
			{
				m_VideoInfo.HasKva = true;
			}
			else
			{
				log->Debug("Subtitle stream found, but not analysis meta data: ignored.");
				m_iMetadataStream = -1;
			}
		}

		// Video stream.
		if( (m_iVideoStream = GetStreamIndex(pFormatCtx, AVMEDIA_TYPE_VIDEO)) < 0 )
		{
			result = OpenVideoResult::VideoStreamNotFound;
			log->Debug("No Video stream found in the file. (File is audio only, or video stream is broken.)");
			break;
		}

		// Codec
		AVCodec* pCodec = nullptr;
		AVCodecContext* pCodecCtx = pFormatCtx->streams[m_iVideoStream]->codec;
		m_VideoInfo.IsCodecMpeg2 = (pCodecCtx->codec_id == CODEC_ID_MPEG2VIDEO);
		if( (pCodec = avcodec_find_decoder(pCodecCtx->codec_id)) == nullptr)
		{
			result = OpenVideoResult::CodecNotFound;
			log->Debug("No suitable codec to decode the video. (Worse than an unsupported codec.)");
			break;
		}

		if(avcodec_open(pCodecCtx, pCodec) < 0)
		{
			result = OpenVideoResult::CodecNotOpened;
			log->Debug("Codec could not be openned. (Codec known, but not supported yet.)");
			break;
		}

		// The fundamental unit of time in Kinovea is the timebase of the file.
		// The timebase is the unit of time (in seconds) in which the timestamps are represented.
		m_VideoInfo.AverageTimeStampsPerSeconds = (double)pFormatCtx->streams[m_iVideoStream]->time_base.den / (double)pFormatCtx->streams[m_iVideoStream]->time_base.num;
		double fAvgFrameRate = 0.0;
		if(pFormatCtx->streams[m_iVideoStream]->avg_frame_rate.den != 0)
			fAvgFrameRate = (double)pFormatCtx->streams[m_iVideoStream]->avg_frame_rate.num / (double)pFormatCtx->streams[m_iVideoStream]->avg_frame_rate.den;

		if(pFormatCtx->start_time > 0)
			m_VideoInfo.FirstTimeStamp = (int64_t)((double)((double)pFormatCtx->start_time / (double)AV_TIME_BASE) * m_VideoInfo.AverageTimeStampsPerSeconds);
		else
			m_VideoInfo.FirstTimeStamp = 0;
	
		if(pFormatCtx->duration > 0)
			m_VideoInfo.DurationTimeStamps = (int64_t)((double)((double)pFormatCtx->duration/(double)AV_TIME_BASE)*m_VideoInfo.AverageTimeStampsPerSeconds);
		else
			m_VideoInfo.DurationTimeStamps = 0;

		if(m_VideoInfo.DurationTimeStamps <= 0)
		{
			result = OpenVideoResult::StreamInfoNotFound;
			log->Debug("Duration info not found.");
			break;
		}
		
		// Average FPS. Based on the following sources:
		// - libav in stream info (already in fAvgFrameRate).
		// - libav in container or stream with duration in frames or microseconds (Rarely available but valid if so).
		// - stream->time_base	(Often KO, like 90000:1, expresses the timestamps unit)
		// - codec->time_base (Often OK, but not always).
		// - some ad-hoc special cases.
		int iTicksPerFrame = pCodecCtx->ticks_per_frame;
		m_VideoInfo.FramesPerSeconds = 0;
		if(fAvgFrameRate != 0)
		{
			m_VideoInfo.FramesPerSeconds = fAvgFrameRate;
			log->Debug("Average Fps estimation method: libav.");
		}
		else
		{
			// 1.a. Durations
			if( (pFormatCtx->streams[m_iVideoStream]->nb_frames > 0) && (pFormatCtx->duration > 0))
			{	
				m_VideoInfo.FramesPerSeconds = ((double)pFormatCtx->streams[m_iVideoStream]->nb_frames * (double)AV_TIME_BASE)/(double)pFormatCtx->duration;

				if(iTicksPerFrame > 1)
					m_VideoInfo.FramesPerSeconds /= iTicksPerFrame;
				
				log->Debug("Average Fps estimation method: Durations.");
			}
			else
			{
				// 1.b. stream->time_base, consider invalid if >= 1000.
				m_VideoInfo.FramesPerSeconds = (double)pFormatCtx->streams[m_iVideoStream]->time_base.den / (double)pFormatCtx->streams[m_iVideoStream]->time_base.num;
				
				if(m_VideoInfo.FramesPerSeconds < 1000)
				{
					if(iTicksPerFrame > 1)
						m_VideoInfo.FramesPerSeconds /= iTicksPerFrame;		

					log->Debug("Average Fps estimation method: Stream timebase.");
				}
				else
				{
					// 1.c. codec->time_base, consider invalid if >= 1000.
					m_VideoInfo.FramesPerSeconds = (double)pCodecCtx->time_base.den / (double)pCodecCtx->time_base.num;

					if(m_VideoInfo.FramesPerSeconds < 1000)
					{
						if(iTicksPerFrame > 1)
							m_VideoInfo.FramesPerSeconds /= iTicksPerFrame;
						
						log->Debug("Average Fps estimation method: Codec timebase.");
					}
					else if (m_VideoInfo.FramesPerSeconds == 30000)
					{
						m_VideoInfo.FramesPerSeconds = 29.97;
						log->Debug("Average Fps estimation method: special case detection (30000:1 -> 30000:1001).");
					}
					else if (m_VideoInfo.FramesPerSeconds == 25000)
					{
						m_VideoInfo.FramesPerSeconds = 24.975;
						log->Debug("Average Fps estimation method: special case detection (25000:1 -> 25000:1001).");
					}
					else
					{
						// Detection failed. Force to 25fps.
						m_VideoInfo.FramesPerSeconds = 25;
						log->Debug("Average Fps estimation method: Estimation failed. Fps will be forced to : " + m_VideoInfo.FramesPerSeconds);
					}
				}
			}
		}
		log->Debug("Ticks per frame: " + iTicksPerFrame);

		m_VideoInfo.FrameIntervalMilliseconds = (double)1000/m_VideoInfo.FramesPerSeconds;
		m_VideoInfo.AverageTimeStampsPerFrame = (int64_t)Math::Round(m_VideoInfo.AverageTimeStampsPerSeconds / m_VideoInfo.FramesPerSeconds);
		
		Cache->WorkingZone = VideoSection(
			m_VideoInfo.FirstTimeStamp, 
			m_VideoInfo.FirstTimeStamp + m_VideoInfo.DurationTimeStamps - m_VideoInfo.AverageTimeStampsPerFrame);

		// Image size
		m_VideoInfo.OriginalSize = Size(pCodecCtx->width, pCodecCtx->height);
		
		if(pCodecCtx->sample_aspect_ratio.num != 0 && pCodecCtx->sample_aspect_ratio.num != pCodecCtx->sample_aspect_ratio.den)
		{
			// Anamorphic video, non square pixels.
			log->Debug("Display Aspect Ratio type: Anamorphic");
			if(pCodecCtx->codec_id == CODEC_ID_MPEG2VIDEO)
			{
				// If MPEG, sample_aspect_ratio is actually the DAR...
				// Reference for weird decision tree: mpeg12.c at mpeg_decode_postinit().
				double fDisplayAspectRatio = (double)pCodecCtx->sample_aspect_ratio.num / (double)pCodecCtx->sample_aspect_ratio.den;
				m_VideoInfo.PixelAspectRatio = ((double)pCodecCtx->height * fDisplayAspectRatio) / (double)pCodecCtx->width;

				if(m_VideoInfo.PixelAspectRatio < 1.0f)
					m_VideoInfo.PixelAspectRatio = fDisplayAspectRatio;
			}
			else
			{
				m_VideoInfo.PixelAspectRatio = (double)pCodecCtx->sample_aspect_ratio.num / (double)pCodecCtx->sample_aspect_ratio.den;
			}	
			
			m_VideoInfo.SampleAspectRatio = Fraction(pCodecCtx->sample_aspect_ratio.num, pCodecCtx->sample_aspect_ratio.den);
		}
		else
		{
			// Assume PAR=1:1.
			log->Debug("Display Aspect Ratio type: Square Pixels");
			m_VideoInfo.PixelAspectRatio = 1.0f;
		}

		SetDecodingSize(Options->ImageAspectRatio);
		
		m_pFormatCtx = pFormatCtx;
		m_pCodecCtx	= pCodecCtx;
		m_bIsLoaded = true;		
		result = OpenVideoResult::Success;
	}
	while(false);
	
	return result;
}
int VideoReaderFFMpeg::GetStreamIndex(AVFormatContext* _pFormatCtx, int _iCodecType)
{
	// Returns the best candidate stream for the specified type, -1 if not found.
	unsigned int iCurrentStreamIndex = -1;
	unsigned int iBestStreamIndex = -1;
	int64_t iBestFrames = -1;

	do
	{
		iCurrentStreamIndex++;
		if(_pFormatCtx->streams[iCurrentStreamIndex]->codec->codec_type != _iCodecType)
			continue;
		
		int64_t frames = _pFormatCtx->streams[iCurrentStreamIndex]->nb_frames;
		if(frames > iBestFrames)
		{
			iBestFrames = frames;
			iBestStreamIndex = iCurrentStreamIndex;
		}
	}
	while(iCurrentStreamIndex < _pFormatCtx->nb_streams-1);

	return (int)iBestStreamIndex;
}
void VideoReaderFFMpeg::SetDecodingSize(Kinovea::Video::ImageAspectRatio _ratio)
{
	// Set the image geometry according to the pixel aspect ratio choosen.
	log->DebugFormat("Image aspect ratio: {0}", _ratio);
	
	// Image height from aspect ratio. (width never moves)
	switch(_ratio)
	{
	case Kinovea::Video::ImageAspectRatio::Force43:
		m_VideoInfo.DecodingSize.Height = (int)((m_VideoInfo.OriginalSize.Width * 3.0) / 4.0);
		break;
	case Kinovea::Video::ImageAspectRatio::Force169:
		m_VideoInfo.DecodingSize.Height = (int)((m_VideoInfo.OriginalSize.Width * 9.0) / 16.0);
		break;
	case Kinovea::Video::ImageAspectRatio::ForcedSquarePixels:
		m_VideoInfo.DecodingSize.Height = m_VideoInfo.OriginalSize.Height;
		break;
	case Kinovea::Video::ImageAspectRatio::Auto:
	default:
		m_VideoInfo.DecodingSize.Height = (int)((double)m_VideoInfo.OriginalSize.Height / m_VideoInfo.PixelAspectRatio);
		break;
	}
	
	// Fix unsupported width for conversion to .NET Bitmap.
	if(m_VideoInfo.DecodingSize.Width % 4 != 0)
		m_VideoInfo.DecodingSize.Width = 4 * ((m_VideoInfo.OriginalSize.Width / 4) + 1);
	else
		m_VideoInfo.DecodingSize.Width = m_VideoInfo.OriginalSize.Width;

	if(m_VideoInfo.DecodingSize != m_VideoInfo.OriginalSize)
		log->DebugFormat("Image size: Original:{0}, Decoding:{1}", m_VideoInfo.OriginalSize, m_VideoInfo.DecodingSize);
}
ReadResult VideoReaderFFMpeg::ReadFrame(int64_t _iTimeStampToSeekTo, int _iFramesToDecode, bool _approximate)
{
	//------------------------------------------------------------------------------------
	// Reads a frame and adds it to the frame cache.
	// This function works either for MoveTo or MoveNext type of requests.
	// It decodes as many frames as needed to reach the target timestamp 
	// or the number of frames to decode. Seeks backwards if needed.
	//
	// The _approximate flag is used for thumbnails retrieval. 
	// In this case we don't really care to land exactly on the right frame,
	// so we return after the first decode post-seek.
	//------------------------------------------------------------------------------------
	
	lock l(m_Locker);

	ReadResult result = ReadResult::Success;
	int	iFramesToDecode = _iFramesToDecode;
	int64_t iTargetTimeStamp = _iTimeStampToSeekTo;
	bool seeking = false;

	if(!m_bIsLoaded) 
		return ReadResult::MovieNotLoaded;

	// Find the proper target and number of frames to decode.
	if(_iFramesToDecode < 0)
	{
		// Negative move. Compute seek target.
		iTargetTimeStamp = Cache->Current->Timestamp + (_iFramesToDecode * m_VideoInfo.AverageTimeStampsPerFrame);
		if(iTargetTimeStamp < 0)
			iTargetTimeStamp = 0;
	}

	if(iTargetTimeStamp >= 0)
	{	
		seeking = true;
		iFramesToDecode = 1; // We'll use the target timestamp anyway.

		// AVSEEK_FLAG_BACKWARD -> goes to first I-Frame before target.
		// Then we'll need to decode frame by frame until the target is reached.
		int iSeekRes = avformat_seek_file(
			m_pFormatCtx, 
			m_iVideoStream, 
			0, 
			iTargetTimeStamp, 
			iTargetTimeStamp + (int64_t)m_VideoInfo.AverageTimeStampsPerSeconds,
			AVSEEK_FLAG_BACKWARD);
		
		avcodec_flush_buffers( m_pFormatCtx->streams[m_iVideoStream]->codec);
		m_TimestampInfo = TimestampInfo::Empty;
		
		if(iSeekRes < 0)
			log->ErrorFormat("Error during seek: {0}. Target was:[{1}]", iSeekRes, iTargetTimeStamp);
	}
		
	// Allocate 2 AVFrames, one for the raw decoded frame and one for deinterlaced/rescaled/converted frame.
	AVFrame* pDecodingAVFrame = avcodec_alloc_frame();
	AVFrame* pFinalAVFrame = avcodec_alloc_frame();

	// The buffer holding the actual frame data.
	int iSizeBuffer = avpicture_get_size(m_PixelFormatFFmpeg, m_VideoInfo.DecodingSize.Width, m_VideoInfo.DecodingSize.Height);
	uint8_t* pBuffer = iSizeBuffer > 0 ? new uint8_t[iSizeBuffer] : nullptr;

	if(pDecodingAVFrame == nullptr || pFinalAVFrame == nullptr || pBuffer == nullptr)
		return ReadResult::MemoryNotAllocated;

	// Assigns appropriate parts of buffer to image planes in the AVFrame.
	avpicture_fill((AVPicture *)pFinalAVFrame, pBuffer , m_PixelFormatFFmpeg, m_VideoInfo.DecodingSize.Width, m_VideoInfo.DecodingSize.Height);

	m_TimestampInfo.CurrentTimestamp = Cache->Current == nullptr ? -1 : Cache->Current->Timestamp;
	
	// Reading/Decoding loop
	bool done = false;
	bool bFirstPass = true;
	int iReadFrameResult;
	int iFrameFinished = 0;
	int	iFramesDecoded	= 0;
	do
	{
		// FFMpeg also has an internal buffer to cope with B-Frames entanglement.
		// The DTS/PTS announced is actually the one of the last frame that was put in the buffer by av_read_frame,
		// it is *not* the one of the frame that was extracted from the buffer by avcodec_decode_video.
		// To solve the DTS/PTS issue, we save the timestamps each time we find libav is buffering a frame.
		// And we use the previously saved timestamps.
		// Ref: http://lists.mplayerhq.hu/pipermail/libav-user/2008-August/001069.html

		// Read next packet
		AVPacket InputPacket;
		iReadFrameResult = av_read_frame( m_pFormatCtx, &InputPacket);

		if(iReadFrameResult < 0)
		{
			// Reading error. We don't know if the error happened on a video frame or audio one.
			done = true;
			delete [] pBuffer;
			result = ReadResult::FrameNotRead;
			break;
		}

		if(InputPacket.stream_index != m_iVideoStream)
		{
			av_free_packet(&InputPacket);
			continue;
		}

		// Decode video packet. This is needed even if we're not on the final frame yet.
		// I-Frame data is kept internally by ffmpeg and will need it to build the final frame.
		avcodec_decode_video2(m_pCodecCtx, pDecodingAVFrame, &iFrameFinished, &InputPacket);
		
		if(iFrameFinished == 0)
		{
			// Buffering frame. libav just read a I or P frame that will be presented later.
			// (But which was necessary to get now to decode a coming B frame.)
			SetTimestampFromPacket(InputPacket.dts, InputPacket.pts, false);
			av_free_packet(&InputPacket);
			continue;
		}

		// Update positions.
		SetTimestampFromPacket(InputPacket.dts, InputPacket.pts, true);

		if(seeking && bFirstPass && !_approximate && iTargetTimeStamp >= 0 && m_TimestampInfo.CurrentTimestamp > iTargetTimeStamp)
		{
			// If the current ts is already after the target, we are dealing with this kind of files
			// where the seek doesn't work as advertised. We'll seek back again further,
			// and then decode until we get to it.
			
			// Do this only once.
			bFirstPass = false;
			
			// For some files, one additional second back is not enough. The seek is wrong by up to 4 seconds.
			// We also allow the target to go before 0.
			int iSecondsBack = 4;
			int64_t iForceSeekTimestamp = iTargetTimeStamp - ((int64_t)m_VideoInfo.AverageTimeStampsPerSeconds * iSecondsBack);
			int64_t iMinTarget = System::Math::Min(iForceSeekTimestamp, (int64_t)0);
			
			// Do the seek.
			log->DebugFormat("[Seek] - First decoded frame [{0}] already after target [{1}]. Force seek {2} more seconds back to [{3}]", 
							m_TimestampInfo.CurrentTimestamp, iTargetTimeStamp, iSecondsBack, iForceSeekTimestamp);
			
			avformat_seek_file(m_pFormatCtx, m_iVideoStream, iMinTarget , iForceSeekTimestamp, iForceSeekTimestamp, AVSEEK_FLAG_BACKWARD); 
			avcodec_flush_buffers(m_pFormatCtx->streams[m_iVideoStream]->codec);

			// Free the packet that was allocated by av_read_frame
			av_free_packet(&InputPacket);

			// Loop back to restart decoding frames until we get to the target.
			continue;
		}

		bFirstPass = false;
		iFramesDecoded++;

		//-------------------------------------------------------------------------------
		// If we're done, convert the image and store it into its final recipient.
		// - seek: if we reached the target timestamp.
		// - linear decoding: if we decoded the required number of frames.
		//-------------------------------------------------------------------------------
		if(	seeking && m_TimestampInfo.CurrentTimestamp >= iTargetTimeStamp ||
			!seeking && iFramesDecoded >= iFramesToDecode ||
			_approximate)
		{
			done = true;

			if(seeking && m_TimestampInfo.CurrentTimestamp != iTargetTimeStamp)
				log->DebugFormat("Seeking to [{0}] completed. Final position:[{1}]", iTargetTimeStamp, m_TimestampInfo.CurrentTimestamp);

			// Deinterlace + rescale + convert pixel format.
			bool rescaled = RescaleAndConvert(
				pFinalAVFrame, 
				pDecodingAVFrame, 
				m_VideoInfo.DecodingSize.Width, 
				m_VideoInfo.DecodingSize.Height, 
				m_PixelFormatFFmpeg,
				Options->Deinterlace);

			if(!rescaled)
			{
				delete [] pBuffer;
				result = ReadResult::ImageNotConverted;
			}
			
			try
			{
				// Import ffmpeg buffer into a .NET bitmap.
				int imageStride = pFinalAVFrame->linesize[0];
				IntPtr scan0 = IntPtr((void*)pFinalAVFrame->data[0]); 
				Bitmap^ bmp = gcnew Bitmap(m_VideoInfo.DecodingSize.Width, m_VideoInfo.DecodingSize.Height, imageStride, DecodingPixelFormat, scan0);
				
				// Store a pointer to the native buffer inside the Bitmap.
				// We'll be asked to free this resource later when the frame is not used anymore.
				// It is boxed inside an Object so we can extract it in a type-safe way.
				IntPtr^ boxedPtr = gcnew IntPtr((void*)pBuffer);
				bmp->Tag = boxedPtr;
				
				// Construct the VideoFrame and push it to cache.
				VideoFrame^ vf = gcnew VideoFrame();
				vf->Image = bmp;
				vf->Timestamp = m_TimestampInfo.CurrentTimestamp;
				Cache->Add(vf);
			}
			catch(Exception^ exp)
			{
				delete [] pBuffer;
				result = ReadResult::ImageNotConverted;
				log->Error("Error while converting AVFrame to Bitmap.");
				log->Error(exp);
			}
		}
		
		// Free the packet that was allocated by av_read_frame
		av_free_packet(&InputPacket);
	}
	while(!done);
	
	// Free the AVFrames. (This will not deallocate the data buffers).
	av_free(pFinalAVFrame);
	av_free(pDecodingAVFrame);

#ifdef INSTRUMENTATION	
	if(!Cache->Empty)
		log->DebugFormat("[{0}] - Memory: {1:0,0} bytes", Cache->Current->Timestamp, Process::GetCurrentProcess()->PrivateMemorySize64);
#endif

	return result;
}
void VideoReaderFFMpeg::SetTimestampFromPacket(int64_t _dts, int64_t _pts, bool _bDecoded)
{
	//---------------------------------------------------------------------------------------------------------
	// Try to guess the presentation timestamp of the packet we just read / decoded.
	// Presentation timestamps will be used everywhere for seeking, positioning, time calculations, etc.
	//
	// dts: decoding timestamp, 
	// pts: presentation timestamp, 
	// decoded: if libav finished to decode the frame or is just buffering.
	//
	// It must be noted that the timestamp given by libav is the timestamp of the frame it just read,
	// but the frame we will get from av_decode_video may come from its internal buffer and have a different timestamp.
	// Furthermore, some muxers do not fill the PTS value, and others only intermittently.
	// Kinovea prior to version 0.8.8 was using the DTS value as primary timestamp, which is wrong.
	//---------------------------------------------------------------------------------------------------------

	if(_pts == AV_NOPTS_VALUE || _pts < 0)
	{
		// Hum, too bad, the muxer did not specify the PTS for this packet.

		if(_bDecoded)
		{
			if(_dts == AV_NOPTS_VALUE || _dts < 0)
			{
				/*log->Debug(String::Format("Decoded - No value for PTS / DTS. Last known timestamp: {0}, Buffered ts if any: {1}", 
					(m_PrimarySelection->iLastDecodedPTS >= 0)?String::Format("{0}", m_PrimarySelection->iLastDecodedPTS):"None", 
					(m_PrimarySelection->iBufferedPTS < Int64::MaxValue)?String::Format("{0}", m_PrimarySelection->iBufferedPTS):"None"));*/

				if(m_TimestampInfo.BufferedPTS < Int64::MaxValue)
				{
					// No info but we know a frame was previously buffered, so it must be this one we took out.
					// Unfortunately, we don't know the timestamp of the frame that is being buffered now...
					m_TimestampInfo.CurrentTimestamp = m_TimestampInfo.BufferedPTS;
					m_TimestampInfo.BufferedPTS = Int64::MaxValue;
				}
				else if(m_TimestampInfo.LastDecodedPTS >= 0)
				{
					// No info but we know a frame was previously decoded, so it must be shortly after it.
					m_TimestampInfo.CurrentTimestamp = m_TimestampInfo.LastDecodedPTS + m_VideoInfo.AverageTimeStampsPerFrame;
					//log->Debug(String::Format("Current PTS estimation: {0}",	m_PrimarySelection->iCurrentTimeStamp));
				}
				else
				{
					// No info + never buffered, never decoded. This must be the first frame.
					m_TimestampInfo.CurrentTimestamp = 0;
					//log->Debug(String::Format("Setting current PTS to 0"));
				}
			}
			else
			{
				// DTS is given, but not PTS.
				// Either this file is not using PTS, or it just didn't fill it for this frame, no way to know...
				if(m_TimestampInfo.BufferedPTS < _dts)
				{
					// Argh. Comparing buffered frame PTS with read frame DTS ?
					// May work for files that only store DTS all along though.
					//log->Debug(String::Format("Decoded buffered frame - [{0}]", m_PrimarySelection->iBufferedPTS));
					//log->Debug(String::Format("Buffering - DTS:[{0}] - No PTS", _dts));
					m_TimestampInfo.CurrentTimestamp = m_TimestampInfo.BufferedPTS;	
					m_TimestampInfo.BufferedPTS = _dts;
				}
				else
				{
					//log->Debug(String::Format("Decoded (direct) - DTS:[{0}], No PTS", _dts));
					m_TimestampInfo.CurrentTimestamp = System::Math::Max((int64_t)0, _dts);
				}
			}

			m_TimestampInfo.LastDecodedPTS = m_TimestampInfo.CurrentTimestamp;
		}
		else
		{
			// Buffering a frame.
			// What if there is already something in the buffer ?
			// We should keep a queue of buffered frames and serve them back in order.
			if(_dts < 0)
			{ 
				//log->Debug(String::Format("Buffering (no decode) - No PTS, negative DTS"));
				
				// Hopeless situation. Let's reset the buffered frame timestamp,
				// The decode will rely on the last seen PTS from a decoded frame, if any.
				m_TimestampInfo.BufferedPTS = Int64::MaxValue;
			}
			else if(_dts == AV_NOPTS_VALUE)
			{
				//log->Debug(String::Format("Buffering (no decode) - No PTS, No DTS"));
				m_TimestampInfo.BufferedPTS = 0;
			}
			else
			{
				//log->Debug(String::Format("Buffering (no decode) - No PTS, DTS:[{0}]", _dts));
				m_TimestampInfo.BufferedPTS = _dts;
			}
		}
	}
	else
	{
		// PTS is given (nice).
		// We still need to check if there is something in the buffer, in which case
		// the decoded frame is in fact the one from the buffer.
		// (This may not even hold true, for H.264 and out of GOP reference.)
		if(_bDecoded)
		{
			if(m_TimestampInfo.BufferedPTS < _pts)
			{
				// There is something in the buffer with a timestamp prior to the one of the decoded frame.
				// That is probably the frame we got from libav.
				// The timestamp it presented us on the other hand, is the one it's being buffering.
				//log->Debug(String::Format("Decoded buffered frame - PTS:[{0}]", m_PrimarySelection->iBufferedPTS));
				//log->Debug(String::Format("Buffering - DTS:[{0}], PTS:[{1}]", _dts, _pts));
				
				m_TimestampInfo.CurrentTimestamp = m_TimestampInfo.BufferedPTS;
				m_TimestampInfo.BufferedPTS = _pts;
			}
			else
			{
				//log->Debug(String::Format("Decoded (direct) - DTS:[{0}], PTS:[{1}]", _dts, _pts));
				m_TimestampInfo.CurrentTimestamp = _pts;
			}

			m_TimestampInfo.LastDecodedPTS = m_TimestampInfo.CurrentTimestamp;
		}
		else
		{
			// What if there is already something in the buffer ?
			// We should keep a queue of buffered frame and serve them back in order.
			//log->Debug(String::Format("Buffering (no decode) -- DTS:[{0}], PTS:[{1}]", _dts, _pts));
			m_TimestampInfo.BufferedPTS = _pts;
		}
	}
}
bool VideoReaderFFMpeg::RescaleAndConvert(AVFrame* _pOutputFrame, AVFrame* _pInputFrame, int _OutputWidth, int _OutputHeight, int _OutputFmt, bool _bDeinterlace)
{
	//------------------------------------------------------------------------
	// Function used by GetNextFrame, ImportAnalysis and SaveMovie.
	// Take the frame we just decoded and turn it to the right size/deint/fmt.
	// todo: sws_getContext could be done only once.
	//------------------------------------------------------------------------
	bool bSuccess = true;
	SwsContext* pSWSCtx = sws_getContext(
		m_pCodecCtx->width, 
		m_pCodecCtx->height, 
		m_pCodecCtx->pix_fmt, 
		_OutputWidth, 
		_OutputHeight, 
		(PixelFormat)_OutputFmt, 
		DecodingQuality, 
		nullptr, nullptr, nullptr); 
		
	uint8_t** ppOutputData = nullptr;
	int* piStride = nullptr;
	uint8_t* pDeinterlaceBuffer = nullptr;

	if(_bDeinterlace)
	{
		AVPicture*	pDeinterlacingFrame;
		AVPicture	tmpPicture;

		// Deinterlacing happens before resizing.
		int iSizeDeinterlaced = avpicture_get_size(m_pCodecCtx->pix_fmt, m_pCodecCtx->width, m_pCodecCtx->height);
		
		pDeinterlaceBuffer = new uint8_t[iSizeDeinterlaced];
		pDeinterlacingFrame = &tmpPicture;
		avpicture_fill(pDeinterlacingFrame, pDeinterlaceBuffer, m_pCodecCtx->pix_fmt, m_pCodecCtx->width, m_pCodecCtx->height);

		int resDeint = avpicture_deinterlace(pDeinterlacingFrame, (AVPicture*)_pInputFrame, m_pCodecCtx->pix_fmt, m_pCodecCtx->width, m_pCodecCtx->height);

		if(resDeint < 0)
		{
			// Deinterlacing failed, use original image.
			log->Debug("Deinterlacing failed, use original image.");
			//sws_scale(pSWSCtx, _pInputFrame->data, _pInputFrame->linesize, 0, m_pCodecCtx->height, _pOutputFrame->data, _pOutputFrame->linesize); 
			ppOutputData = _pInputFrame->data;
			piStride = _pInputFrame->linesize;
		}
		else
		{
			// Use deinterlaced image.
			//sws_scale(pSWSCtx, pDeinterlacingFrame->data, pDeinterlacingFrame->linesize, 0, m_pCodecCtx->height, _pOutputFrame->data, _pOutputFrame->linesize); 
			ppOutputData = pDeinterlacingFrame->data;
			piStride = pDeinterlacingFrame->linesize;
		}
	}
	else
	{
		//sws_scale(pSWSCtx, _pInputFrame->data, _pInputFrame->linesize, 0, m_pCodecCtx->height, _pOutputFrame->data, _pOutputFrame->linesize); 
		ppOutputData = _pInputFrame->data;
		piStride = _pInputFrame->linesize;
	}

	try
	{
		sws_scale(pSWSCtx, ppOutputData, piStride, 0, m_pCodecCtx->height, _pOutputFrame->data, _pOutputFrame->linesize); 
	}
	catch(Exception^)
	{
		bSuccess = false;
		log->Error("RescaleAndConvert Error : sws_scale failed.");
	}

	// Clean Up.
	sws_freeContext(pSWSCtx);
	
	if(pDeinterlaceBuffer != nullptr)
		delete [] pDeinterlaceBuffer;

	return bSuccess;
}
void VideoReaderFFMpeg::Prefetch(Object^ _canceler)
{
	Thread::CurrentThread->Name = "Decoding";
	ThreadCanceler^ canceler = (ThreadCanceler^)_canceler;
	
	log->DebugFormat("Start async decoding thread.");
	
	while(true)
	{
		if(canceler->CancellationPending)
		{
			log->DebugFormat("Async decoding thread cancelled.");
			break;
		}
		
		ReadResult res = ReadFrame(-1, 1, false);
		if(res == ReadResult::FrameNotRead && !canceler->CancellationPending)
			ReadFrame(0, 1, false);
	}

	log->DebugFormat("Exiting async decoding thread.");
}
void VideoReaderFFMpeg::DisposeFrame(VideoFrame^ _frame)
{
	// Dispose the Bitmap and the native buffer.
	// The pointer to the native buffer was stored in the Tag property.
	IntPtr^ ptr = dynamic_cast<IntPtr^>(_frame->Image->Tag);
	
	delete _frame->Image;
	
	if(ptr != nullptr)
	{
		uint8_t* pBuf = (uint8_t*)ptr->ToPointer();
		delete [] pBuf;
	}
}

void VideoReaderFFMpeg::DumpInfo()
{
	log->Debug("---------------------------------------------------");
	log->Debug("[File] - Filename : " + Path::GetFileName(m_VideoInfo.FilePath));
	log->DebugFormat("[Container] - Name: {0} ({1})", gcnew String(m_pFormatCtx->iformat->name), gcnew String(m_pFormatCtx->iformat->long_name));
	DumpStreamsInfos(m_pFormatCtx);
	log->Debug("[Container] - Duration (s): " + (double)m_pFormatCtx->duration/1000000);
	log->Debug("[Container] - Bit rate: " + m_pFormatCtx->bit_rate);
	if(m_pFormatCtx->streams[m_iVideoStream]->nb_frames > 0)
		log->DebugFormat("[Stream] - Duration (frames): {0}", m_pFormatCtx->streams[m_iVideoStream]->nb_frames);
	else
		log->Debug("[Stream] - Duration (frames): Unavailable.");
	log->DebugFormat("[Stream] - PTS wrap bits: {0}", m_pFormatCtx->streams[m_iVideoStream]->pts_wrap_bits);
	log->DebugFormat("[Stream] - TimeBase: {0}:{1}", m_pFormatCtx->streams[m_iVideoStream]->time_base.den, m_pFormatCtx->streams[m_iVideoStream]->time_base.num);
	log->DebugFormat("[Stream] - Average timestamps per seconds: {0}", m_VideoInfo.AverageTimeStampsPerSeconds);
	log->DebugFormat("[Container] - Start time (µs): {0}", m_pFormatCtx->start_time);
	log->DebugFormat("[Container] - Start timestamp: {0}", m_VideoInfo.FirstTimeStamp);
	log->DebugFormat("[Codec] - Name: {0}, id:{1}", gcnew String(m_pCodecCtx->codec_name), (int)m_pCodecCtx->codec_id);
	log->DebugFormat("[Codec] - TimeBase: {0}:{1}", m_pCodecCtx->time_base.den, m_pCodecCtx->time_base.num);
	log->Debug("[Codec] - Bit rate: " + m_pCodecCtx->bit_rate);
	log->Debug("Duration in timestamps: " + m_VideoInfo.DurationTimeStamps);
	log->Debug("Duration in seconds (computed): " + (double)(double)m_VideoInfo.DurationTimeStamps/(double)m_VideoInfo.AverageTimeStampsPerSeconds);
	log->Debug("Average Fps: " + m_VideoInfo.FramesPerSeconds);
	log->Debug("Average Frame Interval (ms): " + m_VideoInfo.FrameIntervalMilliseconds);
	log->Debug("Average Timestamps per frame: " + m_VideoInfo.AverageTimeStampsPerFrame);
	log->DebugFormat("[Codec] - Has B Frames: {0}", m_pCodecCtx->has_b_frames);
	log->Debug("[Codec] - Width (pixels): " + m_pCodecCtx->width);
	log->Debug("[Codec] - Height (pixels): " + m_pCodecCtx->height);
	log->Debug("[Codec] - Pixel Aspect Ratio: " + m_VideoInfo.PixelAspectRatio);
	log->Debug("---------------------------------------------------");
}


void VideoReaderFFMpeg::DumpStreamsInfos(AVFormatContext* _pFormatCtx)
{
	log->Debug("[Container] - Number of streams: " + _pFormatCtx->nb_streams);
	
	for(int i = 0;i<(int)_pFormatCtx->nb_streams;i++)
	{
		String^ streamType;
		
		switch((int)_pFormatCtx->streams[i]->codec->codec_type)
		{
		case AVMEDIA_TYPE_VIDEO:
			streamType = "AVMEDIA_TYPE_VIDEO";
			break;
		case AVMEDIA_TYPE_AUDIO:
			streamType = "AVMEDIA_TYPE_AUDIO";
			break;
		case AVMEDIA_TYPE_DATA:
			streamType = "AVMEDIA_TYPE_DATA";
			break;
		case AVMEDIA_TYPE_SUBTITLE:
			streamType = "AVMEDIA_TYPE_SUBTITLE";
			break;
		case AVMEDIA_TYPE_UNKNOWN:
		default:
			streamType = "AVMEDIA_TYPE_UNKNOWN";
			break;
		}

		log->DebugFormat("[Stream] #{0}, Type : {1}, {2}", i, streamType, _pFormatCtx->streams[i]->nb_frames);
	}
}
void VideoReaderFFMpeg::DumpFrameType(int _type)
{
	switch(_type)
	{
	case FF_I_TYPE:
		log->Debug("(I) Frame +++++");
		break;
	case FF_P_TYPE:
		log->Debug("(P) Frame --");
		break;
	case FF_B_TYPE:
		log->Debug("(B) Frame .");
		break;
	case FF_S_TYPE:
		log->Debug("Frame : S(GMC)-VOP MPEG4");
		break;
	case FF_SI_TYPE:
		log->Debug("Switching Intra");
		break;
	case FF_SP_TYPE:
		log->Debug("Switching Predicted");
		break;
	case FF_BI_TYPE:
		log->Debug("FF_BI_TYPE");
		break;
	}
}
