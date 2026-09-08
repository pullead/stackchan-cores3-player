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
