
#include <windows.h>
#include <shellapi.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>

typedef float Sample;

static Uint32 SDL_AUDIO_TRANSPARENTLY_CONVERT_FORMAT = 0;
static Uint32 SAMPLE_RATE = 48000;
static Uint32 CHANNELS = 6;
static Uint32 BUFFER_NSAMPLES = 9000;
static Uint32 BUFFER_SIZE = BUFFER_NSAMPLES * CHANNELS * sizeof(Sample);
static Uint32 SDL_PACKET_SIZE = 512;

///////////////////////////////////////////////////////////////////////////////

struct platform_program_state {
  SDL_Window *window;
  HMENU menu;
  bool IsRunning;
  SDL_Event LastEvent;
};

struct platform_audio_config {
  int Channels;
  int SamplesPerSecond;
  int BytesPerSample;
};

struct platform_audio_buffer {
  Uint8* Buffer;
  int Size;
  int ReadCursor;
  int WriteCursor;
  int NumInactiveLoops;
  bool Full;
  bool Active;
  SDL_AudioDeviceID DeviceID;
  platform_audio_config* AudioConfig;
};

struct platform_audio_thread_context {
  platform_audio_buffer* AudioBuffer;
  platform_program_state* ProgramState;
  AVFormatContext* format;
  AVCodecContext* codec;
  AVFrame* frame;
};

///////////////////////////////////////////////////////////////////////////////

void SampleIntoAudioBuffer(platform_audio_buffer* AudioBuffer, Sample *samples, uint32_t nsamples) {
  platform_audio_config* AudioConfig = AudioBuffer->AudioConfig;
  int nchannels = AudioBuffer->AudioConfig->Channels;

  if (!AudioBuffer->Active) {
      SDL_PauseAudioDevice(AudioBuffer->DeviceID, 0);
      AudioBuffer->Active = true;
      AudioBuffer->NumInactiveLoops = 0;
  }
  if (AudioBuffer->Full) {
    return;
  }
  uint32_t idx = AudioBuffer->WriteCursor;
  Sample *end_samples = samples + nsamples * nchannels;
  while ((samples < end_samples) && (idx != AudioBuffer->ReadCursor)) {
    Sample* buffer = (Sample*)(AudioBuffer->Buffer + idx);
    for (int channel = 0; channel < nchannels; ++channel) {
      *buffer++ = *samples++;
    }
    idx = (idx + nchannels * sizeof(Sample)) % AudioBuffer->Size;
  }
  AudioBuffer->WriteCursor = idx;
  if (AudioBuffer->WriteCursor == AudioBuffer->ReadCursor) {
    AudioBuffer->Full = true;
  }
  SDL_Delay(1);
}

///////////////////////////////////////////////////////////////////////////////

void PlatformFillAudioDeviceBuffer(void* UserData, Uint8* DeviceBuffer, int Length) {
  platform_audio_buffer* AudioBuffer = (platform_audio_buffer*)UserData;
  bool full = AudioBuffer->Full;
  uint32_t rc = AudioBuffer->ReadCursor;
  uint32_t wc = AudioBuffer->WriteCursor;
  bool empty = (rc == wc) && !full;
  uint32_t size = AudioBuffer->Size;

  // Is the buffer empty?
  if (empty) {
    // Fill with zeros.
    SDL_memset(DeviceBuffer, 0, Length);
    if(++AudioBuffer->NumInactiveLoops > 10) {
      SDL_PauseAudioDevice(AudioBuffer->DeviceID, 1);
      AudioBuffer->Active = false;
    }
  }
  for (uint32_t i = 0; i < Length; ++i) {
    if ((rc == wc) && !full) {
      DeviceBuffer[i] = 0;
    } else {
      DeviceBuffer[i] = AudioBuffer->Buffer[rc];
      rc = (rc + 1) % size;
      full = false;
    }
  }
  AudioBuffer->ReadCursor = rc;
  AudioBuffer->Full = false;
}

///////////////////////////////////////////////////////////////////////////////

