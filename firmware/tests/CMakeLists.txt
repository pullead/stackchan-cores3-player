cmake_minimum_required(VERSION 3.16)

project(stackchan_firmware_tests LANGUAGES CXX)

enable_testing()

add_executable(motion_math_test
    motion_math_test.cpp
    ../main/stackchan/motion/motion_math.cpp
    host_stubs/no_abort_dialog.cpp
)

target_include_directories(motion_math_test PRIVATE
    ../main
)

target_compile_features(motion_math_test PRIVATE cxx_std_17)

add_test(NAME motion_math_test COMMAND $<TARGET_FILE:motion_math_test>)

add_executable(media_volume_policy_test
    media_volume_policy_test.cpp
    host_stubs/no_abort_dialog.cpp
)

target_include_directories(media_volume_policy_test PRIVATE
    ../main
)

target_compile_features(media_volume_policy_test PRIVATE cxx_std_17)

add_test(NAME media_volume_policy_test COMMAND $<TARGET_FILE:media_volume_policy_test>)

add_executable(media_state_machine_test
    media_state_machine_test.cpp
    ../main/media/media_state_machine.cpp
    host_stubs/no_abort_dialog.cpp
)

target_include_directories(media_state_machine_test PRIVATE
    ../main
)

target_compile_features(media_state_machine_test PRIVATE cxx_std_17)

add_test(NAME media_state_machine_test COMMAND $<TARGET_FILE:media_state_machine_test>)

add_executable(media_speaker_sink_test
    media_speaker_sink_test.cpp
    ../main/media/audio/core_s3_speaker_sink.cpp
    ../main/media/audio/media_audio_session.cpp
    host_stubs/no_abort_dialog.cpp
)

target_include_directories(media_speaker_sink_test PRIVATE
    ../main
)

target_compile_features(media_speaker_sink_test PRIVATE cxx_std_17)

add_test(NAME media_speaker_sink_test COMMAND $<TARGET_FILE:media_speaker_sink_test>)

add_executable(wav_reader_test
    wav_reader_test.cpp
    ../main/media/local/wav_reader.cpp
    host_stubs/no_abort_dialog.cpp
)

target_include_directories(wav_reader_test PRIVATE
    ../main
)

target_compile_features(wav_reader_test PRIVATE cxx_std_17)

add_test(NAME wav_reader_test COMMAND $<TARGET_FILE:wav_reader_test>)

add_executable(local_playback_controller_test
    local_playback_controller_test.cpp
    ../main/media/local/local_playback_controller.cpp
    ../main/media/local/wav_reader.cpp
    ../main/media/media_state_machine.cpp
    ../main/media/media_mode_controller.cpp
    ../main/media/audio/pcm_tap.cpp
    host_stubs/no_abort_dialog.cpp
)

target_include_directories(local_playback_controller_test PRIVATE
    ../main
)

target_compile_features(local_playback_controller_test PRIVATE cxx_std_17)

add_test(NAME local_playback_controller_test COMMAND $<TARGET_FILE:local_playback_controller_test>)

add_executable(sd_card_port_test
    sd_card_port_test.cpp
    ../main/media/storage/sd_card_port.cpp
    ../main/hal/board/spi3_display_handoff.cpp
    host_stubs/no_abort_dialog.cpp
)

target_include_directories(sd_card_port_test PRIVATE
    ../main
)

target_compile_features(sd_card_port_test PRIVATE cxx_std_17)

add_test(NAME sd_card_port_test COMMAND $<TARGET_FILE:sd_card_port_test>)

add_executable(spi3_display_handoff_test
    spi3_display_handoff_test.cpp
    ../main/hal/board/spi3_display_handoff.cpp
    host_stubs/no_abort_dialog.cpp
)

target_include_directories(spi3_display_handoff_test PRIVATE
    ../main
)

target_compile_features(spi3_display_handoff_test PRIVATE cxx_std_17)

add_test(NAME spi3_display_handoff_test COMMAND $<TARGET_FILE:spi3_display_handoff_test>)

add_executable(local_music_presenter_test
    local_music_presenter_test.cpp
    ../main/apps/app_local_music/local_music_presenter.cpp
    host_stubs/no_abort_dialog.cpp
)

target_include_directories(local_music_presenter_test PRIVATE
    ../main
)

target_compile_features(local_music_presenter_test PRIVATE cxx_std_17)

add_test(NAME local_music_presenter_test COMMAND $<TARGET_FILE:local_music_presenter_test>)

add_executable(sd_card_diagnostic_test
    sd_card_diagnostic_test.cpp
    ../main/media/storage/sd_card_diagnostic.cpp
    host_stubs/no_abort_dialog.cpp
)

target_include_directories(sd_card_diagnostic_test PRIVATE
    ../main
)

target_compile_features(sd_card_diagnostic_test PRIVATE cxx_std_17)

add_test(NAME sd_card_diagnostic_test COMMAND $<TARGET_FILE:sd_card_diagnostic_test>)

add_executable(aw9523_sd_power_test
    aw9523_sd_power_test.cpp
    host_stubs/no_abort_dialog.cpp
)

target_include_directories(aw9523_sd_power_test PRIVATE
    ../main
)

