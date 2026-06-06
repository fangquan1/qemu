audio_ss = ss.source_set()
gvt_audio_gst = [
  dependency('gstreamer-1.0'),
  dependency('gstreamer-app-1.0'),
  dependency('gstreamer-audio-1.0'),
]

audio_ss.add(gvt_audio_gst)
audio_ss.add(files(
  'audio.c',
  'audio-be.c',
  'audio-mixeng-be.c',
  'mixeng.c',
  'noaudio.c',
  'wavaudio.c',
  'gvt-rtp-audio.c',
))

# deprecated since v10.2, to be removed
system_ss.add(files('audio-hmp-cmds.c', 'wavcapture.c'))

audio_modules = {}
foreach m : [
  ['alsa', alsa, files('alsaaudio.c')],
  ['coreaudio', coreaudio, files('coreaudio.m')],
  ['dsound', dsound, files('dsoundaudio.c', 'audio_win_int.c')],
  ['oss', oss, files('ossaudio.c')],
  ['pa', pulse, files('paaudio.c')],
  ['sdl', sdl, files('sdlaudio.c')],
  ['jack', jack, files('jackaudio.c')],
  ['sndio', sndio, files('sndioaudio.c')],
  ['pipewire', pipewire, files('pwaudio.c')],
  ['spice', spice, files('spiceaudio.c')]
]
  if m[1].found()
    module_ss = ss.source_set()
    module_ss.add(m[1], m[2])
    audio_modules += {m[0] : module_ss}
  endif
endforeach

if dbus_display
    module_ss = ss.source_set()
    module_ss.add(when: [gio, pixman],
                  if_true: [dbus_display1_h, files('dbusaudio.c')])
    audio_modules += {'dbus': module_ss}
endif

modules += {'audio': audio_modules}