void PlatformInitializeAudio(platform_audio_buffer* AudioBuffer) {
  SDL_AudioSpec AudioSettings = {};
  AudioSettings.freq = AudioBuffer->AudioConfig->SamplesPerSecond;
  AudioSettings.format = AUDIO_F32;
  AudioSettings.channels = AudioBuffer->AudioConfig->Channels;
  AudioSettings.samples = SDL_PACKET_SIZE;
  AudioSettings.callback = &PlatformFillAudioDeviceBuffer;
  AudioSettings.userdata = AudioBuffer;

  SDL_AudioSpec ObtainedSettings = {};
  AudioBuffer->DeviceID = SDL_OpenAudioDevice(NULL, 0, &AudioSettings, &ObtainedSettings,
					      SDL_AUDIO_TRANSPARENTLY_CONVERT_FORMAT);

  if ((AudioSettings.format != ObtainedSettings.format) ||
      (AudioSettings.channels != ObtainedSettings.channels)) {
    SDL_Log("Unable to obtain expected audio settings: %s", SDL_GetError());
    exit(1);
  }

  // Start playing the audio buffer
  AudioBuffer->Active = true;
  SDL_PauseAudioDevice(AudioBuffer->DeviceID, 0);
}

///////////////////////////////////////////////////////////////////////////////

void PlatformHandleEvent(platform_program_state* ProgramState) {
  switch (ProgramState->LastEvent.type) {
  case SDL_SYSWMEVENT:
    if (ProgramState->LastEvent.syswm.msg->msg.win.msg == WM_USER + 1) {
      if (LOWORD(ProgramState->LastEvent.syswm.msg->msg.win.lParam) == WM_LBUTTONDBLCLK) {
	SDL_ShowWindow(ProgramState->window);
	SDL_RestoreWindow(ProgramState->window);
      } else if (LOWORD(ProgramState->LastEvent.syswm.msg->msg.win.lParam) == WM_RBUTTONDOWN) {
        // Get current mouse position.
        POINT curPoint ;
        GetCursorPos( &curPoint ) ;
        UINT clicked = TrackPopupMenu(ProgramState->menu,
				      // don't send me WM_COMMAND messages about this window,
				      // instead return the identifier of the clicked menu item
				      TPM_RETURNCMD | TPM_NONOTIFY,
				      curPoint.x, curPoint.y, 0,
				      ProgramState->LastEvent.syswm.msg->msg.win.hwnd, NULL);
        if (clicked == 3000) {
          // quit the application.
	  ProgramState->IsRunning = false;
	}
      }
    }
    break;
  case SDL_QUIT:
    ProgramState->IsRunning = false;
    break;
  case SDL_WINDOWEVENT:
    if (ProgramState->LastEvent.window.event == SDL_WINDOWEVENT_MINIMIZED)
      SDL_HideWindow(ProgramState->window);
    break;
  }
}

///////////////////////////////////////////////////////////////////////////////

int PlatformAudioThread(void* UserData) {
  platform_audio_thread_context* AudioThread = (platform_audio_thread_context*)UserData;
  AVFrame* frame = AudioThread->frame;

  SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
  AVPacket packet;
  av_init_packet(&packet);

  while (AudioThread->ProgramState->IsRunning) {

    if (av_read_frame(AudioThread->format, &packet) >= 0) {

      // decode one frame
      int gotFrame;
      if (avcodec_decode_audio4(AudioThread->codec, frame, &gotFrame, &packet) < 0) {
	continue;
      }
      if (!gotFrame) {
	continue;
      }
      Sample *samples = new float[frame->nb_samples * frame->channels];
      for (size_t s = 0; s < frame->nb_samples; ++s) {
	for (size_t c = 0; c < frame->channels; ++c) {
	  float *samps = (float*)(frame->extended_data[c]);
	  samples[s * frame->channels + c] = samps[s];
	}
      }

      if (frame->nb_samples) {
	SDL_LockAudioDevice(AudioThread->AudioBuffer->DeviceID);
	SampleIntoAudioBuffer(AudioThread->AudioBuffer, samples, frame->nb_samples);
	SDL_UnlockAudioDevice(AudioThread->AudioBuffer->DeviceID);
      }
      delete [] samples;
      av_free_packet(&packet);
      SDL_Delay(1);
    }

  }

  return 0;
}

///////////////////////////////////////////////////////////////////////////////