target_compile_features(aw9523_sd_power_test PRIVATE cxx_std_17)

add_test(NAME aw9523_sd_power_test COMMAND $<TARGET_FILE:aw9523_sd_power_test>)

add_executable(fatfs_config_test
    fatfs_config_test.cpp
    host_stubs/no_abort_dialog.cpp
)

target_compile_features(fatfs_config_test PRIVATE cxx_std_23)

add_test(NAME fatfs_config_test
    COMMAND $<TARGET_FILE:fatfs_config_test> ${CMAKE_CURRENT_SOURCE_DIR}/../sdkconfig)

add_executable(audio_stream_contract_test
    audio_stream_contract_test.cpp
    host_stubs/no_abort_dialog.cpp
)

target_include_directories(audio_stream_contract_test PRIVATE
    ../main
)

target_compile_features(audio_stream_contract_test PRIVATE cxx_std_17)

add_test(NAME audio_stream_contract_test COMMAND $<TARGET_FILE:audio_stream_contract_test>)

add_executable(sd_audio_stream_test
    sd_audio_stream_test.cpp
    ../main/media/storage/sd_audio_stream.cpp
    ../main/hal/board/spi3_display_handoff.cpp
    host_stubs/no_abort_dialog.cpp
)
target_include_directories(sd_audio_stream_test PRIVATE ../main)
target_compile_features(sd_audio_stream_test PRIVATE cxx_std_17)
add_test(NAME sd_audio_stream_test COMMAND $<TARGET_FILE:sd_audio_stream_test>)

add_executable(hifi_decoder_adapter_test
    hifi_decoder_adapter_test.cpp
    ../main/media/decoder/hifi_decoder_adapter.cpp
    host_stubs/esp_mp3_decoder_backend_host_stub.cpp
    host_stubs/no_abort_dialog.cpp
)
target_include_directories(hifi_decoder_adapter_test PRIVATE ../main)
target_compile_features(hifi_decoder_adapter_test PRIVATE cxx_std_17)
add_test(NAME hifi_decoder_adapter_test COMMAND $<TARGET_FILE:hifi_decoder_adapter_test>)

add_executable(media_mode_transition_test
    media_mode_transition_test.cpp
    ../main/media/media_mode_controller.cpp
    host_stubs/no_abort_dialog.cpp
)
target_include_directories(media_mode_transition_test PRIVATE ../main)
target_compile_features(media_mode_transition_test PRIVATE cxx_std_17)
add_test(NAME media_mode_transition_test COMMAND $<TARGET_FILE:media_mode_transition_test>)

add_executable(pcm_tap_test
    pcm_tap_test.cpp
    ../main/media/audio/pcm_tap.cpp
    host_stubs/no_abort_dialog.cpp
)
target_include_directories(pcm_tap_test PRIVATE ../main)
target_compile_features(pcm_tap_test PRIVATE cxx_std_17)
add_test(NAME pcm_tap_test COMMAND $<TARGET_FILE:pcm_tap_test>)

add_executable(media_audio_session_test
    media_audio_session_test.cpp
    ../main/media/audio/media_audio_session.cpp
    host_stubs/no_abort_dialog.cpp
)
target_include_directories(media_audio_session_test PRIVATE ../main)
target_compile_features(media_audio_session_test PRIVATE cxx_std_17)
add_test(NAME media_audio_session_test COMMAND $<TARGET_FILE:media_audio_session_test>)

add_executable(playback_pump_policy_test
    playback_pump_policy_test.cpp
    host_stubs/no_abort_dialog.cpp
)
target_include_directories(playback_pump_policy_test PRIVATE ../main)
target_compile_features(playback_pump_policy_test PRIVATE cxx_std_17)
add_test(NAME playback_pump_policy_test COMMAND $<TARGET_FILE:playback_pump_policy_test>)

add_executable(byte_ring_test
    byte_ring_test.cpp
    ../main/media/storage/byte_ring.cpp
    host_stubs/no_abort_dialog.cpp
)
target_include_directories(byte_ring_test PRIVATE ../main)
target_compile_features(byte_ring_test PRIVATE cxx_std_17)
add_test(NAME byte_ring_test COMMAND $<TARGET_FILE:byte_ring_test>)

add_executable(prefetching_stream_test
    prefetching_stream_test.cpp
    ../main/media/storage/prefetching_stream.cpp
    ../main/media/storage/byte_ring.cpp
    host_stubs/no_abort_dialog.cpp
)
target_include_directories(prefetching_stream_test PRIVATE ../main)
target_compile_features(prefetching_stream_test PRIVATE cxx_std_17)
add_test(NAME prefetching_stream_test COMMAND $<TARGET_FILE:prefetching_stream_test>)

add_executable(spectrum_analyzer_test
    spectrum_analyzer_test.cpp
    ../main/media/audio/spectrum_analyzer.cpp
    host_stubs/no_abort_dialog.cpp
)
target_include_directories(spectrum_analyzer_test PRIVATE ../main)
target_compile_features(spectrum_analyzer_test PRIVATE cxx_std_17)
add_test(NAME spectrum_analyzer_test COMMAND $<TARGET_FILE:spectrum_analyzer_test>)
