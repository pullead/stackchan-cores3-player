cmake_minimum_required(VERSION 3.16)

project(stackchan_firmware_tests LANGUAGES CXX)

enable_testing()

add_executable(motion_math_test
    motion_math_test.cpp
    ../main/stackchan/motion/motion_math.cpp
)

target_include_directories(motion_math_test PRIVATE
    ../main
)

target_compile_features(motion_math_test PRIVATE cxx_std_17)

add_test(NAME motion_math_test COMMAND $<TARGET_FILE:motion_math_test>)

add_executable(media_volume_policy_test
    media_volume_policy_test.cpp
)

target_include_directories(media_volume_policy_test PRIVATE
    ../main
)

target_compile_features(media_volume_policy_test PRIVATE cxx_std_17)

add_test(NAME media_volume_policy_test COMMAND $<TARGET_FILE:media_volume_policy_test>)

add_executable(media_state_machine_test
    media_state_machine_test.cpp
    ../main/media/media_state_machine.cpp
)

target_include_directories(media_state_machine_test PRIVATE
    ../main
)

target_compile_features(media_state_machine_test PRIVATE cxx_std_17)

add_test(NAME media_state_machine_test COMMAND $<TARGET_FILE:media_state_machine_test>)

add_executable(media_speaker_sink_test
    media_speaker_sink_test.cpp
    ../main/media/audio/core_s3_speaker_sink.cpp
)

target_include_directories(media_speaker_sink_test PRIVATE
    ../main
)

target_compile_features(media_speaker_sink_test PRIVATE cxx_std_17)

add_test(NAME media_speaker_sink_test COMMAND $<TARGET_FILE:media_speaker_sink_test>)

add_executable(wav_reader_test
    wav_reader_test.cpp
    ../main/media/local/wav_reader.cpp
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
)

target_include_directories(sd_card_port_test PRIVATE
    ../main
)

target_compile_features(sd_card_port_test PRIVATE cxx_std_17)

add_test(NAME sd_card_port_test COMMAND $<TARGET_FILE:sd_card_port_test>)

add_executable(spi3_display_handoff_test
    spi3_display_handoff_test.cpp
    ../main/hal/board/spi3_display_handoff.cpp
)

target_include_directories(spi3_display_handoff_test PRIVATE
    ../main
)

target_compile_features(spi3_display_handoff_test PRIVATE cxx_std_17)

add_test(NAME spi3_display_handoff_test COMMAND $<TARGET_FILE:spi3_display_handoff_test>)

add_executable(local_music_presenter_test
    local_music_presenter_test.cpp
    ../main/apps/app_local_music/local_music_presenter.cpp
)

target_include_directories(local_music_presenter_test PRIVATE
    ../main
)

target_compile_features(local_music_presenter_test PRIVATE cxx_std_17)

add_test(NAME local_music_presenter_test COMMAND $<TARGET_FILE:local_music_presenter_test>)

add_executable(sd_card_diagnostic_test
    sd_card_diagnostic_test.cpp
    ../main/media/storage/sd_card_diagnostic.cpp
)

target_include_directories(sd_card_diagnostic_test PRIVATE
    ../main
)

target_compile_features(sd_card_diagnostic_test PRIVATE cxx_std_17)

add_test(NAME sd_card_diagnostic_test COMMAND $<TARGET_FILE:sd_card_diagnostic_test>)

add_executable(aw9523_sd_power_test
    aw9523_sd_power_test.cpp
)

target_include_directories(aw9523_sd_power_test PRIVATE
    ../main
)

target_compile_features(aw9523_sd_power_test PRIVATE cxx_std_17)

add_test(NAME aw9523_sd_power_test COMMAND $<TARGET_FILE:aw9523_sd_power_test>)

add_executable(fatfs_config_test
    fatfs_config_test.cpp
)

target_compile_features(fatfs_config_test PRIVATE cxx_std_23)

add_test(NAME fatfs_config_test
    COMMAND $<TARGET_FILE:fatfs_config_test> ${CMAKE_CURRENT_SOURCE_DIR}/../sdkconfig)

add_executable(audio_stream_contract_test
    audio_stream_contract_test.cpp
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
)
target_include_directories(sd_audio_stream_test PRIVATE ../main)
target_compile_features(sd_audio_stream_test PRIVATE cxx_std_17)
add_test(NAME sd_audio_stream_test COMMAND $<TARGET_FILE:sd_audio_stream_test>)

add_executable(hifi_decoder_adapter_test
    hifi_decoder_adapter_test.cpp
    ../main/media/decoder/hifi_decoder_adapter.cpp
)
target_include_directories(hifi_decoder_adapter_test PRIVATE ../main)
target_compile_features(hifi_decoder_adapter_test PRIVATE cxx_std_17)
add_test(NAME hifi_decoder_adapter_test COMMAND $<TARGET_FILE:hifi_decoder_adapter_test>)