int main(int argc, char** argv) {
  //char* filename = "../test2.dts";
  char* filename = "COM4";

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
    SDL_Log("Unable to initialized SDL: %s", SDL_GetError());
    return 1;
  }

  platform_program_state ProgramState = {};
  ProgramState.window = SDL_CreateWindow("USB SPDIF Player", SDL_WINDOWPOS_UNDEFINED,
					 SDL_WINDOWPOS_UNDEFINED, 200, 200, SDL_WINDOW_HIDDEN);

  SDL_SysWMinfo info;
  SDL_VERSION(&info.version); 

  NOTIFYICONDATA icon;
  if (SDL_GetWindowWMInfo(ProgramState.window, &info)) {
    icon.uCallbackMessage = WM_USER + 1;
    icon.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    icon.hIcon = LoadIcon(NULL, IDI_INFORMATION);
    icon.cbSize = sizeof(icon);
    icon.hWnd = info.info.win.window;
    strcpy_s(icon.szTip, "USB SPDIF Player");
    bool success = Shell_NotifyIcon(NIM_ADD, &icon);
  }
  SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);

  ProgramState.menu = CreatePopupMenu();
  AppendMenu(ProgramState.menu, MF_STRING, 3000, TEXT("Exit"));

  // initialize all muxers, demuxers and protocols for libavformat
  // (does nothing if called twice during the course of one program execution)
  av_register_all();

  // get format from audio file
  AVFormatContext* format = avformat_alloc_context();
  if (avformat_open_input(&format, filename, NULL, NULL) != 0) {
    fprintf(stderr, "Could not open file '%s'\n", filename);
    return -1;
  }
  if (avformat_find_stream_info(format, NULL) < 0) {
    fprintf(stderr, "Could not retrieve stream info from file '%s'\n", filename);
    return -1;
  }

  // Find the index of the first audio stream
  int stream_index =- 1;
  for (int i=0; i<format->nb_streams; i++) {
    if (format->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
      stream_index = i;
      break;
    }
  }
  if (stream_index == -1) {
    fprintf(stderr, "Could not retrieve audio stream from file '%s'\n", filename);
    return -1;
  }
  AVStream* stream = format->streams[stream_index];

  // find & open codec
  AVCodecContext* codec = stream->codec;
  if (avcodec_open2(codec, avcodec_find_decoder(codec->codec_id), NULL) < 0) {
    fprintf(stderr, "Failed to open decoder for stream #%u in file '%s'\n", stream_index, filename);
    return -1;
  }
 
  // prepare to read data
  AVFrame* frame = av_frame_alloc();
  if (!frame) {
    fprintf(stderr, "Error allocating the frame\n");
    return -1;
  }

  platform_audio_config AudioConfig = {};
  AudioConfig.SamplesPerSecond = SAMPLE_RATE;
  AudioConfig.Channels = codec->channels;
  AudioConfig.BytesPerSample = AudioConfig.Channels * sizeof(Sample);

  platform_audio_buffer AudioBuffer = {};
  AudioBuffer.Size = BUFFER_SIZE;
  AudioBuffer.Buffer = new Uint8[AudioBuffer.Size];
  AudioBuffer.ReadCursor = 0;
  AudioBuffer.WriteCursor = 0;
  AudioBuffer.NumInactiveLoops = 0;
  AudioBuffer.Full = false;
  AudioBuffer.AudioConfig = &AudioConfig;
  memset(AudioBuffer.Buffer, 0, AudioBuffer.Size);

  ProgramState.IsRunning = true;

  // Start the playback
  PlatformInitializeAudio(&AudioBuffer);

  // Start the read thread.
  platform_audio_thread_context AudioThreadContext = {};
  AudioThreadContext.AudioBuffer = &AudioBuffer;
  AudioThreadContext.ProgramState = &ProgramState;
  AudioThreadContext.format = format;
  AudioThreadContext.frame = frame;
  AudioThreadContext.codec = codec;
  SDL_Thread* AudioThread =
    SDL_CreateThread(PlatformAudioThread, "Audio", (void*)&AudioThreadContext);

  while (ProgramState.IsRunning) {
    while (SDL_PollEvent(&ProgramState.LastEvent)) {
      PlatformHandleEvent(&ProgramState);
    }
    SDL_Delay(100);
  }

  SDL_WaitThread(AudioThread, NULL);

  SDL_CloseAudioDevice(AudioBuffer.DeviceID);
  SDL_Quit();

  delete [] AudioBuffer.Buffer;
  return 0;
}
