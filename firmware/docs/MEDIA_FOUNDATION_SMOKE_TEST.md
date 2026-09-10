# Media Foundation Smoke Test

- Baseline firmware commit: 1362fd8be37f5be74f2949007040fbfa69df03bc
- Candidate branch: `codex/media-foundation`
- Build output: `build-shortpath/stack-chan.bin`
- Date and device identifier:
- Flash port:
- Cold boot emitted no intentional sound: pass/fail
- Applied speaker volume after boot is 0%: pass/fail
- Mooncake launcher opens: pass/fail
- Avatar updates: pass/fail
- Yaw and pitch servos respond: pass/fail
- Setup microphone waveform updates: pass/fail
- Setup microphone playback remains inaudible at 0%: pass/fail
- Entering Xiaozhi emitted no intentional sound before user volume change: pass/fail
- Returning to Mooncake retains 0% applied volume: pass/fail
- Free internal heap and largest DMA block before test:
- Free internal heap and largest DMA block after ten minutes:
- Reset, watchdog, or assertion observed: yes/no

## Safety contract

All smoke-test steps keep the applied speaker volume at 0%. Do not intentionally
play a sound or change the volume unless the operator separately authorizes an
audible test.

## Local Music browse checkpoint

This checkpoint is browse-only. It must not select a track, open the speaker
sink, or change the applied speaker volume from 0%.

- Build includes one `LOCAL MUSIC` Mooncake registration: pass
- Host presentation and media-foundation tests: 9/9 pass
- ESP-IDF build output: `build-shortpath/stack-chan.bin`
- Firmware size: `0x396d60` (27% application partition free)
- Flash port and device identifier:
- Launcher shows `LOCAL MUSIC`: pending hardware check
- Opening the app shows `SCANNING SD CARD`: pending hardware check
- No-card state shows an explicit SD error: pending hardware check
- FAT32 card with no root WAV files shows `NO WAV FILES`: pending hardware check
- FAT32 card with root WAV files shows up to eight titles: pending hardware check
- `BACK` returns to the launcher: pending hardware check
- LCD, touch, and launcher remain stable after ten open/close cycles: pending hardware check
- Audible output observed: must remain no
